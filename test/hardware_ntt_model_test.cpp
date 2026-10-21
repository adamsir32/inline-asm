#include "hpu/model/hardware_ntt.hpp"
#include "scheme/bfv/galois.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

constexpr std::uint32_t kModulus = 2013265921U;
constexpr std::uint32_t kPrimitiveRoot = 31U;

std::uint32_t multiply_mod(
    std::uint32_t left,
    std::uint32_t right,
    std::uint32_t modulus)
{
    return static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(left) * right) % modulus);
}

std::size_t bit_reverse(std::size_t value, std::size_t degree)
{
    std::size_t reversed = 0;
    for (std::size_t remaining = degree; remaining > 1; remaining >>= 1U) {
        reversed = (reversed << 1U) | (value & 1U);
        value >>= 1U;
    }
    return reversed;
}

std::uint32_t psi_for(std::size_t degree)
{
    return hpu::model::pow_mod(
        kPrimitiveRoot,
        (static_cast<std::uint64_t>(kModulus) - 1U) / (2 * degree),
        kModulus);
}
std::vector<std::uint32_t> deterministic_input(std::size_t degree)
{
    std::vector<std::uint32_t> input(degree);
    std::uint64_t state = 0x4850555f4e545455ULL;
    for (std::uint32_t& value : input) {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        value = static_cast<std::uint32_t>(state % kModulus);
    }
    return input;
}

// Independent iterative Cooley-Tukey oracle. Unlike the hardware model, this
// performs an explicit input bit reversal and has no loader or P network.
std::vector<std::uint32_t> mathematical_ntt(
    const std::vector<std::uint32_t>& input,
    std::uint32_t omega)
{
    const std::size_t degree = input.size();
    std::vector<std::uint32_t> values(degree);
    for (std::size_t index = 0; index < degree; ++index) {
        values[bit_reverse(index, degree)] = input[index];
    }
    for (std::size_t length = 2; length <= degree; length <<= 1U) {
        const std::size_t half = length / 2;
        const std::uint32_t step = hpu::model::pow_mod(
            omega, degree / length, kModulus);
        for (std::size_t base = 0; base < degree; base += length) {
            std::uint32_t twiddle = 1;
            for (std::size_t offset = 0; offset < half; ++offset) {
                const std::uint32_t even = values[base + offset];
                const std::uint32_t odd = multiply_mod(
                    values[base + offset + half], twiddle, kModulus);
                const std::uint64_t sum = static_cast<std::uint64_t>(even) + odd;
                values[base + offset] = static_cast<std::uint32_t>(
                    sum >= kModulus ? sum - kModulus : sum);
                values[base + offset + half] = even >= odd
                    ? even - odd
                    : static_cast<std::uint32_t>(
                        static_cast<std::uint64_t>(even) + kModulus - odd);
                twiddle = multiply_mod(twiddle, step, kModulus);
            }
        }
    }
    return values;
}

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_degree(std::size_t degree)
{
    const std::uint32_t psi = psi_for(degree);
    const std::uint32_t omega = hpu::model::pow_mod(psi, 2, kModulus);
    hpu::model::HardwareNttModel model(degree, kModulus, omega);
    const auto input = deterministic_input(degree);

    const auto physical = model.forward(input);
    require(model.inverse(physical) == input, "cyclic HPU NTT round-trip failed");
    const auto forward_tables = model.forward_twiddles();
    const auto supplied_inverse_tables = model.inverse_twiddles();
    require(
        model.forward(input, forward_tables) == physical
            && model.inverse(physical, supplied_inverse_tables) == input,
        "HPU NTT did not consume supplied hardware tables correctly");
    auto perturbed_forward_tables = forward_tables;
    for (auto& value : perturbed_forward_tables[0]) {
        value = (value + 1U) % kModulus;
    }
    require(
        model.forward(input, perturbed_forward_tables) != physical,
        "HPU forward transform ignored the supplied twiddle payload");
    auto perturbed_inverse_tables = supplied_inverse_tables;
    for (auto& value : perturbed_inverse_tables.post_scale) {
        value = (value + 1U) % kModulus;
    }
    require(
        model.inverse(physical, perturbed_inverse_tables) != input,
        "HPU inverse transform ignored the supplied post-scale payload");

    const auto layout = model.forward_layout();
    std::vector<std::uint32_t> logical_ntt(degree);
    for (std::size_t position = 0; position < degree; ++position) {
        logical_ntt[layout[position]] = physical[position];
    }
    require(
        logical_ntt == mathematical_ntt(input, omega),
        "HPU forward transform disagrees with mathematical NTT");

    const auto negacyclic = hpu::model::negacyclic_forward(input, kModulus, psi);
    require(
        hpu::model::negacyclic_inverse(negacyclic, kModulus, psi) == input,
        "negacyclic HPU NTT round-trip failed");

    std::vector<bool> seen(degree, false);
    for (std::size_t logical : layout) {
        require(logical < degree && !seen[logical], "forward layout is not a permutation");
        seen[logical] = true;
    }

    const auto inverse_tables = supplied_inverse_tables;
    require(inverse_tables.stages.size() == model.log_degree(), "wrong inverse stage count");
    require(inverse_tables.post_scale.size() == degree, "wrong inverse post-scale size");
}

