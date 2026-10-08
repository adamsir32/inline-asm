#include "hpu/compiler/resource_ir.hpp"
#include "assembler.hpp"
#include "executable.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using hpu::compiler::ResolvedDma;
using hpu::runtime::AllocationKind;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

template <class Function> void require_throws(Function function, const char* message)
{
    try { function(); } catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}

std::vector<ResolvedDma> bindings(
    const std::vector<hpu::EncodedInstruction>& instructions,
    const hpu::runtime::HpuMemImage& image,
    const std::vector<std::string>& ids)
{
    const auto relocations = hpu::collect_dma_relocations(instructions);
    require(relocations.size() == ids.size(), "incorrect test DMA recipe");
    std::vector<ResolvedDma> result;
    for (std::size_t index = 0; index < ids.size(); ++index) {
        result.push_back({relocations[index].instruction_index, ids[index],
            image.allocation(ids[index]).span, index / 3, "node", index});
    }
    return result;
}

// Independent pointwise replay checks that DMA rewrites preserve words and
// that spills never overwrite immutable inputs. NTT math is tested separately.
std::vector<std::uint32_t> replay(
    const std::vector<hpu::EncodedInstruction>& instructions,
    const std::vector<ResolvedDma>& dma,
    const hpu::runtime::HpuMemImage& image)
{
    auto memory = image.words();
    std::array<std::vector<std::uint32_t>, 8> objects;
    std::size_t dma_index = 0;
    constexpr std::uint32_t modulus = 65537;
    for (const auto& encoded : instructions) {
        const auto& op = encoded.instruction;
        switch (op.mnemonic) {
        case hpu::Mnemonic::kDload:
        case hpu::Mnemonic::kDstore: {
            const auto& entry = dma.at(dma_index++);
            const auto begin = entry.span.line_offset * 64;
            const auto count = entry.span.line_count * 64;
            if (op.mnemonic == hpu::Mnemonic::kDload) {
                objects[op.obj_id] = std::vector<std::uint32_t>(
                    memory.begin() + begin, memory.begin() + begin + count);
            } else {
                require(objects[op.obj_id].size() == count, "replay store length mismatch");
                std::copy(objects[op.obj_id].begin(), objects[op.obj_id].end(),
                    memory.begin() + begin);
                objects[op.obj_id].clear();
            }
            break;
        }
        case hpu::Mnemonic::kPadd:
        case hpu::Mnemonic::kPsub:
        case hpu::Mnemonic::kPmul:
        case hpu::Mnemonic::kPmac: {
            const auto left = objects[op.psrc1];
            const auto right = op.imm8 < 0 ? objects[op.psrc2]
                : std::vector<std::uint32_t>(left.size(), op.imm8);
            const auto accumulator = objects[op.pdst];
            require(!left.empty() && left.size() == right.size(), "replay operand mismatch");
            auto& destination = objects[op.pdst];
            destination.resize(left.size());
            for (std::size_t index = 0; index < left.size(); ++index) {
                std::uint64_t value = 0;
                switch (op.mnemonic) {
                case hpu::Mnemonic::kPadd: value = left[index] + right[index]; break;
                case hpu::Mnemonic::kPsub: value = left[index] + modulus - right[index]; break;
                default:
                    value = std::uint64_t(left[index]) * right[index];
                    if (op.mnemonic == hpu::Mnemonic::kPmac) value += accumulator[index];
                    break;
                }
                destination[index] = value % modulus;
            }
            break;
        }
        case hpu::Mnemonic::kPfree: objects[op.idx0].clear(); break;
        default: break;
        }
    }
    return memory;
}

std::vector<std::uint32_t> allocation_words(
    const std::vector<std::uint32_t>& memory,
    const hpu::runtime::HpuMemImage& image, const std::string& id)
{
    const auto& allocation = image.allocation(id);
    const auto begin = memory.begin() + allocation.span.line_offset * 64;
    return {begin, begin + allocation.word_count};
}

