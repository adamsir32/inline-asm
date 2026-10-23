#include "../examples/native_kernel_fixture.hpp"
#include "hpu/compiler/ssa_optimization.hpp"
#include "hpu/delivery/resource_compiler.hpp"
#include "hpu/model/instruction_executor.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace {
namespace r = hpu::resource;
using hpu::compiler::CompiledResources;
using namespace native_kernel_example;

class ScratchDirectory {
public:
    ScratchDirectory()
    {
        const auto name = (std::filesystem::temp_directory_path() / "hpu_native_package.XXXXXX").string();
        std::vector<char> pattern(name.begin(), name.end());
        pattern.push_back('\0');
        const auto created = ::mkdtemp(pattern.data());
        if (!created) throw std::runtime_error("cannot create native package test directory");
        path_ = created;
    }
    ~ScratchDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    const std::filesystem::path& path() const { return path_; }
private:
    std::filesystem::path path_;
};

CompiledResources compile(const r::OperatorProgram& source, const HpuMemImage& image,
                          const std::set<std::string>& outputs, bool optimized)
{
    const auto program = optimized ? r::optimize_operator_program(source, image, outputs)
                                   : r::canonical_operator_program(source, image, outputs);
    hpu::compiler::CompileOptions options;
    options.optimization = optimized ? hpu::compiler::OptimizationMode::optimized
                                     : hpu::compiler::OptimizationMode::no_opt;
    options.automatic_lifetimes = true;
    options.required_outputs = outputs;
    options.dma_cache_policy = hpu::compiler::DmaCachePolicy::twiddle_loads_only;
    return hpu::compiler::compile_resources(r::bind_program(program, image), {}, {}, image, options);
}

std::size_t loads(const CompiledResources& result, const std::string& prefix)
{
    std::size_t count = 0;
    for (const auto& dma : result.dma) {
        if (result.instructions.at(dma.instruction_index).instruction.mnemonic == hpu::Mnemonic::kDload
            && dma.allocation_id.rfind(prefix, 0) == 0) ++count;
    }
    return count;
}

std::uint64_t load_bytes(const CompiledResources& result, const std::string& prefix)
{
    std::uint64_t bytes = 0;
    for (const auto& dma : result.dma) {
        if (result.instructions.at(dma.instruction_index).instruction.mnemonic == hpu::Mnemonic::kDload
            && dma.allocation_id.rfind(prefix, 0) == 0)
            bytes += dma.span.line_count * hpu::runtime::kHpuMemLineWords * sizeof(std::uint32_t);
    }
    return bytes;
}

void check(const CompiledResources& result,
           const std::vector<std::pair<std::string, std::vector<std::uint32_t>>>& expected)
{
    std::vector<hpu::runtime::HpuMemSpan> spans;
    for (const auto& dma : result.dma) spans.push_back(dma.span);
    const auto execution = hpu::model::execute_instructions(result.instructions, spans, result.image);
    require(execution.report.completed(), "native instruction execution failed: " + execution.report.message);
    std::vector<hpu::model::InstructionMemoryExpectation> comparisons;
    for (const auto& entry : expected)
        comparisons.push_back({entry.first, result.image.allocation(entry.first).span, entry.second});
    const auto comparison = hpu::model::compare_instruction_memory(execution, comparisons);
    require(comparison.verified, "native IO optimization changed arithmetic: " + comparison.message);
}

void cmult_four_inputs_per_modulus()
{
    const auto fixture = cmult_fixture();
    const auto& image = fixture.image;
    const auto& program = fixture.program;
    const auto& outputs = fixture.outputs;
    const auto& expected = fixture.expected;
    const auto result = compile(program, image, outputs, true);
    require(loads(result, "input/") == 16,
            "Q4 CMULT did not load each of its four source polynomials exactly once");
    require(load_bytes(result, "input/") == 16 * degree * sizeof(std::uint32_t),
            "Q4 CMULT source reads did not use the expected 512 KiB");
    require(result.report.required_output_write_bytes == 12 * degree * sizeof(std::uint32_t),
            "Q4 CMULT did not account for its twelve exported output polynomials");
    require(result.report.spills == 0, "N8192 CMULT unnecessarily spilled a source");
    check(result, expected);
}

