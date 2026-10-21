#pragma once

#include "hpu/model/hardware_ntt.hpp"
#include "hpu/runtime/software_executor.hpp"

#include <stdexcept>

namespace hpu::seal_adapter::detail {

// Application images carry the fixed K=8 parameter ABI. The functional
// transform oracle still consumes the reconstructed physical BF factors.
inline std::vector<std::uint32_t> read_stage_twiddles(
    const hpu::runtime::HpuSoftwareExecutor& memory,
    const hpu::runtime::HpuMemSpan& span,
    std::uint32_t modulus,
    std::size_t degree)
{
    if (span.line_count != hpu::model::kStageTwiddleParameterLines) {
        throw std::invalid_argument("NTT stage parameters require exactly two HPU_MEM lines");
    }
    return hpu::model::expand_stage_twiddles(
        memory.read(span, hpu::model::kStageTwiddleParameterWords), modulus, degree);
}

} // namespace hpu::seal_adapter::detail
