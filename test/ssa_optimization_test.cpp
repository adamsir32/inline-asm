#include "hpu/compiler/ssa_optimization.hpp"

#include <algorithm>
#include <iostream>
#include <map>
#include <stdexcept>
#include <vector>

namespace {
namespace r = hpu::resource;
using hpu::compiler::ResolvedDma;
using hpu::runtime::AllocationKind;
using Image = hpu::runtime::HpuMemImage;
using Words = std::vector<std::uint32_t>;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
ResolvedDma reference(const Image& image, const std::string& name)
{
    return {0, name, image.allocation(name).span, {}, {}, 0};
}
void input(r::Operand operand, const Image& image, const std::string& name,
           hpu::DataType type = hpu::DataType::poly,
           hpu::DloadFlag flag = hpu::DloadFlag::regular_bank)
{
    r::input(operand, reference(image, name), type, flag);
}
void output(r::Operand operand, const Image& image, const std::string& name)
{
    r::output(operand, reference(image, name));
}

// Independent logical evaluator. It uses immutable SSA snapshots and actual
// memory writes, so an invalid load merge or write/read forwarding changes the
// observable result instead of being hidden by the resource implementation.
Words execute(const r::OperatorProgram& program, const Image& image)
{
    auto memory = image.words();
    std::vector<Words> values(program.values.size());
    Words modulus_table;
    std::uint32_t modulus = 65537;
    for (const auto& operation : program.operations) {
        const auto& instruction = operation.instruction;
        const auto kind = instruction.mnemonic;
        if (kind == hpu::Mnemonic::kDload || kind == hpu::Mnemonic::kDstore) {
            const auto& binding = *program.ports.at(*operation.port).binding;
            const auto begin = binding.span.line_offset * hpu::runtime::kHpuMemLineWords;
            const auto count = binding.span.line_count * hpu::runtime::kHpuMemLineWords;
            if (kind == hpu::Mnemonic::kDload) {
                auto& words = values.at(operation.outputs.at(0));
                words.assign(memory.begin() + begin, memory.begin() + begin + count);
                if (instruction.type == 1) modulus_table = words;
            } else {
                const auto& words = values.at(operation.inputs.at(0));
                require(words.size() == count, "logical evaluator store shape mismatch");
                std::copy(words.begin(), words.end(), memory.begin() + begin);
            }
        } else if (kind == hpu::Mnemonic::kPmodld) {
            modulus = modulus_table.at(static_cast<std::size_t>(instruction.mod_id));
        } else if (kind != hpu::Mnemonic::kPsync && kind != hpu::Mnemonic::kPfree) {
            const auto& left = values.at(operation.inputs.at(0));
            Words result(left.size());
            for (std::size_t i = 0; i < result.size(); ++i) {
                const auto right = instruction.imm8 >= 0 ? static_cast<std::uint64_t>(instruction.imm8)
                    : values.at(operation.inputs.at(1)).at(i);
                const auto a = static_cast<std::uint64_t>(left[i]);
                if (kind == hpu::Mnemonic::kPadd) result[i] = (a + right) % modulus;
                else if (kind == hpu::Mnemonic::kPsub) result[i] = (a + modulus - right % modulus) % modulus;
                else if (kind == hpu::Mnemonic::kPmul) result[i] = a * right % modulus;
                else if (kind == hpu::Mnemonic::kPmac)
                    result[i] = (a * right + values.at(operation.inputs.back()).at(i)) % modulus;
                else throw std::logic_error("unsupported test evaluator instruction");
            }
            values.at(operation.outputs.at(0)) = std::move(result);
        }
    }
    return memory;
}
Words allocation_words(const Words& memory, const Image& image, const std::string& id)
{
    const auto& allocation = image.allocation(id);
    const auto begin = allocation.span.line_offset * hpu::runtime::kHpuMemLineWords;
    return {memory.begin() + begin, memory.begin() + begin + allocation.word_count};
}
std::size_t accesses(const r::OperatorProgram& program, bool loads, const std::string& name = {})
{
    return std::count_if(program.ports.begin(), program.ports.end(), [&](const auto& port) {
        return port.load == loads && (name.empty() || port.binding->allocation_id == name);
    });
}
void context(const Image& image)
{
    input(r::make_operand("moduli"), image, "moduli", hpu::DataType::mod_ctx,
          hpu::DloadFlag::small_bank);
    r::select_modulus(0);
}

void test_ciphertext_input_reuse()
{
    Image image(8192);
    image.add("moduli", {65537, 65539, 65543, 65551}, AllocationKind::modulus_table);
    std::set<std::string> outputs;
    for (int q = 0; q < 4; ++q) {
        for (int component = 0; component < 4; ++component)
            image.add("input/" + std::to_string(q) + "/" + std::to_string(component),
                      Words(8192, q * 7 + component + 2), AllocationKind::ciphertext);
        for (int component = 0; component < 3; ++component) {
            const auto id = "output/" + std::to_string(q) + "/" + std::to_string(component);
            image.reserve(id, 8192, AllocationKind::output); outputs.insert(id);
        }
    }
    r::Recording recording;
    context(image);
    for (int q = 0; q < 4; ++q) {
        r::select_modulus(q);
        const auto a = r::make_operand("left"), b = r::make_operand("right"), t = r::make_operand("product");
        const auto load = [&](r::Operand operand, int c) {
            input(operand, image, "input/" + std::to_string(q) + "/" + std::to_string(c));
        };
        load(a, 0); load(b, 2); r::multiply(t, a, b);
        r::discard(a); r::discard(b); output(t, image, "output/" + std::to_string(q) + "/0");
        load(a, 0); load(b, 3); r::multiply(t, a, b);
        r::discard(a); r::discard(b);
        load(a, 1); load(b, 2); r::accumulate(t, a, b);
        output(t, image, "output/" + std::to_string(q) + "/1");
        load(a, 1); load(b, 3); r::multiply(t, a, b);
        output(t, image, "output/" + std::to_string(q) + "/2");
    }
    r::finish_program();
    const auto original = recording.finish();
    r::SsaOptimizationReport report;
    const auto optimized = r::optimize_operator_program(original, image, outputs, true, &report);
    require(accesses(original, true) == 33 && accesses(optimized, true) == 17,
            "CMULT Q4 did not reduce 32 input loads to 16");
    require(report.merged_reads == 16 && report.lifetime_hints_removed == 16,
            "CMULT optimization report lost duplicate reads or lifetime hints");
    const auto expected = execute(original, image);
    const auto actual = execute(optimized, image);
    for (const auto& name : outputs)
        require(allocation_words(actual, image, name) == allocation_words(expected, image, name),
                "CMULT load reuse changed an output");
}

void test_aliases_versions_and_forwarding()
{
    Image image(128);
    image.add("moduli", {65537}, AllocationKind::modulus_table);
    image.add("input", Words(256, 7), AllocationKind::ciphertext);
    image.add_alias("input/alias", "input");
    image.add("patch", Words(128, 3), AllocationKind::plaintext);
    image.reserve("mutable", 256, AllocationKind::workspace);
    for (const auto* name : {"old", "new", "alias_result"}) image.reserve(name, 256, AllocationKind::output);
    r::Recording recording;
    context(image);
    const auto first = r::make_operand("first"), second = r::make_operand("second"), sum = r::make_operand("sum");
    input(first, image, "input"); input(second, image, "input/alias");
    r::add(sum, first, second); output(sum, image, "alias_result");
    const auto old = r::make_operand("old"), patch = r::make_operand("patch"), after = r::make_operand("after");
    input(old, image, "mutable"); input(patch, image, "patch");
    auto partial = reference(image, "mutable"); partial.span.line_count = 2;
    r::output(patch, partial);
    input(after, image, "mutable");
    output(old, image, "old"); output(after, image, "new");
    r::finish_program();
    const auto original = recording.finish();
    const auto optimized = r::optimize_operator_program(original, image, {"old", "new", "alias_result"}, true);
    require(accesses(optimized, true, "input") == 1, "read-only alias identity was not canonicalized");
    require(accesses(optimized, true, "mutable") == 2, "partial write failed to invalidate a cached full-span read");
    const auto actual = execute(optimized, image), expected = execute(original, image);
    for (const auto* name : {"old", "new", "alias_result"})
        require(allocation_words(actual, image, name) == allocation_words(expected, image, name),
                "alias/version optimization changed output words");
    require(allocation_words(actual, image, "old") != allocation_words(actual, image, "new"),
            "version test did not observe the partial write");
}

void test_internal_write_read_elimination()
{
    Image image(64);
    image.add("moduli", {65537}, AllocationKind::modulus_table);
    image.add("input", Words(128, 11), AllocationKind::ciphertext);
    image.reserve("temporary", 128, AllocationKind::workspace);
    image.reserve("dead", 128, AllocationKind::output);
    image.reserve("answer", 128, AllocationKind::output);
    r::Recording recording; context(image);
    const auto a = r::make_operand("a"), t = r::make_operand("temporary"), b = r::make_operand("b");
    input(a, image, "input"); r::multiply_immediate(t, a, 2);
    output(t, image, "temporary"); input(b, image, "temporary");
    r::multiply_immediate(b, b, 3); output(b, image, "answer");
    r::multiply_immediate(t, a, 19); output(t, image, "dead");
    r::finish_program();
    const auto source = recording.finish();
    r::SsaOptimizationReport report;
    const auto optimized = r::optimize_operator_program(source, image, {"answer"}, true, &report);
    require(report.forwarded_reads == 1 && accesses(optimized, false) == 1 &&
            accesses(optimized, true, "temporary") == 0,
            "internal write/read roundtrip or unobservable output survived");
    const auto canonical = r::canonical_operator_program(source, image, {"answer"});
    require(accesses(canonical, false) == 3 && accesses(canonical, true, "temporary") == 1,
            "canonical baseline unexpectedly optimized IO");
    r::SsaOptimizationReport policy_report;
    const auto output_policy = r::output_root_operator_program(source, image, {"answer"}, &policy_report);
    require(accesses(output_policy, false) == 2 && accesses(output_policy, false, "dead") == 0 &&
            accesses(output_policy, false, "temporary") == 1 && accesses(output_policy, true, "temporary") == 1,
            "output-policy baseline removed a necessary roundtrip or retained a dead checkpoint");
    require(policy_report.merged_reads == 0 && policy_report.forwarded_reads == 0 &&
            !policy_report.reordered && policy_report.dead_operations == 2,
            "output-policy baseline performed a reuse or scheduling optimization");
    require(allocation_words(execute(output_policy, image), image, "answer") ==
            allocation_words(execute(canonical, image), image, "answer"),
            "output-policy elimination changed observable arithmetic");
    require(allocation_words(execute(optimized, image), image, "answer") == Words(128, 66),
            "store-to-load forwarding changed arithmetic");
}

void test_stage_parameter_identity_and_barriers()
{
    Image image(8192);
    image.add("moduli", {65537}, AllocationKind::modulus_table);
    for (int stage = 0; stage < 13; ++stage)
        image.add("stage/" + std::to_string(stage), Words(128, stage + 1), AllocationKind::twiddle);
    for (int i = 0; i < 3; ++i) {
        image.add("input/" + std::to_string(i), Words(8192, i + 1), AllocationKind::ciphertext);
        image.reserve("output/" + std::to_string(i), 8192, AllocationKind::output);
    }
    r::Recording recording; context(image);
    for (int i = 0; i < 3; ++i) {
        const auto data = r::make_operand("data"), table = r::make_operand("table");
        input(data, image, "input/" + std::to_string(i));
        for (int stage = 0; stage < 13; ++stage) {
            input(table, image, "stage/" + std::to_string(stage), hpu::DataType::twiddle_parameters);
            r::intt_stage(data, table, stage);
        }
        output(data, image, "output/" + std::to_string(i));
    }
    r::finish_program();
    const auto source = recording.finish();
    const auto optimized = r::optimize_operator_program(source, image, {}, true);
    std::size_t table_loads = 0;
    for (const auto& port : optimized.ports)
        if (port.load && port.binding->allocation_id.find("stage/") == 0) ++table_loads;
    require(table_loads == 13, "three INTTs retained 39 stage-parameter reads instead of 13");
    r::Recording barrier_recording; context(image);
    const auto first = r::make_operand("first"), second = r::make_operand("second");
    input(first, image, "input/0"); output(first, image, "output/0"); r::finish_program();
    input(second, image, "input/0"); output(second, image, "output/1"); r::finish_program();
    const auto barriers = r::optimize_operator_program(barrier_recording.finish(), image, {}, true);
    require(accesses(barriers, true, "input/0") == 2, "read merging crossed a synchronization boundary");
}

void test_single_immutable_modulus_identity()
{
    Image image(64);
    image.add("moduli", {65537}, AllocationKind::modulus_table);
    image.add_alias("moduli/alias", "moduli");
    image.add("different/moduli", {65539}, AllocationKind::modulus_table);
    image.add("mutable/moduli", {65543}, AllocationKind::modulus_table, false);
    r::Recording recording;
    const auto table = r::make_operand("table");
    input(table, image, "moduli", hpu::DataType::mod_ctx);
    input(table, image, "moduli/alias", hpu::DataType::mod_ctx);
    r::finish_program();
    const auto aliases = recording.finish();
    r::canonical_operator_program(aliases, image);
    auto different = aliases;
    different.bind_port(1, reference(image, "different/moduli"));
    bool rejected = false;
    try { r::canonical_operator_program(different, image); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "different native modulus tables were silently treated as one context");
    auto mutable_table = aliases;
    mutable_table.bind_port(0, reference(image, "mutable/moduli"));
    rejected = false;
    try { r::optimize_operator_program(mutable_table, image); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "mutable native modulus table was accepted without context versioning");
}

void test_overwritten_mutable_home_under_bank_pressure(bool same_bank = false)
{
    Image image(8192);
    image.add("mutable", Words(65536, 2), AllocationKind::workspace, false);
    // In the same-bank variant the replacement must be loaded eagerly. This
    // evicts the clean old version before its backing is overwritten and makes
    // store() recover from a failed attempt to freeze that old version while
    // the replacement occupies its only permitted bank.
    image.add("replacement", Words(65536, 9),
              same_bank ? AllocationKind::workspace : AllocationKind::ciphertext,
              !same_bank);
    image.add("pressure", Words(65536, 5), AllocationKind::workspace, false);
    image.reserve("old/output", 65536, AllocationKind::output);
    image.reserve("pressure/output", 65536, AllocationKind::output);
    r::Recording recording;
    const auto old = r::make_operand("old"), replacement = r::make_operand("replacement"),
               pressure = r::make_operand("pressure");
    // A full-bank old version and the pressure value both require Bank 0.
    // After replacement overwrites its backing, old must spill its own words
    // rather than silently reload the new version from the same DDR address.
    input(old, image, "mutable", hpu::DataType::poly, static_cast<hpu::DloadFlag>(1));
    input(replacement, image, "replacement", hpu::DataType::poly,
          static_cast<hpu::DloadFlag>(same_bank ? 1 : 2));
    output(replacement, image, "mutable");
    input(pressure, image, "pressure", hpu::DataType::poly, static_cast<hpu::DloadFlag>(1));
    output(pressure, image, "pressure/output");
    output(old, image, "old/output");
    r::finish_program();
    const auto native = r::optimize_operator_program(recording.finish(), image,
        {"mutable", "old/output", "pressure/output"});
    hpu::compiler::CompileOptions options;
    options.automatic_lifetimes = true;
    const auto compiled = hpu::compiler::compile_resources(r::bind_program(native, image), {}, {}, image, options);
    auto memory = compiled.image.words();
    std::vector<Words> objects(64);
    std::size_t dma_index = 0;
    for (const auto& encoded : compiled.instructions) {
        const auto& instruction = encoded.instruction;
        if (instruction.mnemonic == hpu::Mnemonic::kDload || instruction.mnemonic == hpu::Mnemonic::kDstore) {
            const auto span = compiled.dma.at(dma_index++).span;
            const auto begin = span.line_offset * hpu::runtime::kHpuMemLineWords;
            const auto length = span.line_count * hpu::runtime::kHpuMemLineWords;
            if (instruction.mnemonic == hpu::Mnemonic::kDload)
                objects.at(instruction.obj_id).assign(memory.begin() + begin, memory.begin() + begin + length);
            else {
                const auto& words = objects.at(instruction.obj_id);
                require(words.size() == length, "backend emitted an invalid store in home-version regression");
                std::copy(words.begin(), words.end(), memory.begin() + begin);
                if (instruction.type == 1) objects.at(instruction.obj_id).clear();
            }
        } else if (instruction.mnemonic == hpu::Mnemonic::kPfree) objects.at(instruction.idx0).clear();
    }
    const auto old_span = hpu::compiler::relocate_span(compiled, "old/output", image.allocation("old/output").span);
    const auto begin = old_span.line_offset * hpu::runtime::kHpuMemLineWords;
    require(std::all_of(memory.begin() + begin, memory.begin() + begin + 65536,
                        [](auto word) { return word == 2; }),
            "bank eviction reloaded overwritten backing instead of the old SSA version");
    require(compiled.report.spills >= 1, "bank-mask pressure regression did not force an old-version spill");
}

void test_modulus_context_and_named_ports()
{
    Image image(64);
    image.add("moduli", {17, 257}, AllocationKind::modulus_table);
    image.add("input", Words(128, 13), AllocationKind::ciphertext);
    image.reserve("q0", 128, AllocationKind::output);
    image.reserve("q1", 128, AllocationKind::output);
    r::Recording recording; context(image);
    const auto a = r::make_operand("a"), b = r::make_operand("b");
    r::read(a, hpu::DataType::poly, "input/value");
    r::multiply_immediate(b, a, 3); r::output(b, "result/q0");
    r::select_modulus(1); r::multiply_immediate(b, a, 5); r::output(b, "result/q1");
    r::finish_program();
    auto source = recording.finish();
    source.bind_named_ports({{"input/value", reference(image, "input")},
                            {"result/q0", reference(image, "q0")},
                            {"result/q1", reference(image, "q1")}});
    const auto optimized = r::optimize_operator_program(source, image, {}, true);
    const auto result = execute(optimized, image);
    require(allocation_words(result, image, "q0") == Words(128, 5) &&
            allocation_words(result, image, "q1") == Words(128, 65),
            "scheduling lost an explicit modulus context");
    require(source.ports.at(1).logical_binding == "input/value", "named port was rebound by ordinal");
    auto invalid = source;
    invalid.ports.at(1).binding->span.line_count = 0;
    bool rejected = false;
    try { r::optimize_operator_program(invalid, image); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "empty logical input range was accepted");
}

void test_required_workspace_is_materialized()
{
    Image image(1024);
    image.add("input", Words(8192, 7), AllocationKind::ciphertext);
    image.add("replacement", Words(8192, 9), AllocationKind::ciphertext);
    image.reserve("workspace/output", 8192, AllocationKind::workspace);
    image.reserve("workspace/later", 8192, AllocationKind::workspace);
    image.reserve("answer", 8192, AllocationKind::output);
    r::Recording recording;
    const auto first = r::make_operand("first"), second = r::make_operand("second");
    input(first, image, "input");
    output(first, image, "workspace/output");
    r::finish_program();
    input(second, image, "workspace/output");
    output(second, image, "answer");
    r::finish_program();
    input(first, image, "replacement");
    output(first, image, "workspace/later");
    r::finish_program();
    const std::set<std::string> required {"workspace/output", "workspace/later", "answer"};
    const auto native = r::optimize_operator_program(recording.finish(), image, required);
    require(accesses(native, false, "workspace/output") == 1,
            "SSA discarded an explicitly requested workspace output");
    hpu::compiler::CompileOptions options;
    options.automatic_lifetimes = true;
    options.prune_unused_buffers = true;
    options.required_outputs = required;
    const auto compiled = hpu::compiler::compile_resources(r::bind_program(native, image), {}, {}, image, options);
    const auto workspace = hpu::compiler::relocate_span(compiled, "workspace/output",
        image.allocation("workspace/output").span);
    const auto later = hpu::compiler::relocate_span(compiled, "workspace/later",
        image.allocation("workspace/later").span);
    require(workspace.line_offset != later.line_offset,
            "required workspace outputs with separate local lifetimes share a scratch allocation");
    bool written = false;
    for (const auto& dma : compiled.dma)
        if (compiled.instructions.at(dma.instruction_index).instruction.mnemonic == hpu::Mnemonic::kDstore
            && dma.span.line_offset == workspace.line_offset && dma.span.line_count == workspace.line_count)
            written = true;
    require(written, "storage lowering deferred an explicitly requested workspace output");
}
} // namespace

int main()
{
    try {
        test_ciphertext_input_reuse();
        test_aliases_versions_and_forwarding();
        test_internal_write_read_elimination();
        test_stage_parameter_identity_and_barriers();
        test_modulus_context_and_named_ports();
        test_overwritten_mutable_home_under_bank_pressure();
        test_overwritten_mutable_home_under_bank_pressure(true);
        test_single_immutable_modulus_identity();
        test_required_workspace_is_materialized();
        std::cout << "SSA memory optimization tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SSA memory optimization test failed: " << error.what() << '\n';
        return 1;
    }
}