void test_scratch_reuse()
{
    hpu::runtime::HpuMemImage image(64);
    image.add("moduli", {65537}, AllocationKind::modulus_table);
    image.add("a", std::vector<std::uint32_t>(128, 3), AllocationKind::ciphertext);
    image.add("b", std::vector<std::uint32_t>(128, 5), AllocationKind::ciphertext);
    image.add_alias("b_alias", "b");
    image.reserve("scratch/a", 128, AllocationKind::workspace);
    image.reserve("output/a", 128, AllocationKind::output);
    image.reserve("scratch/b", 128, AllocationKind::workspace);
    image.reserve("output/b", 128, AllocationKind::output);
    image.add("scratch/initial", std::vector<std::uint32_t>(128, 9), AllocationKind::workspace, false);
    image.reserve("output/initial", 128, AllocationKind::output);
    const auto instructions = hpu::assemble_source(
        "dload x10, x11, p4, 2, 1\npmodld 0\n"
        "dload x10, x11, p0, 1, 0\ndload x10, x11, p1, 1, 0\n"
        "padd p2, p0, p1\ndstore x10, x11, p2, 0\npfree p0\npfree p1\n"
        "dload x10, x11, p0, 1, 0\npmul p0, p0, 2\ndstore x10, x11, p0, 0\n"
        "dload x10, x11, p0, 1, 0\ndload x10, x11, p1, 1, 0\n"
        "padd p2, p0, p1\ndstore x10, x11, p2, 0\npfree p0\npfree p1\n"
        "dload x10, x11, p0, 1, 0\npmul p0, p0, 3\ndstore x10, x11, p0, 0\n"
        "dload x10, x11, p0, 1, 0\npmul p0, p0, 2\ndstore x10, x11, p0, 0\n"
        "pfree p4\npsync\n");
    const auto dma = bindings(instructions, image,
        {"moduli", "a", "b_alias", "scratch/a", "scratch/a", "output/a",
         "a", "b", "scratch/b", "scratch/b", "output/b", "scratch/initial", "output/initial"});
    hpu::compiler::CompileOptions optimized_options;
    optimized_options.buffer_layouts.emplace("a", hpu::compiler::MemoryLayout::coefficient_natural);
    optimized_options.buffer_layouts.emplace("b", hpu::compiler::MemoryLayout::coefficient_natural);
    const auto optimized = hpu::compiler::optimize_resources(instructions, dma, image, optimized_options);
    hpu::compiler::CompileOptions options;
    options.optimization = hpu::compiler::OptimizationMode::no_opt;
    options.buffer_layouts = optimized_options.buffer_layouts;
    const auto baseline = hpu::compiler::optimize_resources(instructions, dma, image, options);
    require(baseline.instructions.size() == instructions.size()
            && baseline.image.words() == image.words()
            && baseline.image.capacity_lines() == image.capacity_lines(), "NoOpt altered the image");
    for (std::size_t index = 0; index < instructions.size(); ++index) {
        require(baseline.instructions[index].word == instructions[index].word, "NoOpt altered instruction words");
    }
    require(optimized.report.deferred_scratch_stores == 2
            && optimized.dma.size() + 4 == dma.size(), "unique scratch store/load was not removed");
    require(optimized.image.used_lines() < image.used_lines()
            && optimized.report.scratch_arena_lines == 2, "nonoverlapping scratch buffers were not reused");
    require(optimized.image.contains("scratch/initial"), "initial-memory-dependent scratch was reused");
    for (const auto* resources : {&baseline, &optimized}) {
        for (const auto& buffer : resources->buffers) {
            if (buffer.logical_id == "a" || buffer.logical_id == "b")
                require(buffer.layout == hpu::compiler::MemoryLayout::coefficient_natural,
                        "explicit layout annotation was lost during resource compilation");
            else require(buffer.layout == hpu::compiler::MemoryLayout::unspecified,
                         "compiler invented a layout for an unannotated buffer");
        }
    }
    const auto first = hpu::compiler::relocate_span(optimized, "scratch/a", image.allocation("scratch/a").span);
    const auto second = hpu::compiler::relocate_span(optimized, "scratch/b", image.allocation("scratch/b").span);
    require(first.line_offset == second.line_offset, "disjoint scratch lifetimes did not share DDR");
    const auto original_words = replay(instructions, dma, image);
    const auto optimized_words = replay(optimized.instructions, optimized.dma, optimized.image);
    for (const std::string id : {"output/a", "output/b", "output/initial", "a", "b"}) {
        require(allocation_words(original_words, image, id)
                    == allocation_words(optimized_words, optimized.image, id), "optimized words differ from baseline");
    }
    const auto again = hpu::compiler::optimize_resources(instructions, dma, image, optimized_options);
    require(again.image.words() == optimized.image.words()
            && hpu::compiler::render_resource_report(again)
                == hpu::compiler::render_resource_report(optimized), "resource allocation is nondeterministic");
    for (std::size_t index = 0; index < again.instructions.size(); ++index) {
        require(again.instructions[index].word == optimized.instructions[index].word,
                "slot scheduling is nondeterministic");
    }
    auto invalid = dma;
    invalid.front().span.line_count += 1;
    require_throws([&] { hpu::compiler::import_resolved_program(instructions, invalid, image); },
                   "DMA range outside allocation accepted");
    options.buffer_layouts.emplace("absent", hpu::compiler::MemoryLayout::coefficient_bit_reversed);
    require_throws([&] { hpu::compiler::optimize_resources(instructions, dma, image, options); },
                   "layout annotation for an absent buffer was accepted");
    options.buffer_layouts.erase("absent");
    options.buffer_layouts["a"] = static_cast<hpu::compiler::MemoryLayout>(100);
    require_throws([&] { hpu::compiler::optimize_resources(instructions, dma, image, options); },
                   "invalid layout annotation was accepted");
}

