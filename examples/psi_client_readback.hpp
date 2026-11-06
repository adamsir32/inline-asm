#pragma once
#include "hpu/delivery/application_package.hpp"
#include <seal/seal.h>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace psi_client_readback {
inline void require(bool ok, const char *message) {
    if (!ok) throw std::runtime_error(message);
}
inline std::uint32_t u32(const unsigned char *p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
inline std::vector<std::uint32_t> read_image(
    const std::filesystem::path& path, std::size_t expected_words) {
    require(std::filesystem::is_regular_file(path) &&
            std::filesystem::file_size(path) == expected_words * 4 + 64 * 256,
            "PSI C readback image size mismatch");
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "cannot open PSI C readback image");
    std::vector<std::uint32_t> words(expected_words);
    for (auto& word : words) {
        unsigned char bytes[4];
        input.read(reinterpret_cast<char*>(bytes), 4);
        require(static_cast<bool>(input), "truncated PSI C readback image");
        word = u32(bytes);
    }
    std::vector<unsigned char> guard(64 * 256);
    input.read(reinterpret_cast<char*>(guard.data()), guard.size());
    require(static_cast<bool>(input), "PSI C readback guard is truncated");
    return words;
}
inline std::size_t reverse(std::size_t value, std::size_t degree) {
    std::size_t result = 0;
    for (std::size_t span = degree; span > 1; span >>= 1) {
        result = (result << 1) | (value & 1);
        value >>= 1;
    }
    return result;
}
inline ::seal::Ciphertext reconstruct(
    const ::seal::Ciphertext& oracle,
    const hpu::delivery::ApplicationPackageRequest& package,
    const std::string& object_id, const std::vector<std::uint32_t>& words,
    const ::seal::SEALContext& context) {
    const auto level = context.get_context_data(oracle.parms_id());
    require(static_cast<bool>(level), "PSI C readback has unknown SEAL level");
    const auto degree = oracle.poly_modulus_degree();
    const auto& moduli = level->parms().coeff_modulus();
    ::seal::Ciphertext result = oracle;
    std::fill_n(result.data(), result.size() * result.coeff_modulus_size() * degree,
                std::uint64_t{0});
    std::vector<bool> covered(result.size() * moduli.size(), false);
    for (const auto& limb : package.outputs) {
        if (limb.object_id != object_id) continue;
        require(limb.domain == hpu::delivery::OutputDomain::coefficient_bit_reversed &&
                limb.word_count == degree && limb.component < result.size() &&
                limb.modulus_id < moduli.size() &&
                limb.modulus == moduli[limb.modulus_id].value(),
                "PSI C readback output metadata mismatch");
        const auto key = limb.component * moduli.size() + limb.modulus_id;
        require(!covered[key], "PSI C readback duplicate output limb");
        covered[key] = true;
        const auto offset = limb.destination.line_offset * 64;
        require(offset <= words.size() && degree <= words.size() - offset,
                "PSI C readback output exceeds image");
        for (std::size_t physical = 0; physical < degree; ++physical) {
            const auto value = words[offset + physical];
            require(value < limb.modulus, "PSI C readback noncanonical coefficient");
            result.data(limb.component)[limb.modulus_id * degree +
                reverse(physical, degree)] = value;
        }
    }
    require(std::all_of(covered.begin(), covered.end(), [](bool seen) { return seen; }),
            "PSI C readback missing output limb");
    for (std::size_t component = 0; component < result.size(); ++component)
        for (std::size_t word = 0; word < moduli.size() * degree; ++word)
            require(result.data(component)[word] == oracle.data(component)[word],
                    "PSI C readback differs from independent SEAL ciphertext");
    return result;
}
} // namespace psi_client_readback