void transform_table_residency(bool inverse)
{
    const auto fixture = transform_fixture(inverse);
    const auto& image = fixture.image;
    const auto& program = fixture.program;
    const auto& outputs = fixture.outputs;
    const auto& expected = fixture.expected;
    const std::string factor_id = inverse ? "tables/post" : "tables/pre";
    const auto baseline = compile(program, image, outputs, false);
    const auto optimized = compile(program, image, outputs, true);
    if (inverse) {
        require(loads(baseline, "tables/stage") == 39 && loads(optimized, "tables/stage") == 13,
                "three N8192 INTTs did not reduce stage parameter loads from 39 to 13");
        require(load_bytes(baseline, "tables/stage") - load_bytes(optimized, "tables/stage") == 13 * 1024,
                "three N8192 INTTs did not save 13 KiB of stage parameter reads");
    } else {
        require(loads(baseline, factor_id) == 6 && loads(optimized, factor_id) == 1,
                "six N8192 NTTs did not retain their shared pre-twist vector");
        require(load_bytes(baseline, factor_id) - load_bytes(optimized, factor_id) == 160 * 1024,
                "six N8192 NTTs did not save 160 KiB of pre-twist reads");
    }
    check(baseline, expected);
    check(optimized, expected);
}

void native_package_roundtrip(const std::string& kernel)
{
    auto fixture = kernel == "cmult" ? cmult_fixture() : transform_fixture(kernel == "intt");
    auto request = package_request(fixture, kernel, {"inline-asm", "473f6ec", "test-fixture"});
    hpu::delivery::compile_application_resources(request, fixture.image);
    ScratchDirectory scratch;
    const auto destination = scratch.path() / request.case_name;
    hpu::delivery::write_application_package(destination, request);
    hpu::delivery::validate_application_package_on_disk(destination);
    require(request.outputs.size() == fixture.expected.size(),
            "native package lost an exported result limb");
    require(request.parameters_json.find("\"encrypted\":false") != std::string::npos,
            "native ring package incorrectly claims encrypted application verification");
    require(request.oracle_report_json.find("\"host_instruction_model_verified\":true") != std::string::npos,
            "native package did not verify its final encoded instruction stream");
}

void infeasible_dead_checkpoint_does_not_block_output_roots()
{
    auto fixture = cmult_fixture();
    fixture.image.add("unused/left", std::vector<std::uint32_t>(65536, 7), AllocationKind::constant);
    fixture.image.add("unused/right", std::vector<std::uint32_t>(65536, 9), AllocationKind::constant);
    r::Recording recording;
    recording.append(fixture.program);
    r::select_modulus(0);
    const auto left = r::make_operand("unused_left");
    const auto right = r::make_operand("unused_right");
    const auto product = r::make_operand("unused_product");
    const auto bank_zero_only = static_cast<hpu::DloadFlag>(0x01);
    r::input(left, binding(fixture.image, "unused/left"), hpu::DataType::poly, bank_zero_only);
    r::input(right, binding(fixture.image, "unused/right"), hpu::DataType::poly, bank_zero_only);
    r::multiply(product, left, right);
    r::finish_program();
    fixture.program = recording.finish();
    auto request = package_request(fixture, "cmult");
    const auto report = hpu::delivery::compile_application_resources(request, fixture.image);
    require(report.find("\"all_checkpoint_status\":\"infeasible\"") != std::string::npos &&
                report.find("\"all_checkpoint_dma_bytes\":null") != std::string::npos &&
                report.find("\"all_checkpoint_dma_count\":null") != std::string::npos,
            "infeasible unused branch did not produce an unavailable diagnostic baseline");
    require(request.oracle_report_json.find("\"host_instruction_model_verified\":true") != std::string::npos,
            "a dead infeasible branch blocked verification of feasible observable outputs");
    require(!fixture.image.contains("unused/left") && !fixture.image.contains("unused/right"),
            "an unobservable infeasible branch survived deployment image pruning");
}

} // namespace

int main()
{
    try {
        cmult_four_inputs_per_modulus();
        transform_table_residency(true);
        transform_table_residency(false);
        for (const auto& kernel : {"cmult", "ntt", "intt"}) native_package_roundtrip(kernel);
        infeasible_dead_checkpoint_does_not_block_output_roots();
        std::cout << "Native N8192 IO optimization acceptance tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Native IO optimization test failed: " << error.what() << '\n';
        return 1;
    }
}