void test_delta_basis()
{
    constexpr std::size_t degree = 128;
    const std::uint32_t psi = psi_for(degree);
    const std::uint32_t omega = hpu::model::pow_mod(psi, 2, kModulus);
    hpu::model::HardwareNttModel model(degree, kModulus, omega);
    std::vector<std::uint32_t> delta(degree, 0);
    delta[1] = 1;
    const auto physical = model.forward(delta);
    const auto layout = model.forward_layout();
    std::vector<std::uint32_t> logical(degree);
    for (std::size_t position = 0; position < degree; ++position) {
        logical[layout[position]] = physical[position];
    }
    for (std::size_t frequency = 0; frequency < degree; ++frequency) {
        require(
            logical[frequency] == hpu::model::pow_mod(omega, frequency, kModulus),
            "forward(delta_1) is not omega^k");
    }
}

void test_fused_automorphism(std::size_t degree, std::uint64_t galois_element)
{
    const std::uint32_t psi = psi_for(degree);
    const auto input = deterministic_input(degree);
    const auto canonical_ntt = hpu::model::negacyclic_forward(input, kModulus, psi);
    const auto expected_coefficients = hpu::model::automorphism_coefficients(
        input, galois_element, kModulus);
    const auto expected_ntt = hpu::model::negacyclic_forward(
        expected_coefficients, kModulus, psi);

    require(
        hpu::model::automorphism_fused_inverse(
            canonical_ntt, galois_element, kModulus, psi) == expected_ntt,
        "INTT-fused automorphism disagrees with coefficient reference");
    require(
        hpu::model::automorphism_fused_forward(
            canonical_ntt, galois_element, kModulus, psi) == expected_ntt,
        "NTT-fused automorphism disagrees with coefficient reference");
}

template <typename Function>
void require_invalid(Function&& function, const char* message)
{
    try { function(); }
    catch (const std::invalid_argument&) { return; }
    throw std::runtime_error(message);
}

std::uint32_t parameter_test_root(std::size_t degree, std::uint32_t modulus)
{
    for (std::uint32_t base = 2; base < 128; ++base) {
        const auto psi = hpu::model::pow_mod(base,
            (static_cast<std::uint64_t>(modulus) - 1) / (2 * degree), modulus);
        if (hpu::model::pow_mod(psi, degree, modulus) == modulus - 1) return psi;
    }
    throw std::runtime_error("test modulus has no selected primitive root");
}

