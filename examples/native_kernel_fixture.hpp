#pragma once

#include "hpu/compiler/operator_resources.hpp"
#include "hpu/delivery/application_package.hpp"
#include "hpu/model/hardware_ntt.hpp"
#include "poly/cmult.hpp"
#include "util/ntt.hpp"

#include <limits>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Shared native-kernel examples and IO regression fixtures. These operate on
// ring polynomials directly; they do not encode/encrypt complete ciphertexts.
namespace native_kernel_example {
namespace r = hpu::resource;
using hpu::compiler::ResolvedDma;
using hpu::runtime::AllocationKind;
using hpu::runtime::HpuMemImage;
inline constexpr std::size_t degree = 8192;
inline constexpr std::uint32_t modulus = 65537;

inline void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

struct Fixture {
    HpuMemImage image;
    r::OperatorProgram program;
    std::set<std::string> outputs;
    std::vector<std::pair<std::string, std::vector<std::uint32_t>>> expected;
};

inline std::vector<std::uint32_t> moduli(std::size_t count)
{
    const auto mu = std::numeric_limits<std::uint64_t>::max() / modulus;
    std::vector<std::uint32_t> result;
    for (std::size_t i = 0; i < count; ++i) {
        result.insert(result.end(), {modulus, static_cast<std::uint32_t>(mu),
                                    static_cast<std::uint32_t>(mu >> 32U), 0});
    }
    return result;
}

inline ResolvedDma binding(const HpuMemImage& image, const std::string& id)
{
    return {0, id, image.allocation(id).span, 0, "native_io", 0};
}

inline std::vector<std::uint32_t> coefficients(std::size_t seed)
{
    std::vector<std::uint32_t> words(degree);
    for (std::size_t i = 0; i < words.size(); ++i)
        words[i] = static_cast<std::uint32_t>((i * i + seed * i + seed + 3) % modulus);
    return words;
}

inline Fixture cmult_fixture()
{
    HpuMemImage image(8192);
    image.add("moduli", moduli(4), AllocationKind::modulus_table);
    std::set<std::string> outputs;
    std::vector<std::pair<std::string, std::vector<std::uint32_t>>> expected;
    std::map<std::string, ResolvedDma> ports;
    ports.emplace("constants/modulus_table", binding(image, "moduli"));
    for (std::size_t q = 0; q < 4; ++q) {
        const auto prefix = "tensor/mod" + std::to_string(q) + "/";
        std::vector<std::vector<std::uint32_t>> inputs;
        for (const auto& role : {"left/c0", "right/c0", "left/c1", "right/c1"}) {
            const auto id = "input/q" + std::to_string(q) + "/" + role;
            inputs.push_back(coefficients(1 + inputs.size() + q * 7));
            image.add(id, inputs.back(), AllocationKind::constant);
            ports.emplace(prefix + role, binding(image, id));
        }
        for (std::size_t c = 0; c < 3; ++c) {
            const auto id = "output/q" + std::to_string(q) + "/c" + std::to_string(c);
            image.reserve(id, degree, AllocationKind::output);
            outputs.insert(id);
            ports.emplace(prefix + "output/c" + std::to_string(c), binding(image, id));
            std::vector<std::uint32_t> words(degree);
            for (std::size_t i = 0; i < degree; ++i) {
                const auto product = [&](std::size_t a, std::size_t b) {
                    return static_cast<std::uint64_t>(inputs[a][i]) * inputs[b][i] % modulus;
                };
                words[i] = static_cast<std::uint32_t>(c == 0 ? product(0, 1)
                    : c == 1 ? (product(0, 3) + product(2, 1)) % modulus : product(2, 3));
            }
            expected.emplace_back(id, std::move(words));
        }
    }
    r::Recording recording;
    generate_hpu_cmult_body_asm(4, true, true);
    auto program = recording.finish();
    program.bind_named_ports(ports);
    for (const auto& port : program.ports)
        require(port.binding.has_value(), "CMULT test did not bind every semantic data reference");
    return {std::move(image), std::move(program), std::move(outputs), std::move(expected)};
}

inline Fixture transform_fixture(bool inverse)
{
    HpuMemImage image(8192);
    image.add("moduli", moduli(1), AllocationKind::modulus_table);
    const auto psi = hpu::model::pow_mod(3, (modulus - 1U) / (2 * degree), modulus);
    hpu::model::HardwareNttModel model(degree, modulus, hpu::model::pow_mod(psi, 2, modulus));
    const auto forward = model.forward_twiddles();
    const auto inverse_tables = model.inverse_twiddles();
    const auto inverse_psi = hpu::model::inverse_mod_prime(psi, modulus);
    std::vector<std::string> tables;
    for (std::size_t stage = 0; stage < 13; ++stage) {
        const auto id = "tables/stage" + std::to_string(stage);
        image.add(id, hpu::model::pack_stage_twiddles(
            inverse ? inverse_tables.stages[stage] : forward[stage], modulus, degree), AllocationKind::twiddle);
        tables.push_back(id);
    }
    const auto factors = hpu::model::pack_online_pmul_parameters(
        inverse ? hpu::model::inverse_mod_prime(static_cast<std::uint32_t>(degree), modulus) : 1,
        inverse ? inverse_psi : psi, modulus, degree);
    const std::string factor_id = inverse ? "tables/post" : "tables/pre";
    image.add(factor_id, factors, AllocationKind::twiddle);
    std::map<std::string, ResolvedDma> table_ports;
    std::set<std::string> outputs;
    std::vector<std::pair<std::string, std::vector<std::uint32_t>>> expected;
    r::Recording recording;
    const auto mod = r::make_operand("modulus");
    r::input(mod, binding(image, "moduli"), hpu::DataType::mod_ctx, hpu::DloadFlag::small_bank);
    r::select_modulus(0);
    const std::size_t count = inverse ? 3 : 6;
    for (std::size_t i = 0; i < count; ++i) {
        const auto data = r::make_operand("polynomial");
        const auto table = r::make_operand("twiddle");
        const auto input_id = "input/" + std::to_string(i);
        const auto output_id = "output/" + std::to_string(i);
        const auto natural = coefficients(i + 1);
        auto spectrum = hpu::model::negacyclic_forward(natural, modulus, psi);
        image.add(input_id, inverse ? spectrum : natural, AllocationKind::constant);
        image.reserve(output_id, degree, AllocationKind::output);
        outputs.insert(output_id);
        expected.emplace_back(output_id, inverse ? natural : spectrum);
        r::input(data, binding(image, input_id), hpu::DataType::poly);
        const auto prefix = "transform/" + std::to_string(i) + (inverse ? "/inverse" : "/forward");
        if (inverse) generate_hpu_intt_body_asm(degree, data, table, false, prefix);
        else generate_hpu_ntt_body_asm(degree, data, table, false, prefix);
        for (std::size_t stage = 0; stage < tables.size(); ++stage)
            table_ports.emplace(prefix + "/stage" + std::to_string(stage), binding(image, tables[stage]));
        table_ports.emplace(prefix + (inverse ? "/post_untwist_scale" : "/pre_twist"), binding(image, factor_id));
        r::output(data, binding(image, output_id));
    }
    r::finish_program();
    auto program = recording.finish();
    program.bind_named_ports(table_ports);
    for (const auto& port : program.ports)
        require(port.binding.has_value(), "transform test did not bind every semantic table reference");
    return {std::move(image), std::move(program), std::move(outputs), std::move(expected)};
}

inline hpu::delivery::ApplicationPackageRequest package_request(
    Fixture& fixture, const std::string& kernel,
    hpu::delivery::ProducerIdentity producer = {"inline-asm", "unknown", "unknown"})
{
    if (kernel != "cmult" && kernel != "ntt" && kernel != "intt")
        throw std::invalid_argument("unknown native ring kernel: " + kernel);
    const auto stem = "native_" + kernel + "_n8192";
    hpu::delivery::ApplicationPackageRequest request;
    // Package v1 requires a scheme. This tag selects the representation
    // convention only, not encrypted CKKS or SEAL differential verification.
    request.scheme = hpu::delivery::SchemeKind::ckks;
    request.case_name = stem;
    request.producer = std::move(producer);
    request.initial_image = &fixture.image;
    request.program.stem = stem;
    request.program.operator_program = std::make_shared<r::OperatorProgram>(fixture.program);
    const auto domain = kernel == "intt" ? hpu::delivery::OutputDomain::coefficient
                                         : hpu::delivery::OutputDomain::canonical_ntt_physical;
    for (std::size_t i = 0; i < fixture.expected.size(); ++i) {
        const auto& output = fixture.expected[i];
        const std::size_t component = kernel == "cmult" ? i % 3 : i;
        const auto modulus_id = static_cast<std::uint8_t>(kernel == "cmult" ? i / 3 : 0);
        request.outputs.push_back({"native_result", component, modulus_id, modulus, domain,
            fixture.image.allocation(output.first).span, output.second.size(), output.second});
    }
    request.parameters_json = "{\"poly_modulus_degree\":8192,\"test_layer\":\"native_ring_arithmetic\","
        "\"encrypted\":false,\"seal_differential_oracle\":false,\"deterministic_seed\":1,"
        "\"modulus_context_count\":" + std::string(kernel == "cmult" ? "4" : "1")
        + ",\"context_moduli\":" + std::string(kernel == "cmult"
            ? "[65537,65537,65537,65537]" : "[65537]")
        + ",\"input_domain\":\"" + std::string(kernel == "ntt"
            ? "coefficient_natural" : "canonical_ntt_physical") + "\"}\n";
    request.operation_graph_json = "{\"operation\":\"" + kernel
        + "\",\"ir\":\"bound_native_ssa\",\"observable_output_limbs\":"
        + std::to_string(request.outputs.size()) + "}\n";
    request.oracle_report_json = "{\"status\":\"verified\",\"oracle\":\""
        + std::string(kernel == "cmult" ? "independent_u64_modular_tensor"
            : "existing_negacyclic_functional_model") + "\",\"seed\":1}\n";
    return request;
}

} // namespace native_kernel_example