void test_dirty_spill()
{
    hpu::runtime::HpuMemImage image(64);
    image.add("moduli", {65537}, AllocationKind::modulus_table);
    for (int index = 0; index < 6; ++index) {
        image.add("input/" + std::to_string(index), std::vector<std::uint32_t>(128, index + 1),
            AllocationKind::ciphertext);
    }
    image.reserve("output", 128, AllocationKind::output);
    const auto instructions = hpu::assemble_source(
        "dload x10, x11, p4, 2, 1\npmodld 0\n"
        "dload x10, x11, p0, 1, 0\npmul p0, p0, 2\n"
        "dload x10, x11, p1, 1, 0\ndload x10, x11, p2, 1, 0\n"
        "dload x10, x11, p3, 1, 0\ndload x10, x11, p5, 1, 0\n"
        "dload x10, x11, p6, 1, 0\n"
        "padd p1, p1, p2\npfree p2\npadd p3, p3, p5\npfree p5\n"
        "padd p6, p6, p1\npfree p1\npadd p0, p0, p3\npfree p3\n"
        "padd p0, p0, p6\npfree p6\ndstore x10, x11, p0, 0\npfree p4\npsync\n");
    const auto dma = bindings(instructions, image,
        {"moduli", "input/0", "input/1", "input/2", "input/3", "input/4", "input/5", "output"});
    const auto optimized = hpu::compiler::optimize_resources(instructions, dma, image);
    require(optimized.report.spills >= 1 && optimized.report.reloads >= 1,
            "dirty victim did not spill and reload");
    require(optimized.report.peak_regular_banks <= 5 && optimized.report.peak_object_slots <= 8,
            "scheduler confused logical slots and physical banks");
    for (const auto& entry : optimized.dma) {
        require(entry.operation_id == "node" && entry.operation_index.has_value(),
                "spill/reload changed graph operation provenance");
    }
    const auto original = replay(instructions, dma, image);
    const auto actual = replay(optimized.instructions, optimized.dma, optimized.image);
    require(allocation_words(original, image, "output")
                == allocation_words(actual, optimized.image, "output"), "spill corrupted arithmetic");
    for (int index = 0; index < 6; ++index) {
        const auto id = "input/" + std::to_string(index);
        require(allocation_words(image.words(), image, id)
                    == allocation_words(actual, optimized.image, id), "dirty spill overwrote immutable input");
    }
    hpu::compiler::CompileOptions options;
    options.optimization = hpu::compiler::OptimizationMode::no_opt;
    require_throws([&] { hpu::compiler::optimize_resources(instructions, dma, image, options); },
                   "NoOpt accepted six simultaneous regular-bank objects");
}

