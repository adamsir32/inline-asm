#include "hpu/seal/evaluator.hpp"
#include "hpu/delivery/resource_compiler.hpp"

namespace hpu::seal_adapter::detail {

std::string finalize_frontend_package(
    hpu::delivery::ApplicationPackageRequest& request,
    hpu::runtime::HpuMemImage& image, bool optimize,
    bool require_instruction_verification,
    hpu::model::InstructionNttContract instruction_ntt_contract)
{
    return hpu::delivery::compile_application_resources(request, image, optimize,
        require_instruction_verification, instruction_ntt_contract);
}

} // namespace hpu::seal_adapter::detail
