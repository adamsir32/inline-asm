#include "hpu/seal/evaluator.hpp"
#include "hpu/compiler/resource_ir.hpp"
#include "hpu/model/instruction_executor.hpp"
#include "hpu/seal/operation_runtime.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace hpu::seal_adapter::detail {
namespace {

std::string json_string(const std::string& value)
{
    std::ostringstream out;
    out << '"';
    for (const unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c == '\n') out << "\\n";
        else if (c == '\r') out << "\\r";
        else if (c == '\t') out << "\\t";
        else if (c < 32) {
            const char* digits = "0123456789abcdef";
            out << "\\u00" << digits[c >> 4] << digits[c & 15];
        } else out << c;
    }
    return out.str() + '"';
}

std::vector<std::vector<std::string>> csv_rows(const std::string& csv)
{
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string field;
    bool quoted = false;
    for (std::size_t i = 0; i < csv.size(); ++i) {
        const char c = csv[i];
        if (c == '"') {
            if (quoted && i + 1 < csv.size() && csv[i + 1] == '"') {
                field += '"'; ++i;
            } else quoted = !quoted;
        } else if (!quoted && (c == ',' || c == '\n')) {
            row.push_back(std::move(field)); field.clear();
            if (c == '\n') { rows.push_back(std::move(row)); row.clear(); }
        } else field += c;
    }
    if (quoted) throw std::invalid_argument("unterminated resolved DMA CSV field");
    if (!field.empty() || !row.empty()) {
        row.push_back(std::move(field)); rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<hpu::compiler::ResolvedDma> resolved_dma(
    const hpu::delivery::ProgramArtifacts& program)
{
    const auto rows = csv_rows(program.resolved_dma_manifest);
    if (rows.empty() || rows.front().size() != 14 || rows.size() - 1 != program.ordered_dma_spans.size())
        throw std::invalid_argument("frontend requires complete resolved DMA provenance");
    std::vector<hpu::compiler::ResolvedDma> result;
    for (std::size_t i = 1; i < rows.size(); ++i) {
        const auto& row = rows[i];
        if (row.size() != 14) throw std::invalid_argument("invalid resolved DMA row");
        hpu::compiler::ResolvedDma dma;
        dma.instruction_index = std::stoull(row[0]);
        if (!row[2].empty()) dma.operation_index = std::stoull(row[2]);
        dma.operation_id = row[3];
        dma.operation_dma_index = std::stoull(row[4]);
        dma.allocation_id = row[9];
        dma.span = {std::stoull(row[10]), std::stoull(row[11])};
        const auto expected = program.ordered_dma_spans[i - 1];
        if (dma.span.line_offset != expected.line_offset || dma.span.line_count != expected.line_count)
            throw std::invalid_argument("resolved DMA CSV disagrees with encoded program spans");
        result.push_back(std::move(dma));
    }
    return result;
}

const hpu::runtime::HpuMemAllocation& containing_allocation(
    const hpu::runtime::HpuMemImage& image, hpu::runtime::HpuMemSpan span)
{
    for (const auto& allocation : image.allocations()) {
        if (span.line_offset >= allocation.span.line_offset &&
            span.line_count <= allocation.span.line_count &&
            span.line_offset - allocation.span.line_offset <= allocation.span.line_count - span.line_count)
            return allocation;
    }
    throw std::invalid_argument("golden destination has no physical allocation");
}

void append_json_members(std::string& object, const std::string& members)
{
    const auto closing = object.find_last_of('}');
    if (closing == std::string::npos) throw std::invalid_argument("invalid generated JSON object");
    object.insert(closing, "," + members);
}

} // namespace

std::string finalize_frontend_package(
    hpu::delivery::ApplicationPackageRequest& request,
    hpu::runtime::HpuMemImage& image, bool optimize, bool require_instruction_verification,
    hpu::model::InstructionNttContract instruction_ntt_contract)
{
    hpu::model::InstructionExecutionOptions execution_options;
    execution_options.ntt_contract = instruction_ntt_contract;
    std::optional<hpu::model::InstructionExecutionResult> baseline_execution;
    std::vector<hpu::runtime::HpuMemSpan> baseline_output_spans;
    if (optimize) {
        baseline_execution = hpu::model::execute_instructions(
            request.program.instructions, request.program.ordered_dma_spans, image, execution_options);
        for (const auto& output : request.outputs) baseline_output_spans.push_back(output.destination);
    }
    hpu::compiler::CompileOptions options;
    options.optimization = optimize ? hpu::compiler::OptimizationMode::optimized
                                   : hpu::compiler::OptimizationMode::no_opt;
    using Layout = hpu::compiler::MemoryLayout;
    using Kind = hpu::runtime::AllocationKind;
    for (const auto& allocation : image.allocations()) {
        if (allocation.kind == Kind::ciphertext && allocation.read_only)
            options.buffer_layouts.emplace(allocation.id,
                request.scheme == hpu::delivery::SchemeKind::bfv
                    ? Layout::coefficient_natural : Layout::canonical_ntt_physical);
        else if (allocation.kind == Kind::evaluation_key
            || (allocation.kind == Kind::plaintext
                && request.scheme != hpu::delivery::SchemeKind::bfv))
            options.buffer_layouts.emplace(allocation.id, Layout::canonical_ntt_physical);
    }
    // Output labels describe the golden ABI, not a claim that the instruction
    // stream has already produced those words. Mutable transform scratch can
    // cross domains and is deliberately not assigned one permanent layout.
    for (const auto& output : request.outputs) {
        const auto& allocation = containing_allocation(image, output.destination);
        const auto layout = output.domain == hpu::delivery::OutputDomain::coefficient
            ? Layout::coefficient_natural : Layout::canonical_ntt_physical;
        const auto inserted = options.buffer_layouts.emplace(allocation.id, layout);
        if (!inserted.second && inserted.first->second != layout)
            throw std::invalid_argument("conflicting output buffer layouts: " + allocation.id);
    }
    auto resources = hpu::compiler::optimize_resources(
        request.program.instructions, resolved_dma(request.program), image, options);
    for (auto& output : request.outputs) {
        const auto& allocation = containing_allocation(image, output.destination);
        output.destination = hpu::compiler::relocate_span(resources, allocation.id, output.destination);
    }

    // All schemes share the encoded ISA and resolved DMA format. Rendering
    // consumes the scheduled stream rather than a second operation recipe.
    CkksRuntimeProgram program;
    program.instructions = resources.instructions;
    std::map<std::string, std::size_t> operation_dma_counts;
    for (std::size_t i = 0; i < resources.dma.size(); ++i) {
        const auto& dma = resources.dma[i];
        const auto& inst = program.instructions.at(dma.instruction_index).instruction;
        CkksDmaBinding binding;
        binding.program_dma_index = i;
        binding.operation_index = dma.operation_index;
        binding.operation_id = dma.operation_id;
        binding.operation_dma_index = operation_dma_counts[dma.operation_id]++;
        binding.object_slot = inst.obj_id;
        binding.direction = inst.mnemonic == hpu::Mnemonic::kDload ? CkksDmaDirection::load
                                                                 : CkksDmaDirection::store;
        binding.load_type = static_cast<hpu::DataType>(inst.type);
        binding.load_flag = static_cast<hpu::DloadFlag>(inst.dma_flag);
        binding.store_release = inst.type;
        binding.allocation_id = dma.allocation_id;
        binding.span = dma.span;
        program.dma.push_back({dma.instruction_index, std::move(binding)});
    }
    const auto artifacts = render_ckks_runtime_artifacts(
        request.program.stem, program, resources.image.capacity_lines());
    request.program.instructions = program.instructions;
    request.program.ordered_dma_spans = program.spans();
    request.program.header = artifacts.header;
    request.program.source = artifacts.source;
    request.program.resolved_dma_manifest = artifacts.resolved_dma_manifest;
    std::ostringstream assembly;
    for (const auto& instruction : program.instructions) assembly << instruction.normalized_asm << '\n';
    request.program.assembly = assembly.str();

    const auto resource_report = hpu::compiler::render_resource_report(resources);
    image = std::move(resources.image);
    request.initial_image = &image;
    const auto execution = hpu::model::execute_instructions(
        request.program.instructions, request.program.ordered_dma_spans, image, execution_options);
    std::vector<hpu::model::InstructionMemoryExpectation> expected;
    for (const auto& output : request.outputs)
        expected.push_back({output.object_id + "/c" + std::to_string(output.component) +
            "/q" + std::to_string(output.modulus_id), output.destination, output.golden_words});
    const auto comparison = hpu::model::compare_instruction_memory(execution, expected);
    bool optimization_verified = !optimize;
    if (baseline_execution && baseline_execution->report.completed() && execution.report.completed()) {
        std::vector<hpu::model::InstructionMemoryExpectation> baseline_expected;
        for (std::size_t i = 0; i < request.outputs.size(); ++i) {
            const auto begin = baseline_output_spans[i].line_offset * hpu::runtime::kHpuMemLineWords;
            const auto& output = request.outputs[i];
            const auto& words = baseline_execution->words;
            if (begin > words.size() || output.word_count > words.size() - begin)
                throw std::runtime_error("baseline instruction output exceeds image");
            baseline_expected.push_back({expected[i].id, output.destination,
                std::vector<std::uint32_t>(words.begin() + begin, words.begin() + begin + output.word_count)});
        }
        const auto equivalence = hpu::model::compare_instruction_memory(execution, baseline_expected);
        if (!equivalence.verified)
            throw std::runtime_error("resource optimization changed encoded execution: " + equivalence.message);
        optimization_verified = true;
    }
    const auto status = comparison.verified ? "pass" : execution.report.completed() ? "fail" :
        execution.report.status == hpu::model::InstructionExecutionStatus::unsupported_contract ? "unsupported" : "fail";
    const auto message = execution.report.completed() ? comparison.message : execution.report.message;
    if (require_instruction_verification && !comparison.verified)
        throw std::runtime_error("encoded instruction verification " + std::string(status)
            + " [" + hpu::model::instruction_ntt_contract_name(execution.report.ntt_contract)
            + "]: " + message);

    std::ostringstream diagnostic;
    diagnostic << "{\"status\":" << json_string(status)
        << ",\"required\":" << (require_instruction_verification ? "true" : "false")
        << ",\"verified\":" << (comparison.verified ? "true" : "false")
        << ",\"ntt_contract\":" << json_string(
            hpu::model::instruction_ntt_contract_name(execution.report.ntt_contract))
        << ",\"hardware_contract_verified\":"
            << (execution.report.hardware_contract_verified ? "true" : "false")
        << ",\"forward_boundary_conversions\":" << execution.report.forward_boundary_conversions
        << ",\"inverse_boundary_conversions\":" << execution.report.inverse_boundary_conversions
        << ",\"message\":" << json_string(message)
        << ",\"executed_instructions\":" << execution.report.executed_instructions
        << ",\"instruction_index\":" << execution.report.instruction_index
        << ",\"compared_words\":" << comparison.compared_words
        << ",\"object_id\":" << json_string(comparison.object_id)
        << ",\"object_word_index\":" << comparison.object_word_index
        << ",\"expected\":" << comparison.expected << ",\"actual\":" << comparison.actual << '}';
    append_json_members(request.oracle_report_json,
        "\"compiler_resources\":" + resource_report +
        ",\"host_instruction_model\":" + diagnostic.str() +
        ",\"host_instruction_model_verified\":" + (comparison.verified ? "true" : "false") +
        ",\"optimization_equivalence_verified\":" + (optimization_verified ? "true" : "false"));
    hpu::delivery::validate_application_package_request(request);
    return resource_report;
}

} // namespace hpu::seal_adapter::detail
