#pragma once

#include "hpu/runtime/memory_image.hpp"
#include "instruction.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace hpu::compiler {

using BufferId = std::size_t;
using ObjectId = std::size_t;

enum class OptimizationMode { no_opt, optimized };
enum class ResourceIrOrigin { encoded_import, operator_emission };
enum class MemoryLayout {
    unspecified,
    coefficient_natural,
    coefficient_bit_reversed,
    canonical_ntt_physical
};

struct CompileOptions {
    OptimizationMode optimization = OptimizationMode::optimized;
    std::size_t regular_bank_count = hpu::runtime::kRegularBankCount;
    std::uint64_t regular_bank_lines = 1024;
    // Explicit buffer ABI annotations. They never permute payload words.
    // Workspace whose layout changes during execution must stay unspecified.
    std::map<std::string, MemoryLayout> buffer_layouts;
};

// Scheme-independent DMA provenance. Direction and object operands are taken
// from the encoded instruction, so they cannot disagree with a second recipe.
struct ResolvedDma {
    std::size_t instruction_index = 0;
    std::string allocation_id;
    hpu::runtime::HpuMemSpan span;
    std::optional<std::size_t> operation_index;
    std::string operation_id;
    std::size_t operation_dma_index = 0;
};

struct BufferRange {
    BufferId buffer = 0;
    std::uint64_t line_offset = 0;
    std::uint64_t line_count = 0;
};

struct VirtualBuffer {
    std::string id;
    hpu::runtime::HpuMemSpan original_span;
    std::size_t word_count = 0;
    hpu::runtime::AllocationKind kind = hpu::runtime::AllocationKind::workspace;
    bool read_only = false;
    bool retained = true;
    MemoryLayout layout = MemoryLayout::unspecified;
    // Reuse requires a complete store before every initial-memory load.
    bool fully_defined_before_read = false;
    std::size_t first_use = 0;
    std::size_t last_use = 0;
};

struct VirtualObject {
    std::uint64_t line_count = 0;
    std::uint8_t type = 0;
    std::uint8_t dma_flag = 0;
};

struct VirtualInstruction {
    hpu::Instruction instruction;
    std::vector<ObjectId> uses;
    std::vector<ObjectId> definitions;
    std::optional<BufferRange> memory;
    std::optional<ResolvedDma> origin;
    bool required_store = true;
    // A deferred scratch store and its unique reload share the object ID.
    bool reload_retained_object = false;
};

struct VirtualProgram {
    ResourceIrOrigin origin = ResourceIrOrigin::encoded_import;
    std::vector<VirtualBuffer> buffers;
    std::vector<VirtualObject> objects;
    std::vector<VirtualInstruction> instructions;
};

struct LogicalBufferPlacement {
    std::string logical_id;
    std::string physical_allocation_id;
    hpu::runtime::HpuMemSpan original_span;
    hpu::runtime::HpuMemSpan span;
    bool retained = true;
    std::size_t first_use = 0;
    std::size_t last_use = 0;
    MemoryLayout layout = MemoryLayout::unspecified;
};

struct ResourceReport {
    ResourceIrOrigin ir_origin = ResourceIrOrigin::encoded_import;
    std::uint64_t original_ddr_lines = 0;
    std::uint64_t allocated_ddr_lines = 0;
    std::uint64_t scratch_arena_lines = 0;
    std::size_t original_dma_count = 0;
    std::size_t dma_count = 0;
    std::size_t peak_regular_banks = 0;
    std::size_t peak_object_slots = 0;
    std::size_t deferred_scratch_stores = 0;
    std::size_t spills = 0;
    std::size_t reloads = 0;
};

struct CompiledResources {
    explicit CompiledResources(std::uint64_t capacity_lines = 1)
        : image(capacity_lines) {}

    std::vector<hpu::EncodedInstruction> instructions;
    std::vector<ResolvedDma> dma;
    hpu::runtime::HpuMemImage image;
    std::vector<LogicalBufferPlacement> buffers;
    ResourceReport report;
};

// Imports resolved encoded instructions into typed virtual object/buffer IR.
// Every DMA range is validated against its named physical allocation.
VirtualProgram import_resolved_program(
    const std::vector<hpu::EncodedInstruction>& instructions,
    const std::vector<ResolvedDma>& dma,
    const hpu::runtime::HpuMemImage& image);

// NoOpt preserves the original instruction words, DMA ranges and image bytes.
// Optimized keeps immutable resources and every non-workspace allocation,
// reuses proven scratch lifetimes, and schedules distinct slots and banks.
CompiledResources optimize_resources(
    const std::vector<hpu::EncodedInstruction>& instructions,
    const std::vector<ResolvedDma>& dma,
    const hpu::runtime::HpuMemImage& image,
    const CompileOptions& options = {});

// Native operator IR entry. Physical operand fields of virtual instructions
// are ignored; all final object IDs are chosen by the shared scheduler.
CompiledResources compile_resources(
    VirtualProgram program,
    const std::vector<hpu::EncodedInstruction>& baseline,
    const std::vector<ResolvedDma>& dma,
    const hpu::runtime::HpuMemImage& image,
    const CompileOptions& options = {});

hpu::runtime::HpuMemSpan relocate_span(
    const CompiledResources& resources,
    const std::string& logical_allocation_id,
    hpu::runtime::HpuMemSpan original_span);

std::string render_resource_report(const CompiledResources& resources);

} // namespace hpu::compiler
