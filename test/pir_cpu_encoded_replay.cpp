// Replay target-C generated stage1 DDR images with the fixed encoded HPU program.
#include "hpu/model/instruction_executor.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::vector<std::uint32_t> read_words(const std::string &path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("cannot open " + path);
    const auto size = input.tellg();
    if (size <= 0 || size % 4 != 0) throw std::runtime_error("invalid u32 image size");
    input.seekg(0);
    std::vector<std::uint32_t> result(static_cast<std::size_t>(size) / 4);
    input.read(reinterpret_cast<char *>(result.data()), size);
    if (!input) throw std::runtime_error("truncated image " + path);
    return result;
}

std::vector<hpu::EncodedInstruction> read_program(const std::string &path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open " + path);
    std::vector<hpu::EncodedInstruction> result;
    std::string bits;
    while (input >> bits) {
        if (bits.size() != 32 || bits.find_first_not_of("01") != std::string::npos)
            throw std::runtime_error("invalid inst32 word");
        hpu::EncodedInstruction instruction;
        instruction.word = static_cast<std::uint32_t>(std::stoul(bits, nullptr, 2));
        result.push_back(instruction);
    }
    if (result.empty()) throw std::runtime_error("empty HPU program");
    return result;
}

std::uint32_t read_u32(std::istream &input) {
    unsigned char bytes[4];
    input.read(reinterpret_cast<char *>(bytes), 4);
    if (!input) throw std::runtime_error("truncated DMA span table");
    return static_cast<std::uint32_t>(bytes[0]) |
           (static_cast<std::uint32_t>(bytes[1]) << 8U) |
           (static_cast<std::uint32_t>(bytes[2]) << 16U) |
           (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

std::vector<hpu::runtime::HpuMemSpan> read_dma(const std::string &path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("cannot open " + path);
    const auto size = input.tellg();
    if (size <= 0 || size % 8 != 0) throw std::runtime_error("invalid DMA span table");
    input.seekg(0);
    std::vector<hpu::runtime::HpuMemSpan> result;
    for (std::streamoff index = 0; index < size / 8; ++index)
        result.push_back({read_u32(input), read_u32(input)});
    return result;
}

} // namespace

int main(int argc, char **argv) {
    try {
        if (argc != 5) throw std::runtime_error(
            "usage: pir_cpu_encoded_replay INST32 DMA_U32X2 PATCHED_IMAGE EXPECTED_EXECUTED");
        const auto program = read_program(argv[1]);
        const auto dma = read_dma(argv[2]);
        auto patched = read_words(argv[3]);
        const auto expected = read_words(argv[4]);
        if (patched.size() != expected.size() || patched.size() % 64 != 0)
            throw std::runtime_error("executed image length mismatch");
        hpu::runtime::HpuMemImage image(patched.size() / 64);
        image.add("stage1-complete-image", patched,
                  hpu::runtime::AllocationKind::workspace, false);
        if (image.words() != patched)
            throw std::runtime_error("model source image differs from CPU output");
        const auto execution = hpu::model::execute_instructions(program, dma, image);
        if (!execution.report.completed())
            throw std::runtime_error("encoded HPU model: " + execution.report.message);
        if (execution.report.executed_instructions != program.size())
            throw std::runtime_error("encoded HPU instruction count mismatch");
        if (execution.words.size() != expected.size())
            throw std::runtime_error("model output length mismatch");
        for (std::size_t index = 0; index < expected.size(); ++index)
            if (execution.words[index] != expected[index])
                throw std::runtime_error("model output mismatch at word " +
                                         std::to_string(index));
        std::cout << "PIR_CPU_ENCODED_REPLAY_PASS instructions=" << program.size()
                  << " dma=" << dma.size() << " compared_words=" << expected.size()
                  << '\n';
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "PIR_CPU_ENCODED_REPLAY_FAIL " << error.what() << '\n';
        return 1;
    }
}
