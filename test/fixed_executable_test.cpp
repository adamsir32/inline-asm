#include "assembler.hpp"
#include "executable.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Span = hpu::ExecutableDmaSpan;

void require(bool condition, const std::string& message)
{
    if (!condition) throw std::runtime_error(message);
}

std::size_t occurrences(const std::string& text, const std::string& needle)
{
    std::size_t count = 0;
    for (std::size_t pos = 0;
         (pos = text.find(needle, pos)) != std::string::npos;
         pos += needle.size()) ++count;
    return count;
}

std::vector<std::uint32_t> emitted_words(const std::string& text)
{
    const std::regex word(R"(\.word (0x[0-9A-Fa-f]+))");
    std::vector<std::uint32_t> result;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), word);
         it != std::sregex_iterator(); ++it) {
        result.push_back(static_cast<std::uint32_t>(
            std::stoul((*it)[1].str(), nullptr, 16)));
    }
    return result;
}

std::string fixed_function(const std::string& source, const std::string& stem)
{
    const auto begin = source.find("int hpu_run_" + stem + "(void) {");
    require(begin != std::string::npos, "fixed entry point is missing");
    return source.substr(begin);
}

void expect_rejected(
    const std::string& assembly, std::uint64_t capacity,
    const std::vector<Span>& spans, const std::string& reason)
{
    try {
        (void)hpu::render_fixed_executable_source(
            "invalid", hpu::assemble_source(assembly), capacity, spans);
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error("fixed renderer accepted " + reason);
}

struct Fixture {
    const std::string stem = "dma_register_fixture";
    const std::uint64_t capacity = 16384;
    const std::vector<hpu::EncodedInstruction> instructions = hpu::assemble_source(
        "dload x10, x11, p4, 2, 32\n"
        "pmodld 0\n"
        "dload x10, x11, p0, 1, 0\n"
        "dload x10, x11, p1, 1, 0\n"
        "pmul p2, p0, p1\n"
        "pmac p2, p0, p1\n"
        "pfree p0\n"
        "pfree p1\n"
        "dstore x10, x11, p2, 1\n"
        "dload x10, x11, p0, 1, 0\n"
        "dload x10, x11, p1, 3, 32\n"
        "pntt p0, p1, 0, 0, 0\n"
        "pfree p1\n"
        "dstore x10, x11, p0, 1\n"
        "dload x10, x11, p0, 1, 0\n"
        "dstore x10, x11, p0, 1\n"
        "dload x10, x11, p0, 1, 0\n"
        "dstore x10, x11, p0, 1\n"
        "pfree p4\n"
        "psync\n");
    std::vector<Span> spans;

    Fixture()
    {
        std::uint64_t offset = 0;
        for (const std::uint64_t count :
             {1, 512, 512, 512, 1024, 2, 1024, 1024, 1024, 512, 512}) {
            spans.push_back({offset, count});
            offset += count + 1;
        }
    }
};

void test_fixed_register_setup(const Fixture& fixture)
{
    const auto dynamic = hpu::render_executable_source(
        fixture.stem, fixture.instructions, fixture.capacity);
    const auto source = hpu::render_fixed_executable_source(
        fixture.stem, fixture.instructions, fixture.capacity, fixture.spans);
    require(source.compare(0, dynamic.size(), dynamic) == 0,
            "fixed renderer changed the dynamic entry point");
    const auto fixed = fixed_function(source, fixture.stem);
    require(occurrences(fixed, "__asm__ volatile") == 1,
            "fixed entry must contain one uninterrupted assembly block");
    require(occurrences(fixed, "li x10, ") == fixture.spans.size(),
            "fixed entry must set x10 before every DMA");
    require(occurrences(fixed, "li x11, ") == 6,
            "fixed entry must set x11 only initially or when length changes");
    require(fixed.find(": : : \"x10\", \"x11\", \"memory\"") != std::string::npos,
            "fixed assembly must declare x10/x11 and memory clobbers");
    require(fixed.find("return -4;") != std::string::npos,
            "fixed entry must retain the non-RISC-V result");

    const auto words = emitted_words(fixed);
    require(words.size() == fixture.instructions.size(),
            "fixed entry emitted the wrong number of HPU instructions");
    for (std::size_t i = 0; i < words.size(); ++i) {
        require(words[i] == fixture.instructions[i].word,
                "fixed entry changed HPU instruction order or encoding");
    }

    // Follow the emitted assembly in order, checking the actual register value
    // seen by every DMA. Counting setup instructions alone misses stale values.
    const std::regex emitted(R"(li x(10|11), ([0-9]+)|\.word (0x[0-9A-Fa-f]+))");
    const auto unset = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t x10 = unset, x11 = unset;
    std::size_t dma = 0, x10_since_dma = 0;
    for (auto it = std::sregex_iterator(fixed.begin(), fixed.end(), emitted);
         it != std::sregex_iterator(); ++it) {
        if ((*it)[1].matched) {
            const auto value = std::stoull((*it)[2].str());
            if ((*it)[1].str() == "10") {
                x10 = value;
                ++x10_since_dma;
            } else x11 = value;
        } else {
            const auto word = std::stoul((*it)[3].str(), nullptr, 16);
            if ((word & 0x7fU) != 0x2bU) continue;
            require(dma < fixture.spans.size(), "extra emitted DMA");
            require(x10_since_dma == 1, "x10 was not set exactly once before DMA");
            require(x10 == fixture.spans[dma].line_offset,
                    "DMA observes an incorrect x10 offset");
            require(x11 == fixture.spans[dma].line_count,
                    "DMA observes an incorrect retained x11 count");
            ++dma;
            x10_since_dma = 0;
        }
    }
    require(dma == fixture.spans.size(), "missing emitted DMA");
    require(occurrences(dynamic,
                "register uintptr_t hpu_rs1 __asm__(\"x10\")") == dma,
            "dynamic entry no longer sets x10 independently per DMA");
    require(occurrences(dynamic,
                "register uintptr_t hpu_rs2 __asm__(\"x11\")") == dma,
            "dynamic entry no longer accepts arbitrary lengths per DMA");
}

void test_rejected_spans_and_shapes()
{
    const std::string copy =
        "dload x10, x11, p0, 1, 0\n"
        "dstore x10, x11, p0, 1\n"
        "psync\n";
    expect_rejected(copy, 16, {{0, 0}, {1, 1}}, "zero-length span");
    expect_rejected(copy, 0, {{0, 1}, {1, 1}}, "zero memory capacity");
    expect_rejected(copy, 16, {{16, 1}, {1, 1}}, "offset at capacity");
    expect_rejected(copy, 16, {{15, 2}, {1, 2}}, "span crossing capacity");
    expect_rejected(copy, 16, {{0, 1}}, "missing span");
    expect_rejected(copy, 16, {{0, 1}, {1, 1}, {2, 1}}, "extra span");
    const auto beyond_u32 = std::uint64_t{1} << 32;
    expect_rejected(copy, beyond_u32 + 2,
                    {{beyond_u32, 1}, {1, 1}}, "offset outside the frozen ABI");
    expect_rejected(copy, beyond_u32 + 2,
                    {{0, beyond_u32}, {0, beyond_u32}}, "length outside the frozen ABI");
    expect_rejected(copy, 4096, {{0, 1025}, {1025, 1025}},
                    "regular-bank overflow");
    expect_rejected(copy, 16, {{0, 2}, {2, 1}}, "mismatched DSTORE length");
    expect_rejected(
        "dload x10, x11, p4, 2, 32\npfree p4\npsync\n",
        64, {{0, 33}}, "small-bank overflow");

    expect_rejected(
        "dload x10, x11, p63, 3, 0\npsync\n",
        64, {{0, 1}}, "one-line TYPE3 seed");
    expect_rejected(
        "dload x10, x11, p63, 3, 32\npsync\n",
        64, {{0, 3}}, "three-line TYPE3 seed");
    expect_rejected(
        "dload x10, x11, p40, 2, 0\npmodld 64\npsync\n",
        64, {{0, 4}}, "PMODLD outside the actual loaded table");
    expect_rejected(
        "dload x10, x11, p40, 2, 0\npsync\n",
        64, {{0, 5}}, "default four-line modulus reservation");
    (void)hpu::render_fixed_executable_source("wide_modulus_table",
        hpu::assemble_source("dload x10, x11, p40, 2, 0\npmodld 127\npsync\n"),
        64, {{0, 8}}, hpu::ExecutableTwiddleFormat::online_k8, 8);
    (void)hpu::render_fixed_executable_source("retained_high_object",
        hpu::assemble_source("dload x10, x11, p63, 1, 1\n"
            "dstore x10, x11, p63, 0\npsync\n"
            "dstore x10, x11, p63, 1\npsync\n"),
        64, {{0, 2}, {2, 2}, {4, 2}});

    const std::string two_sources =
        "dload x10, x11, p0, 1, 0\n"
        "dload x10, x11, p1, 1, 0\n";
    expect_rejected(
        two_sources + "padd p2, p0, p1\npfree p0\npfree p1\npfree p2\npsync\n",
        16, {{0, 1}, {1, 2}}, "unequal arithmetic source lengths");
    expect_rejected(
        two_sources + "pmac p1, p0, 2\npfree p0\npfree p1\npsync\n",
        16, {{0, 1}, {1, 2}}, "unequal PMAC accumulator length");
    (void)hpu::render_fixed_executable_source("replace_destination",
        hpu::assemble_source(two_sources + "pmul p1, p0, 2\npfree p0\npfree p1\npsync\n"),
        16, {{0, 1}, {1, 2}});
    for (const std::string direction : {"pntt", "pintt"}) {
        const auto transform = two_sources + direction +
            " p0, p1, 0, 0, 0\npfree p0\npfree p1\npsync\n";
        expect_rejected(transform, 16, {{0, 2}, {2, 1}},
                        "one-line stage parameters at N=128");
        expect_rejected(transform, 4096, {{0, 1024}, {1024, 512}},
                        "legacy full-stage twiddle payload");
        const auto archived = hpu::render_fixed_executable_source(
            "archived", hpu::assemble_source(transform), 4096,
            {{0, 1024}, {1024, 512}}, hpu::ExecutableTwiddleFormat::legacy_full_table);
        require(archived.find("li x11, 512") != std::string::npos,
                "archived full-table source cannot be reproduced for validation");
        expect_rejected(transform, 16, {{0, 1}, {1, 2}},
                        "NTT degree below 128");
        expect_rejected(transform, 16, {{0, 3}, {3, 2}},
                        "non-power-of-two NTT degree");
        for (const std::uint64_t lines : {2, 4, 16, 64, 1024}) {
            (void)hpu::render_fixed_executable_source(
                "k8_shape", hpu::assemble_source(transform), lines + 2,
                {{0, lines}, {lines, 2}});
        }
    }
    expect_rejected(
        "dload x10, x11, p0, 1, 0\n"
        "dload x10, x11, p1, 2, 1\n"
        "pntt p0, p1, 0, 0, 0\npfree p0\npfree p1\npsync\n",
        16, {{0, 2}, {2, 2}}, "modulus-table object used as stage parameters");

    // A span may end exactly at capacity; immediate multiplication has no
    // second resident operand whose length must be checked.
    const auto valid = hpu::assemble_source(
        "dload x10, x11, p0, 1, 0\n"
        "pmul p0, p0, 2\n"
        "dstore x10, x11, p0, 1\npsync\n");
    (void)hpu::render_fixed_executable_source("boundary", valid, 16,
                                             {{15, 1}, {15, 1}});
}

void test_no_dma()
{
    const auto instructions = hpu::assemble_source("psync\n");
    const auto source = hpu::render_fixed_executable_source(
        "no_dma", instructions, 1, {});
    const auto fixed = fixed_function(source, "no_dma");
    require(occurrences(fixed, "li x10, ") == 0
                && occurrences(fixed, "li x11, ") == 0,
            "no-DMA program sets unused DMA registers");
    const auto words = emitted_words(fixed);
    require(words.size() == 1 && words[0] == instructions[0].word,
            "no-DMA program did not preserve terminal PSYNC");
}

void write_file(const std::filesystem::path& path, const std::string& contents)
{
    std::ofstream output(path, std::ios::binary);
    output << contents;
    require(static_cast<bool>(output), "failed to write target fixture: " + path.string());
}

void write_target_fixture(const Fixture& fixture, const std::filesystem::path& root)
{
    std::filesystem::create_directories(root);
    auto header = hpu::render_executable_header(fixture.stem, fixture.spans.size());
    const std::string marker = "#ifdef __cplusplus\n}\n#endif\n\n#endif\n";
    const auto position = header.find(marker);
    require(position != std::string::npos, "unexpected generated header format");
    header.insert(position, "int hpu_run_" + fixture.stem + "(void);\n\n");
    write_file(root / (fixture.stem + ".h"), header);
    write_file(root / (fixture.stem + ".c"), hpu::render_fixed_executable_source(
        fixture.stem, fixture.instructions, fixture.capacity, fixture.spans));
    std::string manifest = "instruction_index,dma_index,line_offset,line_count,word_hex\n";
    for (const auto& dma : hpu::collect_dma_relocations(fixture.instructions)) {
        manifest += std::to_string(dma.instruction_index) + ','
            + std::to_string(dma.dma_index) + ','
            + std::to_string(fixture.spans[dma.dma_index].line_offset) + ','
            + std::to_string(fixture.spans[dma.dma_index].line_count) + ','
            + hpu::format_word_hex(fixture.instructions[dma.instruction_index].word) + '\n';
    }
    write_file(root / "dma_relocation_manifest.csv", manifest);
    std::string words;
    for (const auto& instruction : fixture.instructions) {
        for (unsigned bit = 32; bit != 0; --bit)
            words += ((instruction.word >> (bit - 1U)) & 1U) ? '1' : '0';
        words += '\n';
    }
    write_file(root / (fixture.stem + ".inst32"), words);
    write_file(root / "fixture.json",
        "{\n  \"program_source\": \"" + fixture.stem + ".c\",\n"
        "  \"program_header\": \"" + fixture.stem + ".h\",\n"
        "  \"program_inst32\": \"" + fixture.stem + ".inst32\",\n"
        "  \"dma_relocation_manifest\": \"dma_relocation_manifest.csv\"\n}\n");
}

} // namespace

int main(int argc, char** argv)
{
    try {
        require(argc <= 2, "usage: hpu_fixed_executable_test [FIXTURE_DIRECTORY]");
        const Fixture fixture;
        test_fixed_register_setup(fixture);
        test_rejected_spans_and_shapes();
        test_no_dma();
        if (argc == 2) write_target_fixture(fixture, argv[1]);
        std::cout << "Fixed executable DMA register tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Fixed executable DMA register tests failed: "
                  << error.what() << '\n';
        return 1;
    }
}
