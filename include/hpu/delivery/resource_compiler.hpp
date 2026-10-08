#pragma once

#include "hpu/delivery/application_package.hpp"
#include "hpu/model/instruction_executor.hpp"

namespace hpu::delivery {

// Common native-operator compilation and verification for frontend and Plan
// packages. Mutates the supplied private image and relocates retained outputs.
std::string compile_application_resources(
    ApplicationPackageRequest& request, hpu::runtime::HpuMemImage& image,
    bool optimize = true, bool require_instruction_verification = true,
    hpu::model::InstructionNttContract ntt_contract =
        hpu::model::InstructionNttContract::natural_coefficient_boundaries);

} // namespace hpu::delivery
