#pragma once

#include "hpu/runtime/memory_image.hpp"
#include "hpu/seal/operation_codegen.hpp"
#include "util/hpu_asm.hpp"

#include <seal/seal.h>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace hpu::seal_adapter {

enum class CkksDmaDirection {
    load,
    store
};

// Binds a native logical data port to a named HPU_MEM allocation. The compiler
// assigns final DMA order and renders the required x10/x11 updates after
// scheduling, resource allocation, and DDR placement.
struct CkksDmaBinding {
    std::size_t program_dma_index = 0;
    std::optional<std::size_t> operation_index;
    std::size_t operation_dma_index = 0;
    std::string operation_id;
    CkksDmaDirection direction = CkksDmaDirection::load;
    int object_slot = 0;
    hpu::DataType load_type = hpu::DataType::poly;
    hpu::DloadFlag load_flag = hpu::DloadFlag::regular_bank;
    int store_release = 0;
    std::string allocation_id;
    hpu::runtime::HpuMemSpan span;
    std::uint8_t cp = 0;
};

// Complex kernels remain visible instead of receiving a guessed relocation.
// first_program_dma_index and dma_count reserve their exact positions in the
// generated stream so later resolved operations retain stable ordinals.
struct CkksUnresolvedRelocation {
    std::size_t operation_index = 0;
    std::string operation_id;
    CkksOperationKind kind = CkksOperationKind::square;
    std::size_t first_program_dma_index = 0;
    std::size_t dma_count = 0;
    std::string reason;
};

struct CkksRelocationSchedule {
    std::size_t expected_dma_count = 0;
    std::vector<CkksDmaBinding> bindings;
    std::vector<CkksUnresolvedRelocation> unresolved_operations;

    bool complete() const noexcept
    {
        return unresolved_operations.empty()
            && bindings.size() == expected_dma_count;
    }
};

// Resolves supported plan operations by named native data ports. Physical
// object hints and compatibility assembly order do not identify their data.
CkksRelocationSchedule build_ckks_relocation_schedule(
    const CkksLoweredProgram& program,
    const hpu::runtime::HpuMemImage& image,
    const ::seal::SEALContext& context);

} // namespace hpu::seal_adapter
