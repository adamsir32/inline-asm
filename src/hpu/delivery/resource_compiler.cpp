#include "hpu/delivery/resource_compiler.hpp"
#include "executable.hpp"
#include "assembler.hpp"

#include <cctype>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace hpu::delivery {
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

std::string csv_field(const std::string& value)
{
    std::string result = "\"";
    for (char c : value) {
        if (c == '\"') result += '\"';
        result += c;
    }
    return result + '\"';
}

void render_program(ProgramArtifacts& program, const hpu::compiler::CompiledResources& resources)
{
    const auto identifier = hpu::c_identifier(program.stem);
    program.header = hpu::render_executable_header(program.stem, resources.dma.size());
    const std::string marker = "#ifdef __cplusplus\n}\n#endif\n\n#endif\n";
    const auto position = program.header.find(marker);
    if (position == std::string::npos) throw std::logic_error("unexpected runtime header layout");
    program.header.insert(position, "int hpu_run_" + identifier + "(void);\n\n");
    program.instructions = resources.instructions;
    std::vector<hpu::ExecutableDmaSpan> fixed_spans;
    std::ostringstream manifest, assembly;
    manifest << "instruction_index,dma_index,operation_index,operation_id,operation_dma_index,direction,object_slot,type_or_release,flag,allocation_id,line_offset,line_count,word_hex,normalized_asm,cp\n";
    std::map<std::string, std::size_t> operation_indices;
    program.ordered_dma_spans.clear();
    program.resource_dma = resources.dma;
    for (std::size_t i = 0; i < resources.dma.size(); ++i) {
        const auto& dma = resources.dma[i];
        const auto& encoded = resources.instructions.at(dma.instruction_index);
        const auto& inst = encoded.instruction;
        const bool load = inst.mnemonic == hpu::Mnemonic::kDload;
        const auto operation_dma_index = operation_indices[dma.operation_id]++;
        program.resource_dma[i].operation_dma_index = operation_dma_index;
        fixed_spans.push_back({dma.span.line_offset, dma.span.line_count});
        manifest << dma.instruction_index << ',' << i << ',';
        if (dma.operation_index) manifest << *dma.operation_index;
        manifest << ',' << csv_field(dma.operation_id) << ',' << operation_dma_index << ','
                 << (load ? "dload" : "dstore") << ',' << unsigned(inst.obj_id) << ',' << unsigned(inst.type)
                 << ',' << (load ? unsigned(inst.dma_flag) : 0) << ',' << csv_field(dma.allocation_id)
                 << ',' << dma.span.line_offset << ',' << dma.span.line_count << ','
                 << hpu::format_word_hex(encoded.word) << ',' << csv_field(encoded.normalized_asm) << ',' << unsigned(inst.cp) << '\n';
        program.ordered_dma_spans.push_back(dma.span);
    }
    program.source = hpu::render_fixed_executable_source(
        program.stem, program.instructions, resources.image.capacity_lines(), fixed_spans);
    program.resolved_dma_manifest = manifest.str();
    for (const auto& instruction : program.instructions) assembly << instruction.normalized_asm << '\n';
    program.assembly = assembly.str();
}

} // namespace

std::string compile_application_resources(
    hpu::delivery::ApplicationPackageRequest& request,
    hpu::runtime::HpuMemImage& image, bool optimize, bool require_instruction_verification,
    hpu::model::InstructionNttContract instruction_ntt_contract)
{
    if (request.program.resources_compiled)
        throw std::invalid_argument("application resources are already compiled");
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
    options.dma_cache_policy = hpu::compiler::DmaCachePolicy::twiddle_loads_only;
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
    if (!request.program.operator_program)
        throw std::invalid_argument("resource compilation requires native operator IR");
    const auto& dma = request.program.resource_dma;
    if (dma.size() != request.program.ordered_dma_spans.size())
        throw std::invalid_argument("native operator DMA ports are incomplete");
    for (std::size_t i = 0; i < dma.size(); ++i) {
        const auto span = request.program.ordered_dma_spans[i];
        if (span.line_offset != dma[i].span.line_offset || span.line_count != dma[i].span.line_count)
            throw std::invalid_argument("native operator DMA port disagrees with baseline span");
    }
    auto source = *request.program.operator_program;
    source.bind_ports(dma);
    auto program = hpu::resource::bind_program(source, image);
    auto resources = hpu::compiler::compile_resources(std::move(program),
        request.program.instructions, dma, image, options);
    for (auto& output : request.outputs) {
        const auto& allocation = containing_allocation(image, output.destination);
        output.destination = hpu::compiler::relocate_span(resources, allocation.id, output.destination);
    }

    render_program(request.program, resources);
    request.program.dma_cache_policy = options.dma_cache_policy;

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
    request.program.resources_compiled = true;
    hpu::delivery::validate_application_package_request(request);
    return resource_report;
}

} // namespace hpu::delivery
