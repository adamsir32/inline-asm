#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "instruction.hpp"

namespace hpu {

inline constexpr std::uint32_t kSmallBankLineCount = 32;
inline constexpr std::uint32_t kDefaultModTableLineCount = 4;

struct DmaRelocation {
    std::size_t instruction_index = 0;
    std::size_t dma_index = 0;
    Mnemonic direction = Mnemonic::kDload;
    std::uint8_t object_id = 0;
    std::uint8_t type_or_release = 0;
    std::uint8_t flag = 0;
};

// Resolved CPU-side DMA operands, in instruction order. Wider fields allow the
// emitter to reject values that cannot be represented by the uint32 span ABI.
struct ExecutableDmaSpan {
    std::uint64_t line_offset = 0;
    std::uint64_t line_count = 0;
};

// The legacy variant exists to reproduce and validate archived V1 sources.
// Current code generators use online_k8 and never select a legacy fallback.
enum class ExecutableTwiddleFormat { online_k8, legacy_full_table };

std::string c_identifier(std::string value);
std::vector<DmaRelocation> collect_dma_relocations(
    const std::vector<EncodedInstruction>& encoded);
void validate_executable_program(
    const std::vector<EncodedInstruction>& encoded);
std::string render_executable_header(
    const std::string& stem,
    std::size_t dma_count);
std::string render_executable_source(
    const std::string& stem,
    const std::vector<EncodedInstruction>& encoded,
    std::uint64_t hpu_mem_line_count,
    std::uint32_t mod_table_lines = kDefaultModTableLineCount);
// Keeps the dynamic-spans entry unchanged and adds a fixed hpu_run_* entry.
// Validate fixed spans before emission; only length changes write CPU x11.
std::string render_fixed_executable_source(
    const std::string& stem,
    const std::vector<EncodedInstruction>& encoded,
    std::uint64_t hpu_mem_line_count,
    const std::vector<ExecutableDmaSpan>& spans,
    ExecutableTwiddleFormat twiddle_format = ExecutableTwiddleFormat::online_k8,
    std::uint32_t mod_table_lines = kDefaultModTableLineCount);
std::string render_dma_manifest(
    const std::vector<EncodedInstruction>& encoded);

}  // namespace hpu