void test_stage_parameters(std::size_t degree, std::uint32_t modulus, std::uint32_t root_power)
{
    const auto psi = parameter_test_root(degree, modulus);
    const auto omega = hpu::model::pow_mod(psi, 2 * root_power, modulus);
    hpu::model::HardwareNttModel model(degree, modulus, omega);
    const auto forward = model.forward_twiddles();
    const auto inverse = model.inverse_twiddles();
    std::vector<std::uint32_t> data(degree);
    for (std::size_t i = 0; i < degree; ++i)
        data[i] = static_cast<std::uint32_t>((std::uint64_t(i) * i + 17 * i + 1) % modulus);
    for (bool is_inverse : {false, true}) {
        const auto& stages = is_inverse ? inverse.stages : forward;
        for (std::size_t stage = 0; stage < stages.size(); ++stage) {
            const auto packed = hpu::model::pack_stage_twiddles(stages[stage], modulus, degree);
            require(packed.size() == 128 && hpu::model::kStageTwiddleParameterLines == 2
                        && hpu::model::kTwiddleChains == 8,
                    "stage parameters do not use the fixed two-line K=8 ABI");
            require(std::equal(packed.begin(), packed.begin() + 64, stages[stage].begin()),
                    "stage parameter line zero is not the first physical batch");
            const auto expanded = hpu::model::expand_stage_twiddles(packed, modulus, degree);
            require(expanded == stages[stage], "K=8 parameters differ from the complete physical oracle");
            require(hpu::model::execute_ntt_stage(data, expanded, modulus, stage, is_inverse)
                        == hpu::model::execute_ntt_stage(data, stages[stage], modulus, stage, is_inverse),
                    "K=8 parameter expansion changed a stage's BF result");
            const auto batches = degree / 128;
            for (std::size_t slot = 1; slot < std::min<std::size_t>(8, batches); ++slot)
                require(packed[64 + slot - 1] == stages[stage][64 * slot], "V is not stored at fixed slot-1 offset");
            for (std::size_t slot = std::min<std::size_t>(8, batches); slot < 8; ++slot)
                require(packed[64 + slot - 1] == 1, "unused V was not filled with one");
            std::size_t ratio_count = 0;
            for (auto b = batches; b > 8; b >>= 1U) ++ratio_count;
            for (std::size_t word = 71 + ratio_count; word < 128; ++word)
                require(packed[word] == 1, "unused R/padding was not filled with one");
        }
    }
}

void test_stage_parameter_validation()
{
    constexpr std::size_t degree = 4096;
    const auto omega = hpu::model::pow_mod(psi_for(degree), 2, kModulus);
    hpu::model::HardwareNttModel model(degree, kModulus, omega);
    const auto full = model.forward_twiddles().back();
    const auto packed = hpu::model::pack_stage_twiddles(full, kModulus, degree);
    const auto change = [](std::uint32_t value) { return value == kModulus - 1 ? 1U : value + 1; };
    auto irregular = full;
    irregular[9 * 64 + 17] = change(irregular[9 * 64 + 17]);
    require_invalid([&] { hpu::model::pack_stage_twiddles(irregular, kModulus, degree); },
                    "packing skipped mandatory full reconstruction validation");
    auto invalid = full;
    invalid[0] = 2;
    require_invalid([&] { hpu::model::pack_stage_twiddles(invalid, kModulus, degree); }, "packing accepted T[0]!=1");
    invalid = full; invalid[17] = 0;
    require_invalid([&] { hpu::model::pack_stage_twiddles(invalid, kModulus, degree); }, "packing accepted a zero root");
    invalid[17] = kModulus;
    require_invalid([&] { hpu::model::pack_stage_twiddles(invalid, kModulus, degree); }, "packing accepted a noncanonical root");
    invalid.pop_back();
    require_invalid([&] { hpu::model::pack_stage_twiddles(invalid, kModulus, degree); }, "packing accepted wrong table size");
    require_invalid([&] { hpu::model::pack_stage_twiddles(full, kModulus, 192); }, "packing accepted unsupported degree");
    require_invalid([&] { hpu::model::expand_stage_twiddles(packed, 1, degree); }, "expansion accepted invalid modulus");
    auto bad_parameters = packed;
    bad_parameters.pop_back();
    require_invalid([&] { hpu::model::expand_stage_twiddles(bad_parameters, kModulus, degree); }, "expansion accepted non-two-line parameters");
    bad_parameters = packed; bad_parameters[127] = 2;
    require_invalid([&] { hpu::model::expand_stage_twiddles(bad_parameters, kModulus, degree); }, "expansion ignored non-one padding");
    bad_parameters = packed; bad_parameters[1] = 0;
    require_invalid([&] { hpu::model::expand_stage_twiddles(bad_parameters, kModulus, degree); }, "expansion accepted a zero parameter");
    bad_parameters[1] = kModulus;
    require_invalid([&] { hpu::model::expand_stage_twiddles(bad_parameters, kModulus, degree); }, "expansion accepted a noncanonical parameter");
    auto small = hpu::model::pack_stage_twiddles(std::vector<std::uint32_t>(64, 1), kModulus, 128);
    small[64] = 2;
    require_invalid([&] { hpu::model::expand_stage_twiddles(small, kModulus, 128); }, "small N accepted an unused V parameter");
    std::vector<std::uint32_t> data(degree, 1);
    const auto expected = hpu::model::execute_ntt_stage(data, full, kModulus, 11);
    for (const auto word : {std::size_t{1}, std::size_t{64}, std::size_t{71}}) {
        auto changed = packed; changed[word] = change(changed[word]);
        const auto expanded = hpu::model::expand_stage_twiddles(changed, kModulus, degree);
        require(expanded != full
                    && hpu::model::execute_ntt_stage(data, expanded, kModulus, 11) != expected,
                "stage execution ignored a supplied U/V/R parameter");
    }
}

