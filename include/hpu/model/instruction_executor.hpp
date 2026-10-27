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

// These are software execution contracts, not proof of an RTL test result.
// The production ABI uses explicit bit-reversed coefficient-domain storage;
// PNTT/PINTT execute only the supplied loader/BF/P stages, without a hidden
// whole-polynomial boundary permutation. PMUL is always pointwise.
enum class InstructionNttContract {
    // Compatibility profile for old natural-order functional images. Performs
    // software-only boundary conversions and does not describe the RTL ABI.
    natural_coefficient_boundaries,
    legacy_raw_stage,
    raw_bit_reversed_coefficients
};

const char* instruction_ntt_contract_name(InstructionNttContract contract) noexcept;

struct InstructionExecutionOptions {
    // New images store data and pre/post factors in bit-reversed coefficient
    // order. Neither transforms nor pointwise PMUL repair their layout.
    InstructionNttContract ntt_contract =
        InstructionNttContract::raw_bit_reversed_coefficients;
    std::size_t regular_bank_count = 5;
    std::size_t regular_bank_lines = 1024;
    std::size_t mod_table_lines = 4;
};

struct InstructionExecutionReport {
    InstructionExecutionStatus status = InstructionExecutionStatus::invalid_program;
    InstructionNttContract ntt_contract =
        InstructionNttContract::raw_bit_reversed_coefficients;
    // A software replay cannot establish a hardware contract. Actual RTL or
    // target execution evidence must be tracked separately by the caller.
    bool hardware_contract_verified = false;
    std::size_t forward_boundary_conversions = 0;
    std::size_t inverse_boundary_conversions = 0;
    std::size_t instruction_index = 0;
    std::size_t executed_instructions = 0;
    std::size_t peak_regular_banks = 0;
    std::size_t peak_regular_lines = 0;
    std::size_t peak_small_lines = 0;
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
// DSTORE releases only for REL=1. Physical allocation follows the v1.2 bank
// contract, including transient NTT/PMAC regions and packed Bank 5 parameters.
// CP is accepted on either DMA direction; functional replay does not simulate
// LLC residency, cache hits or DMA timing.
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
