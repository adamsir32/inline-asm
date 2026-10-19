#include "evaluator_application.hpp"

#include "hpu/seal/bfv_context.hpp"

#include <seal/seal.h>
#include <seal/util/numth.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t kDegree = 4096;
constexpr std::size_t kElementBytes = 1024;
constexpr std::uint32_t kUsablePlainBits = 20;
constexpr std::size_t kDimensions = 2;
constexpr std::uint64_t kDatabaseSeed = 0x5345414c50495232ULL;

struct Profile {
    std::string name;
    std::size_t element_count;
    std::size_t default_target;
    std::size_t dot_shard_outputs;
};

struct Options {
    Profile profile;
    std::size_t target = 0;
    evaluator_example::Options evaluator;
};

struct StageStats {
    std::string name;
    std::string directory;
    std::size_t graph_nodes = 0;
    std::size_t instructions = 0;
    std::size_t dma_spans = 0;
    std::uint64_t memory_lines = 0;
    bool instruction_verified = false;
};

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

std::size_t parse_size(const std::string& text, const char* option)
{
    std::size_t used = 0;
    const auto value = std::stoull(text, &used);
    if (used != text.size() || value > std::numeric_limits<std::size_t>::max())
        throw std::invalid_argument(std::string(option) + " has an invalid integer value");
    return static_cast<std::size_t>(value);
}

Options parse_options(int argc, char** argv)
{
    Profile profile{"small", 128, 0, 4};
    bool target_set = false;
    std::size_t target = 0;
    std::size_t requested_shard_outputs = 0;
    std::vector<char*> remaining{argv[0]};
    for (int i = 1; i < argc; ++i) {
        const std::string argument(argv[i]);
        if (argument == "--profile") {
            if (++i >= argc) throw std::invalid_argument("--profile requires small or full");
            const std::string value(argv[i]);
            if (value == "small") profile = {"small", 128, 0, 4};
            else if (value == "full") profile = {"full", 65536, 65535, 1};
            else throw std::invalid_argument("--profile must be small or full");
        } else if (argument == "--record-index") {
            if (++i >= argc) throw std::invalid_argument("--record-index requires a value");
            target = parse_size(argv[i], "--record-index");
            target_set = true;
        } else if (argument == "--dot-shard-outputs") {
            if (++i >= argc) throw std::invalid_argument("--dot-shard-outputs requires a value");
            requested_shard_outputs = parse_size(argv[i], "--dot-shard-outputs");
            if (!requested_shard_outputs)
                throw std::invalid_argument("--dot-shard-outputs must be positive");
        } else {
            remaining.push_back(argv[i]);
        }
    }
    if (requested_shard_outputs) profile.dot_shard_outputs = requested_shard_outputs;
    if (!target_set) target = profile.default_target;
    if (target >= profile.element_count)
        throw std::invalid_argument("--record-index is outside the selected profile");
    return {profile, target,
            evaluator_example::options(static_cast<int>(remaining.size()), remaining.data())};
}

std::size_t integer_log2_ceil(std::size_t value)
{
    std::size_t result = 0;
    for (std::size_t power = 1; power < value; power <<= 1) ++result;
    return result;
}

std::vector<std::size_t> dimensions(std::size_t plaintext_count)
{
    const auto root = std::max<std::size_t>(
        2, static_cast<std::size_t>(std::floor(std::pow(plaintext_count, 0.5))));
    std::vector<std::size_t> result(kDimensions, root);
    for (std::size_t i = 0; i < result.size(); ++i) {
        if (std::accumulate(result.begin(), result.end(), std::size_t{1},
                            std::multiplies<std::size_t>()) > plaintext_count)
            break;
        ++result[i];
    }
    return result;
}

std::vector<std::size_t> compute_indices(
    std::size_t desired, const std::vector<std::size_t>& shape)
{
    auto product = std::accumulate(shape.begin(), shape.end(), std::size_t{1},
                                   std::multiplies<std::size_t>());
    std::vector<std::size_t> result;
    result.reserve(shape.size());
    for (const auto extent : shape) {
        product /= extent;
        result.push_back(desired / product);
        desired %= product;
    }
    return result;
}