void test_hardware_migration_checkpoint()
{
    // Hardware team's N=4096/q=50061313 IT fixture, migration section 7.1.
    // V[2] exposes omega for this loader, fixing the selected primitive root.
    constexpr std::uint32_t modulus = 50061313U;
    hpu::model::HardwareNttModel model(4096, modulus, 3474171U);
    const auto stages = model.forward_twiddles();
    const auto first = hpu::model::pack_stage_twiddles(stages.front(), modulus, 4096);
    require(first == std::vector<std::uint32_t>(128, 1), "IT stage zero is not the all-one parameter block");
    const auto last = hpu::model::pack_stage_twiddles(stages.back(), modulus, 4096);
    const std::vector<std::uint32_t> expected_u{
        1, 2564634, 41945451, 26788067, 15836256, 8007847, 43637878, 16593746};
    const std::vector<std::uint32_t> expected_meta{
        44649554, 3474171, 962682, 31511628, 25687718, 1581086, 23227686,
        35622094, 35622094};
    require(std::equal(expected_u.begin(), expected_u.end(), last.begin())
                && last[63] == 37489420
                && std::equal(expected_meta.begin(), expected_meta.end(), last.begin() + 64)
                && std::all_of(last.begin() + 73, last.end(), [](auto x) { return x == 1; }),
            "IT stage eleven differs from hardware migration checkpoints");
}

} // namespace

int main()
{
    try {
        test_degree(128);
        test_degree(2048);
        test_degree(65536);
        test_delta_basis();
        test_fused_automorphism(128, 3);
        test_fused_automorphism(
            128, hpu::scheme::bfv::row_rotation_galois_element(128, -2));
        test_fused_automorphism(
            128, hpu::scheme::bfv::column_rotation_galois_element(128));
        test_fused_automorphism(65536, 3);
        test_fused_automorphism(
            65536, hpu::scheme::bfv::column_rotation_galois_element(65536));
        // B<8, B=8, and B>8, including every stride-8 carry and unsigned q.
        test_stage_parameters(128, kModulus, 1);
        test_stage_parameters(256, 998244353U, 3);
        test_stage_parameters(1024, 4293918721U, 1);
        test_stage_parameters(4096, kModulus, 3);
        test_stage_parameters(65536, 4293918721U, 5);
        test_stage_parameter_validation();
        test_hardware_migration_checkpoint();
        std::cout << "HPU NTT model tests passed, including N=65536 and fused automorphism\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "HPU NTT model test failed: " << error.what() << '\n';
        return 1;
    }
}