void test_stage_bank_and_multiple_readers()
{
    hpu::runtime::HpuMemImage image(32);
    image.add("moduli", {65537}, AllocationKind::modulus_table);
    for (int index = 0; index < 5; ++index) {
        image.add("input/" + std::to_string(index), std::vector<std::uint32_t>(128, index + 1),
            AllocationKind::twiddle);
    }
    image.reserve("scratch", 128, AllocationKind::workspace);
    image.reserve("output/1", 128, AllocationKind::output);
    image.reserve("output/2", 128, AllocationKind::output);
    const auto stages = hpu::assemble_source(
        "dload x10, x11, p4, 2, 1\npmodld 0\n"
        "dload x10, x11, p0, 1, 0\ndload x10, x11, p1, 1, 0\n"
        "dload x10, x11, p2, 1, 0\ndload x10, x11, p3, 1, 0\n"
        "dload x10, x11, p5, 1, 0\npntt p0, p1, 0, 0, 0\n"
        "pfree p1\npmul p2, p2, p3\npfree p3\npmul p2, p2, p5\npfree p5\n"
        "pmul p0, p0, p2\npfree p2\ndstore x10, x11, p0, 0\npfree p4\npsync\n");
    const auto stage_dma = bindings(stages, image,
        {"moduli", "input/0", "input/1", "input/2", "input/3", "input/4", "output/1"});
    const auto staged = hpu::compiler::optimize_resources(stages, stage_dma, image);
    require(staged.report.peak_regular_banks <= 5 && staged.report.reloads >= 1,
            "NTT out-of-place output bank was not reserved");
    const auto multiple = hpu::assemble_source(
        "dload x10, x11, p4, 2, 1\npmodld 0\n"
        "dload x10, x11, p0, 1, 0\npmul p0, p0, 2\ndstore x10, x11, p0, 0\n"
        "dload x10, x11, p0, 1, 0\npmul p0, p0, 2\ndstore x10, x11, p0, 0\n"
        "dload x10, x11, p0, 1, 0\npmul p0, p0, 3\ndstore x10, x11, p0, 0\n"
        "pfree p4\npsync\n");
    const auto multi_dma = bindings(multiple, image,
        {"moduli", "input/0", "scratch", "scratch", "output/1", "scratch", "output/2"});
    const auto optimized = hpu::compiler::optimize_resources(multiple, multi_dma, image);
    require(optimized.report.deferred_scratch_stores == 0,
            "writeback was removed despite multiple DDR readers");
    const auto expected = replay(multiple, multi_dma, image);
    const auto actual = replay(optimized.instructions, optimized.dma, optimized.image);
    for (const std::string id : {"output/1", "output/2"}) {
        require(allocation_words(expected, image, id)
                    == allocation_words(actual, optimized.image, id), "multiple-reader memory semantics changed");
    }
    auto readonly_dma = multi_dma;
    readonly_dma[2].allocation_id = "input/0";
    readonly_dma[2].span = image.allocation("input/0").span;
    require_throws([&] { hpu::compiler::optimize_resources(multiple, readonly_dma, image); },
                   "DSTORE to immutable allocation accepted");
}

