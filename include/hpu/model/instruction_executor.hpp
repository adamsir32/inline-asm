#pragma once

#include "hpu/runtime/memory_image.hpp"
#include "instruction.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hpu::model {

enum class InstructionExecutionStatus {
    complete,
    invalid_program,
    unsupported_contract
};

// These are software execution contracts, not identified RTL revisions.
// All consume the supplied DMA words and tables unchanged. The raw contract
// preserves the loader/BF/P stages from the historical reference model; its
// increasing-m forward chain requires bit-reversed coefficients before stage 0.
enum class InstructionNttContract {
    // Functional natural coefficient boundaries for complete transforms.
    // Forward stage 0 enters the reference DIT layout; the last inverse stage
    // leaves it. Twist and normalization remain explicit pointwise PMULs.
    // Intermediate stages are the reference schedule, not a verified RTL trace.
    natural_coefficient_boundaries,
    legacy_raw_stage
};

const char* instruction_ntt_contract_name(InstructionNttContract contract) noexcept;

struct InstructionExecutionOptions {
    // New application images use natural coefficient order at both transform
    // boundaries. The raw profile remains available to replay historical images.
    // Boundary permutations model functional behavior, not RTL address cycles.
    // PMUL always consumes data and constants at the same physical index.
    InstructionNttContract ntt_contract =
        InstructionNttContract::natural_coefficient_boundaries;
    std::size_t regular_bank_count = 5;
    std::size_t regular_bank_lines = 1024;
};

struct InstructionExecutionReport {
    InstructionExecutionStatus status = InstructionExecutionStatus::invalid_program;
    InstructionNttContract ntt_contract =
        InstructionNttContract::natural_coefficient_boundaries;
    // A software replay cannot establish a hardware contract. Actual RTL or
    // target execution evidence must be tracked separately by the caller.
    bool hardware_contract_verified = false;
    std::size_t forward_boundary_conversions = 0;
    std::size_t inverse_boundary_conversions = 0;
    std::size_t instruction_index = 0;
    std::size_t executed_instructions = 0;
    std::size_t peak_regular_banks = 0;
    std::size_t peak_resident_objects = 0;
    std::size_t loaded_words = 0;
    std::size_t stored_words = 0;
    std::string message;

    // Completion alone is not an oracle comparison or proof of RTL execution.
    bool completed() const noexcept { return status == InstructionExecutionStatus::complete; }
};

struct InstructionExecutionResult {
    std::vector<std::uint32_t> words;
    InstructionExecutionReport report;
};

// Decodes instruction.word independently of instruction/normalized_asm metadata.
// Each custom1 consumes the next concrete DDR span, in encoded program order.
// Arithmetic consumes the actual DMA payload, including supplied twiddle words.
// DSTORE releases its source, and transform stages require an extra output bank.
InstructionExecutionResult execute_instructions(
    const std::vector<hpu::EncodedInstruction>& instructions,
    const std::vector<hpu::runtime::HpuMemSpan>& ordered_dma_spans,
    const hpu::runtime::HpuMemImage& image,
    InstructionExecutionOptions options = {});

struct InstructionMemoryExpectation {
    std::string id;
    hpu::runtime::HpuMemSpan span;
    // Only meaningful payload words are compared; line padding is excluded.
    std::vector<std::uint32_t> words;
};

struct InstructionMemoryComparison {
    bool verified = false;
    std::size_t compared_words = 0;
    std::string object_id;
    std::size_t object_word_index = 0;
    std::uint32_t expected = 0;
    std::uint32_t actual = 0;
    std::string message;
};

InstructionMemoryComparison compare_instruction_memory(
    const InstructionExecutionResult& execution,
    const std::vector<InstructionMemoryExpectation>& expectations);

} // namespace hpu::model