std::uint64_t splitmix64(std::uint64_t value)
{
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

std::vector<std::uint8_t> database_element(std::size_t index)
{
    std::vector<std::uint8_t> result(kElementBytes);
    for (std::size_t offset = 0; offset < result.size(); ++offset) {
        const auto word = splitmix64(kDatabaseSeed
            ^ (static_cast<std::uint64_t>(index) << 32)
            ^ static_cast<std::uint64_t>(offset / 8));
        result[offset] = static_cast<std::uint8_t>(word >> (8 * (offset % 8)));
    }
    return result;
}

std::size_t coefficients_per_element()
{
    return (kElementBytes * 8 + kUsablePlainBits - 1) / kUsablePlainBits;
}

std::vector<std::uint64_t> bytes_to_coefficients(const std::vector<std::uint8_t>& bytes)
{
    std::vector<std::uint64_t> output(coefficients_per_element());
    std::uint32_t room = kUsablePlainBits;
    std::size_t target = 0;
    for (std::uint8_t byte : bytes) {
        std::uint8_t source = byte;
        std::uint32_t remaining = 8;
        while (remaining) {
            if (!room) { ++target; room = kUsablePlainBits; }
            const auto take = std::min(room, remaining);
            output[target] = (output[target] << take) | (source >> (8 - take));
            source = static_cast<std::uint8_t>(source << take);
            room -= take;
            remaining -= take;
        }
    }
    output[target] <<= room;
    return output;
}

std::vector<std::uint8_t> coefficients_to_bytes(
    const std::vector<std::uint64_t>& coefficients)
{
    std::vector<std::uint8_t> output(kElementBytes);
    std::uint32_t room = 8;
    std::size_t target = 0;
    std::uint32_t bits_left = static_cast<std::uint32_t>(kElementBytes * 8);
    for (auto source : coefficients) {
        std::uint32_t remaining = std::min(kUsablePlainBits, bits_left);
        while (remaining && target < output.size()) {
            const auto take = std::min(room, remaining);
            output[target] = static_cast<std::uint8_t>(
                (output[target] << take) | (source >> (kUsablePlainBits - take)));
            source <<= take;
            room -= take;
            remaining -= take;
            bits_left -= take;
            if (!room) { ++target; room = 8; }
        }
    }
    return output;
}

std::vector<::seal::Plaintext> build_database(
    const ::seal::SEALContext& context, std::size_t element_count,
    const std::vector<std::size_t>& shape)
{
    ::seal::BatchEncoder encoder(context);
    const auto per_plain = kDegree / coefficients_per_element();
    const auto used_plaintexts = (element_count + per_plain - 1) / per_plain;
    const auto matrix_plaintexts = std::accumulate(
        shape.begin(), shape.end(), std::size_t{1}, std::multiplies<std::size_t>());
    std::vector<::seal::Plaintext> result;
    result.reserve(matrix_plaintexts);
    for (std::size_t plain_index = 0; plain_index < used_plaintexts; ++plain_index) {
        std::vector<std::uint64_t> coefficients(
            per_plain * coefficients_per_element());
        for (std::size_t local = 0; local < per_plain; ++local) {
            const auto element = plain_index * per_plain + local;
            if (element >= element_count) break;
            const auto packed = bytes_to_coefficients(database_element(element));
            std::copy(packed.begin(), packed.end(),
                      coefficients.begin() + local * coefficients_per_element());
        }
        coefficients.resize(kDegree, 1);
        ::seal::Plaintext plain;
        encoder.encode(coefficients, plain);
        result.push_back(std::move(plain));
    }
    while (result.size() < matrix_plaintexts) {
        ::seal::Plaintext padding(kDegree);
        std::fill_n(padding.data(), kDegree, std::uint64_t{1});
        result.push_back(std::move(padding));
    }
    return result;
}

::seal::Plaintext monomial(std::uint32_t exponent)
{
    ::seal::Plaintext result(kDegree);
    result.set_zero();
    result[exponent % kDegree] = 1;
    return result;
}

StageStats package_stats(const std::string& name, const std::string& directory,
                         const hpu::seal_adapter::CompiledApplication& compiled)
{
    const auto& package = compiled.package();
    return {name, directory, compiled.node_count(), package.program.instructions.size(),
            package.program.ordered_dma_spans.size(), package.initial_image->used_lines(),
            compiled.instruction_verified()};
}

void maybe_write(const Options& options, const std::string& directory,
                 const hpu::seal_adapter::CompiledApplication& compiled)
{
    if (options.evaluator.delivery.emit_directory)
        compiled.write(*options.evaluator.delivery.emit_directory / directory);
    if (options.evaluator.delivery.print_program)
        std::cout << "\n# " << directory << "\n" << compiled.package().program.assembly;
}

std::vector<::seal::Ciphertext> expand_query_hpu(
    const Options& options, const ::seal::SEALContext& context,
    const ::seal::Ciphertext& encrypted, std::size_t count,
    const ::seal::GaloisKeys& galois_keys, const std::string& stage,
    std::vector<StageStats>& stats)
{
    const auto logm = integer_log2_ceil(count);
    hpu::seal_adapter::ApplicationBuilder app(context);
    auto evaluator = app.evaluator();
    std::vector<hpu::seal_adapter::Ciphertext> expanded{
        app.import_ciphertext("input/compressed_query", encrypted)};
    ::seal::Plaintext native_two("2");
    const auto two = app.import_plaintext("constant/two", native_two);
    std::map<std::uint32_t, hpu::seal_adapter::Plaintext> monomials;
    auto monomial_handle = [&](std::uint32_t exponent)
        -> const hpu::seal_adapter::Plaintext& {
        exponent %= static_cast<std::uint32_t>(kDegree);
        auto found = monomials.find(exponent);
        if (found == monomials.end()) {
            found = monomials.emplace(exponent, app.import_plaintext(
                "constant/x_" + std::to_string(exponent), monomial(exponent))).first;
        }
        return found->second;
    };
    auto multiply_power_of_x = [&](const hpu::seal_adapter::Ciphertext& input,
                                   std::uint32_t exponent) {
        exponent %= static_cast<std::uint32_t>(2 * kDegree);
        hpu::seal_adapter::Ciphertext output;
        evaluator.multiply_plain(input, monomial_handle(exponent), output);
        if (exponent >= kDegree) evaluator.negate_inplace(output);
        return output;
    };

    for (std::size_t level = 0; level < logm; ++level) {
        const auto old_size = expanded.size();
        std::vector<hpu::seal_adapter::Ciphertext> next(old_size * 2);
        const auto element = static_cast<std::uint32_t>(
            kDegree / (std::size_t{1} << level) + 1);
        const auto raw = static_cast<std::uint32_t>(
            2 * kDegree - (std::size_t{1} << level));
        const auto shifted = static_cast<std::uint32_t>(
            (static_cast<std::uint64_t>(raw) * element) % (2 * kDegree));
        for (std::size_t index = 0; index < old_size; ++index) {
            const bool corner = level + 1 == logm
                && index >= count - (std::size_t{1} << (logm - 1));
            if (corner) {
                evaluator.multiply_plain(expanded[index], two, next[index]);
                continue;
            }
            hpu::seal_adapter::Ciphertext rotated;
            evaluator.apply_galois(expanded[index], element, galois_keys, rotated);
            evaluator.add(expanded[index], rotated, next[index]);
            auto left = multiply_power_of_x(expanded[index], raw);
            auto right = multiply_power_of_x(rotated, shifted);
            evaluator.add(left, right, next[index + old_size]);
        }
        expanded = std::move(next);
    }
    expanded.resize(count);
    for (std::size_t i = 0; i < expanded.size(); ++i)
        app.mark_output("expanded_" + std::to_string(i), expanded[i]);
    const auto stem = "bfv_sealpir_2d_" + options.profile.name + '_' + stage;
    const auto compiled = app.compile(stem, options.evaluator.compiler);
    const auto directory = stage + "_expand";
    maybe_write(options, directory, compiled);
    stats.push_back(package_stats(stage + " query expansion", directory, compiled));
    std::vector<::seal::Ciphertext> actual;
    actual.reserve(expanded.size());
    for (const auto& value : expanded)
        actual.push_back(compiled.software_result(value));
    return actual;
}

std::vector<::seal::Ciphertext> dot_product_hpu(
    const Options& options, const ::seal::SEALContext& context,
    const std::vector<::seal::Ciphertext>& expanded,
    const std::vector<::seal::Plaintext>& database, std::size_t output_count,
    bool mod_switch, const std::string& stage, std::vector<StageStats>& stats)
{
    require(database.size() == expanded.size() * output_count,
            stage + " matrix shape is inconsistent");
    std::vector<::seal::Ciphertext> actual(output_count);
    std::size_t shard_number = 0;
    for (std::size_t begin = 0; begin < output_count;
         begin += options.profile.dot_shard_outputs, ++shard_number) {
        const auto end = std::min(output_count, begin + options.profile.dot_shard_outputs);
        hpu::seal_adapter::ApplicationBuilder app(context);
        auto evaluator = app.evaluator();
        std::vector<hpu::seal_adapter::Ciphertext> query;
        query.reserve(expanded.size());
        for (std::size_t j = 0; j < expanded.size(); ++j)
            query.push_back(app.import_ciphertext(
                "input/query_" + std::to_string(j), expanded[j]));
        std::vector<hpu::seal_adapter::Ciphertext> outputs;
        outputs.reserve(end - begin);
        for (std::size_t k = begin; k < end; ++k) {
            const auto first_plain = app.import_plaintext(
                "database/output_" + std::to_string(k) + "_term_0", database[k]);
            hpu::seal_adapter::Ciphertext sum;
            evaluator.multiply_plain(query[0], first_plain, sum);
            for (std::size_t j = 1; j < query.size(); ++j) {
                const auto plain = app.import_plaintext(
                    "database/output_" + std::to_string(k) + "_term_" + std::to_string(j),
                    database[k + j * output_count]);
                hpu::seal_adapter::Ciphertext term;
                evaluator.multiply_plain(query[j], plain, term);
                evaluator.add_inplace(sum, term);
            }
            if (mod_switch) evaluator.mod_switch_to_next_inplace(sum);
            app.mark_output("reply_" + std::to_string(k), sum);
            outputs.push_back(sum);
        }
        std::ostringstream suffix;
        suffix << stage << "_dot_shard_" << std::setw(3) << std::setfill('0') << shard_number;
        const auto directory = suffix.str();
        const auto compiled = app.compile(
            "bfv_sealpir_2d_" + options.profile.name + '_' + directory,
            options.evaluator.compiler);
        maybe_write(options, directory, compiled);
        stats.push_back(package_stats(stage + " dot-product shard", directory, compiled));
        for (std::size_t local = 0; local < outputs.size(); ++local)
            actual[begin + local] = compiled.software_result(outputs[local]);
    }
    return actual;
}

std::size_t expansion_ratio(const ::seal::EncryptionParameters& parameters)
{
    const auto plain_bits = static_cast<std::uint32_t>(
        std::floor(std::log2(parameters.plain_modulus().value())));
    std::size_t result = 0;
    for (const auto& modulus : parameters.coeff_modulus())
        result += static_cast<std::size_t>(std::ceil(std::log2(modulus.value()) / plain_bits));
    return result;
}

std::vector<::seal::Plaintext> decompose_to_plaintexts(
    const ::seal::EncryptionParameters& parameters, const ::seal::Ciphertext& ciphertext)
{
    const auto plain_bits = static_cast<std::uint32_t>(
        std::floor(std::log2(parameters.plain_modulus().value())));
    const auto degree = parameters.poly_modulus_degree();
    const auto mask = (std::uint64_t{1} << plain_bits) - 1;
    std::vector<::seal::Plaintext> result;
    result.reserve(expansion_ratio(parameters) * ciphertext.size());
    for (std::size_t component = 0; component < ciphertext.size(); ++component) {
        for (std::size_t limb = 0; limb < parameters.coeff_modulus().size(); ++limb) {
            const auto local_ratio = static_cast<std::size_t>(std::ceil(
                std::log2(parameters.coeff_modulus()[limb].value()) / plain_bits));
            for (std::size_t chunk = 0; chunk < local_ratio; ++chunk) {
                ::seal::Plaintext plain(degree);
                const auto shift = static_cast<std::uint32_t>(chunk * plain_bits);
                for (std::size_t coefficient = 0; coefficient < degree; ++coefficient)
                    plain[coefficient] =
                        (ciphertext.data(component)[limb * degree + coefficient] >> shift) & mask;
                result.push_back(std::move(plain));
            }
        }
    }
    return result;
}

::seal::Ciphertext compose_to_ciphertext(
    const ::seal::SEALContext& context, const ::seal::EncryptionParameters& parameters,
    const ::seal::parms_id_type& parms_id,
    std::vector<::seal::Plaintext>::const_iterator begin, std::size_t components)
{
    const auto plain_bits = static_cast<std::uint32_t>(
        std::floor(std::log2(parameters.plain_modulus().value())));
    const auto degree = parameters.poly_modulus_degree();
    ::seal::Ciphertext result;
    result.resize(context, parms_id, components);
    result.is_ntt_form() = false;
    result.scale() = 1.0;
    result.correction_factor() = 1;
    auto iterator = begin;
    for (std::size_t component = 0; component < components; ++component) {
        for (std::size_t limb = 0; limb < parameters.coeff_modulus().size(); ++limb) {
            const auto local_ratio = static_cast<std::size_t>(std::ceil(
                std::log2(parameters.coeff_modulus()[limb].value()) / plain_bits));
            for (std::size_t chunk = 0; chunk < local_ratio; ++chunk, ++iterator) {
                const auto shift = static_cast<std::uint32_t>(chunk * plain_bits);
                for (std::size_t coefficient = 0; coefficient < degree; ++coefficient)
                    result.data(component)[limb * degree + coefficient] |=
                        ((*iterator)[coefficient] << shift);
            }
        }
    }
    return result;
}

::seal::Plaintext decode_recursive_reply(
    const ::seal::SEALContext& context, const ::seal::SecretKey& secret_key,
    std::vector<::seal::Ciphertext> reply)
{
    require(!reply.empty(), "recursive reply is empty");
    const auto recursive_parms_id = reply.front().parms_id();
    const auto recursive_context = context.get_context_data(recursive_parms_id);
    require(static_cast<bool>(recursive_context),
            "recursive reply has an unknown modulus level");
    const auto& parameters = recursive_context->parms();
    const auto ratio = expansion_ratio(parameters);
    ::seal::Decryptor decryptor(context, secret_key);
    for (std::size_t level = 0; level < kDimensions; ++level) {
        std::vector<::seal::Plaintext> plains;
        plains.reserve(reply.size());
        for (const auto& ciphertext : reply) {
            ::seal::Plaintext plain;
            decryptor.decrypt(ciphertext, plain);
            plains.push_back(std::move(plain));
        }
        if (level + 1 == kDimensions) {
            require(reply.size() == 1 && plains.size() == 1,
                    "final recursive reply did not collapse to one plaintext");
            return plains.front();
        }
        const auto components = reply.front().size();
        const auto group = ratio * components;
        require(plains.size() % group == 0,
                "recursive reply plaintext count is not composable");
        std::vector<::seal::Ciphertext> combined;
        for (std::size_t i = 0; i < plains.size(); i += group)
            combined.push_back(compose_to_ciphertext(
                context, parameters, recursive_parms_id,
                plains.begin() + static_cast<std::ptrdiff_t>(i), components));
        reply = std::move(combined);
    }
    throw std::logic_error("unreachable recursive decode state");
}

int minimum_noise_budget(const ::seal::SEALContext& context,
                         const ::seal::SecretKey& secret_key,
                         const std::vector<::seal::Ciphertext>& values)
{
    ::seal::Decryptor decryptor(context, secret_key);
    int result = std::numeric_limits<int>::max();
    for (const auto& value : values)
        result = std::min(result, decryptor.invariant_noise_budget(value));
    return result;
}

void write_bytes(const std::filesystem::path& path,
                 const std::vector<std::uint8_t>& bytes)
{
    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("failed to create " + path.string());
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("failed to write " + path.string());
}

void write_manifest(const Options& options, const std::vector<std::size_t>& shape,
                    std::size_t used_plaintexts, std::size_t selected_plaintext,
                    std::size_t selected_offset, std::size_t decomposition_count,
                    int first_noise, int final_noise, const std::vector<StageStats>& stats,
                    const std::vector<std::uint8_t>& expected,
                    const std::vector<std::uint8_t>& actual)
{
    if (!options.evaluator.delivery.emit_directory) return;
    const auto& root = *options.evaluator.delivery.emit_directory;
    std::filesystem::create_directories(root);
    write_bytes(root / "expected_record.bin", expected);
    write_bytes(root / "actual_record.bin", actual);
    std::ofstream manifest(root / "pipeline_manifest.json");
    if (!manifest) throw std::runtime_error("failed to create pipeline_manifest.json");
    manifest << "{\n"
             << "  \"application\": \"Microsoft SealPIR two-dimensional BFV HPU port\",\n"
             << "  \"profile\": \"" << options.profile.name << "\",\n"
             << "  \"record_count\": " << options.profile.element_count << ",\n"
             << "  \"record_bytes\": " << kElementBytes << ",\n"
             << "  \"poly_modulus_degree\": " << kDegree << ",\n"
             << "  \"usable_plain_bits\": " << kUsablePlainBits << ",\n"
             << "  \"dimensions\": [" << shape[0] << ", " << shape[1] << "],\n"
             << "  \"used_plaintexts\": " << used_plaintexts << ",\n"
             << "  \"selected_record\": " << options.target << ",\n"
             << "  \"selected_plaintext\": " << selected_plaintext << ",\n"
             << "  \"selected_offset\": " << selected_offset << ",\n"
             << "  \"database_seed\": \"0x5345414c50495232\",\n"
             << "  \"seal_random_seed\": \"0x5345414c50495232,1,2,3,4,5,6,7\",\n"
             << "  \"coefficient_modulus_bits\": [31, 31, 31, 31],\n"
             << "  \"recursive_modulus_target\": \"two 31-bit Q primes after one HPU mod-switch\",\n"
             << "  \"parameter_adaptation\": \"SealPIR's one approximately 36-bit-Q last level is not representable by the 31-bit HPU ABI; a two-Q 62-bit recursive level is used without removing modulus switching\",\n"
             << "  \"first_stage_decomposed_plaintexts\": " << decomposition_count << ",\n"
             << "  \"first_stage_min_noise_budget_bits\": " << first_noise << ",\n"
             << "  \"final_stage_min_noise_budget_bits\": " << final_noise << ",\n"
             << "  \"cross_stage_source\": \"BFV software executor memory (not SEAL oracle)\",\n"
             << "  \"semantic_record_match\": " << (expected == actual ? "true" : "false") << ",\n"
             << "  \"real_hardware_verified\": false,\n"
             << "  \"packages\": [\n";
    for (std::size_t i = 0; i < stats.size(); ++i) {
        const auto& item = stats[i];
        manifest << "    {\"name\": \"" << item.name << "\", \"directory\": \""
                 << item.directory << "\", \"graph_nodes\": " << item.graph_nodes
                 << ", \"instructions\": " << item.instructions
                 << ", \"dma_spans\": " << item.dma_spans
                 << ", \"memory_lines\": " << item.memory_lines
                 << ", \"host_instruction_verified\": "
                 << (item.instruction_verified ? "true" : "false") << "}"
                 << (i + 1 == stats.size() ? "\n" : ",\n");
    }
    manifest << "  ]\n}\n";
    std::ofstream commands(root / "README.txt");
    commands << "Generated by:\n"
             << "  hpu_bfv_sealpir_2d_example --profile " << options.profile.name
             << " --record-index " << options.target << " --emit-dir " << root.string() << "\n\n"
             << "Validate every HPU package directory listed in pipeline_manifest.json:\n"
             << "  hpu_validate_package <package-directory>\n\n"
             << "This run verifies SEAL-oracle/HPU-software equality and the final record.\n"
             << "It does not claim execution on real HPU hardware.\n";
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const auto options = parse_options(argc, argv);
        if (options.evaluator.delivery.poly_modulus_degree
            && *options.evaluator.delivery.poly_modulus_degree != kDegree)
            throw std::invalid_argument("SealPIR 2D fixes --degree to N=4096");

        hpu::seal_adapter::BfvContextSpec spec;
        spec.poly_modulus_degree = kDegree;
        // SealPIR's BFVDefault(4096) primes are 36/36/37 bits. A single
        // 31-bit recursive Q exhausts its budget for the 86-term full dot
        // product, so use three data Qs plus P and switch once to a two-Q
        // recursive level. Modulus switching and both recursion rounds remain.
        spec.coeff_modulus_bits = {31, 31, 31, 31};
        spec.plain_modulus_bits = 21;
        spec.random_seed = std::array<std::uint64_t, 8>{
            kDatabaseSeed, 1, 2, 3, 4, 5, 6, 7};
        const auto bundle = hpu::seal_adapter::create_bfv_context(spec);
        const auto& context = *bundle.context;
        require(static_cast<std::uint32_t>(std::floor(std::log2(bundle.plain_modulus)))
                    == kUsablePlainBits,
                "batching plaintext modulus does not provide 20 usable bits");

        const auto per_plain = kDegree / coefficients_per_element();
        const auto used_plaintexts =
            (options.profile.element_count + per_plain - 1) / per_plain;
        const auto shape = dimensions(used_plaintexts);
        require(per_plain == 9 && coefficients_per_element() == 410,
                "SealPIR 1024-byte packing changed unexpectedly");
        if (options.profile.name == "small")
            require(used_plaintexts == 15 && shape == std::vector<std::size_t>{4, 4},
                    "small profile must retain its 4x4 layout");
        else
            require(used_plaintexts == 7282 && shape == std::vector<std::size_t>{86, 85},
                    "full profile must retain SealPIR's 86x85 layout");

        const auto selected_plaintext = options.target / per_plain;
        const auto selected_offset = options.target % per_plain;
        const auto query_indices = compute_indices(selected_plaintext, shape);
        auto database = build_database(context, options.profile.element_count, shape);

        ::seal::KeyGenerator keygen(context);
        ::seal::GaloisKeys galois_keys;
        std::vector<std::uint32_t> galois_elements;
        for (std::size_t i = 0; i < integer_log2_ceil(kDegree); ++i)
            galois_elements.push_back(static_cast<std::uint32_t>(
                kDegree / (std::size_t{1} << i) + 1));
        keygen.create_galois_keys(galois_elements, galois_keys);
        ::seal::Encryptor encryptor(context, keygen.secret_key());
        std::vector<::seal::Ciphertext> compressed_queries;
        for (std::size_t dimension = 0; dimension < shape.size(); ++dimension) {
            ::seal::Plaintext query(kDegree);
            query.set_zero();
            const auto scale = std::uint64_t{1} << integer_log2_ceil(shape[dimension]);
            std::uint64_t inverse = 0;
            require(::seal::util::try_invert_uint_mod(scale, bundle.plain_modulus, inverse),
                    "query expansion scale is not invertible modulo t");
            query[query_indices[dimension]] = inverse;
            ::seal::Ciphertext encrypted;
            encryptor.encrypt_symmetric(query, encrypted);
            compressed_queries.push_back(std::move(encrypted));
        }

        std::vector<StageStats> stats;
        const auto expanded_first = expand_query_hpu(
            options, context, compressed_queries[0], shape[0], galois_keys,
            "stage0", stats);
        const auto first_actual = dot_product_hpu(
            options, context, expanded_first, database, shape[1], true,
            "stage0", stats);
        const auto first_noise = minimum_noise_budget(
            context, keygen.secret_key(), first_actual);
        require(first_noise > 0, "first recursive stage exhausted its BFV noise budget");

        const auto recursive_context = context.get_context_data(first_actual.front().parms_id());
        require(static_cast<bool>(recursive_context),
                "first HPU stage produced an unknown recursive modulus level");
        const auto& recursive_parameters = recursive_context->parms();
        std::vector<::seal::Plaintext> decomposed;
        for (const auto& ciphertext : first_actual) {
            require(ciphertext.parms_id() == recursive_context->parms_id(),
                    "first HPU stage outputs disagree on the recursive modulus level");
            auto chunks = decompose_to_plaintexts(recursive_parameters, ciphertext);
            decomposed.insert(decomposed.end(),
                              std::make_move_iterator(chunks.begin()),
                              std::make_move_iterator(chunks.end()));
        }
        require(decomposed.size() == shape[1] * expansion_ratio(recursive_parameters) * 2,
                "first-stage ciphertext decomposition has the wrong size");

        const auto expanded_second = expand_query_hpu(
            options, context, compressed_queries[1], shape[1], galois_keys,
            "stage1", stats);
        require(decomposed.size() % shape[1] == 0,
                "second-stage recursive matrix is not divisible by its dimension");
        const auto final_actual = dot_product_hpu(
            options, context, expanded_second, decomposed,
            decomposed.size() / shape[1], true, "stage1", stats);
        for (const auto& ciphertext : final_actual)
            require(ciphertext.parms_id() == recursive_context->parms_id(),
                    "final HPU reply was not switched to the recursive modulus level");
        const auto final_noise = minimum_noise_budget(
            context, keygen.secret_key(), final_actual);
        require(final_noise > 0, "second recursive stage exhausted its BFV noise budget");

        const auto decoded_plain = decode_recursive_reply(
            context, keygen.secret_key(), final_actual);
        ::seal::BatchEncoder encoder(context);
        std::vector<std::uint64_t> decoded;
        encoder.decode(decoded_plain, decoded);
        const auto coefficient_begin = selected_offset * coefficients_per_element();
        std::vector<std::uint64_t> selected_coefficients(
            decoded.begin() + static_cast<std::ptrdiff_t>(coefficient_begin),
            decoded.begin() + static_cast<std::ptrdiff_t>(
                coefficient_begin + coefficients_per_element()));
        const auto actual_record = coefficients_to_bytes(selected_coefficients);
        const auto expected_record = database_element(options.target);
        require(actual_record == expected_record,
                "two-dimensional recursive PIR returned the wrong 1024-byte record");

        write_manifest(options, shape, used_plaintexts, selected_plaintext,
                       selected_offset, decomposed.size(), first_noise, final_noise,
                       stats, expected_record, actual_record);

        std::size_t instructions = 0;
        std::size_t dma = 0;
        for (const auto& item : stats) {
            instructions += item.instructions;
            dma += item.dma_spans;
        }
        std::cout << "Microsoft SealPIR two-dimensional BFV HPU application: PASS\n"
                  << "Profile: " << options.profile.name << " ("
                  << options.profile.element_count << " records x " << kElementBytes
                  << " bytes, N=" << kDegree << ", d=2)\n"
                  << "Batch packing: " << per_plain << " records/plaintext, "
                  << used_plaintexts << " used, matrix " << shape[0] << 'x' << shape[1]
                  << "\nSelected record/plaintext/offset: " << options.target << '/'
                  << selected_plaintext << '/' << selected_offset << "\n"
                  << "First-stage actual HPU-software outputs: " << first_actual.size()
                  << ", decomposed plaintexts: " << decomposed.size() << "\n"
                  << "Second-stage actual HPU-software replies: " << final_actual.size()
                  << "\nMinimum noise budgets (stage0/stage1): " << first_noise << '/'
                  << final_noise << " bits\n"
                  << "Packages/instructions/DMA spans: " << stats.size() << '/'
                  << instructions << '/' << dma << "\n"
                  << "Cross-stage source: HPU software executor memory, not SEAL oracle\n"
                  << "HPU adaptation: BFVDefault 36/36/37-bit chain -> 31/31/31/31-bit chain;\n"
                  << "  recursive transfer uses two Qs after one HPU mod-switch (single-Q 31-bit failed at full scale)\n"
                  << "SEAL oracle comparison: PASS\nHPU software execution: PASS\n"
                  << "Real HPU hardware execution: NOT RUN\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SealPIR 2D application failed: " << error.what() << '\n';
        return 1;
    }
}
