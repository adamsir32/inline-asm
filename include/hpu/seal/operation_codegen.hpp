#pragma once

#include "hpu/seal/operation_plan.hpp"

#include <seal/seal.h>

#include "hpu/compiler/operator_resources.hpp"
#include <memory>
#include <string>
#include <vector>

namespace hpu::seal_adapter {

struct CkksLoweredOperation {
    CkksOperationStep operation;
    std::string body_asm;
    std::shared_ptr<const hpu::resource::OperatorProgram> operator_program;
};

struct CkksLoweredProgram {
    std::string body_asm;
    std::vector<CkksLoweredOperation> operations;
    std::shared_ptr<const hpu::resource::OperatorProgram> operator_program;
};

// Emits native operator resource IR alongside a compatibility assembly body.
// Logical data/resource IDs remain in the per-operation manifest for DMA port
// binding; the common compiler assigns final object slots and memory spans.
CkksLoweredProgram lower_ckks_operation_plan(
    const CkksOperationPlan& plan,
    const ::seal::SEALContext& context,
    bool append_psync = true,
    bool manage_modulus_table = true);

} // namespace hpu::seal_adapter
