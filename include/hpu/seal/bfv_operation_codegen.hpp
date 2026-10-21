#pragma once

#include "hpu/seal/bfv_operation_plan.hpp"

#include <seal/seal.h>

#include "hpu/compiler/operator_resources.hpp"
#include <memory>
#include <string>
#include <vector>

namespace hpu::seal_adapter {

struct BfvLoweredOperation {
    BfvOperationStep operation;
    std::string body_asm;
    std::shared_ptr<const hpu::resource::OperatorProgram> operator_program;
};

struct BfvLoweredProgram {
    std::string body_asm;
    std::vector<BfvLoweredOperation> operations;
    std::shared_ptr<const hpu::resource::OperatorProgram> operator_program;
};

// Emits native operator resource IR and compatibility assembly for NoOpt.
BfvLoweredProgram lower_bfv_operation_plan(const BfvOperationPlan& plan,
                                           const ::seal::SEALContext& context,
                                           bool append_psync = true,
                                           bool manage_modulus_table = true);

} // namespace hpu::seal_adapter