void test_destination_shape_change()
{
    hpu::runtime::HpuMemImage image(16);
    image.add("moduli", {65537}, AllocationKind::modulus_table);
    image.add("a", std::vector<std::uint32_t>(128, 3), AllocationKind::ciphertext);
    image.add("b", std::vector<std::uint32_t>(128, 4), AllocationKind::ciphertext);
    image.add("old_destination", std::vector<std::uint32_t>(64, 7), AllocationKind::constant);
    image.reserve("output", 128, AllocationKind::output);
    const auto instructions = hpu::assemble_source(
        "dload x10, x11, p4, 2, 1\npmodld 0\n"
        "dload x10, x11, p0, 1, 0\ndload x10, x11, p1, 1, 0\n"
        "dload x10, x11, p2, 0, 0\npadd p2, p0, p1\n"
        "pfree p0\npfree p1\ndstore x10, x11, p2, 0\npfree p4\npsync\n");
    const auto dma = bindings(instructions, image,
        {"moduli", "a", "b", "old_destination", "output"});
    const auto optimized = hpu::compiler::optimize_resources(instructions, dma, image);
    const auto original = replay(instructions, dma, image);
    const auto actual = replay(optimized.instructions, optimized.dma, optimized.image);
    require(allocation_words(original, image, "output")
                == allocation_words(actual, optimized.image, "output"),
            "arithmetic destination did not inherit source shape");
}

void test_partial_initialization_and_overlapping_lifetimes()
{
    hpu::runtime::HpuMemImage image(32);
    image.add("moduli", {65537}, AllocationKind::modulus_table);
    image.add("input", std::vector<std::uint32_t>(128, 3), AllocationKind::ciphertext);
    image.reserve("scratch/a", 128, AllocationKind::workspace);
    image.reserve("scratch/b", 128, AllocationKind::workspace);
    image.reserve("scratch/partial", 256, AllocationKind::workspace);
    image.reserve("output/a", 128, AllocationKind::output);
    image.reserve("output/b", 128, AllocationKind::output);
    image.reserve("output/partial", 128, AllocationKind::output);
    const auto instructions = hpu::assemble_source(
        "dload x10, x11, p4, 2, 1\npmodld 0\n"
        "dload x10, x11, p0, 1, 0\npmul p0, p0, 2\ndstore x10, x11, p0, 0\n"
        "dload x10, x11, p0, 1, 0\npmul p0, p0, 3\ndstore x10, x11, p0, 0\n"
        "dload x10, x11, p0, 1, 0\ndload x10, x11, p1, 1, 0\n"
        "padd p2, p0, p1\npfree p0\npfree p1\ndstore x10, x11, p2, 0\n"
        "dload x10, x11, p0, 1, 0\ndstore x10, x11, p0, 0\n"
        "dload x10, x11, p0, 1, 0\ndstore x10, x11, p0, 0\n"
        "dload x10, x11, p0, 1, 0\ndstore x10, x11, p0, 0\npfree p4\npsync\n");
    auto dma = bindings(instructions, image,
        {"moduli", "input", "scratch/a", "input", "scratch/b", "scratch/a", "scratch/b",
         "output/a", "scratch/a", "output/b", "input", "scratch/partial", "scratch/partial", "output/partial"});
    dma[11].span.line_count = 2;
    dma[12].span.line_offset += 2;
    dma[12].span.line_count = 2;
    const auto optimized = hpu::compiler::optimize_resources(instructions, dma, image);
    const auto a = hpu::compiler::relocate_span(optimized, "scratch/a", image.allocation("scratch/a").span);
    const auto b = hpu::compiler::relocate_span(optimized, "scratch/b", image.allocation("scratch/b").span);
    require(a.line_offset + a.line_count <= b.line_offset || b.line_offset + b.line_count <= a.line_offset,
            "overlapping scratch lifetimes shared bytes");
    require(optimized.image.contains("scratch/partial"),
            "partially initialized workspace was reused despite unwritten reads");
    const auto expected = replay(instructions, dma, image);
    const auto actual = replay(optimized.instructions, optimized.dma, optimized.image);
    for (const std::string id : {"output/a", "output/b", "output/partial"}) {
        require(allocation_words(expected, image, id)
                    == allocation_words(actual, optimized.image, id), "partial initialization changed output");
    }
}

} // namespace

int main()
{
    try {
        test_scratch_reuse();
        test_dirty_spill();
        test_stage_bank_and_multiple_readers();
        test_destination_shape_change();
        test_partial_initialization_and_overlapping_lifetimes();
        std::cout << "Compiler resource IR, DDR reuse and bank scheduling tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Compiler resource test failed: " << error.what() << '\n';
        return 1;
    }
}
