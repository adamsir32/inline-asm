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

// PMUL has a separate exact-factor parameter ABI. In particular U[0]
// may be invN (or zero), so NTT-stage parameter validation is inappropriate.
inline std::vector<std::uint32_t> read_online_pmul_factors(
    const hpu::runtime::HpuSoftwareExecutor& memory,
    const hpu::runtime::HpuMemSpan& span,
    std::uint32_t modulus,
    std::size_t degree)
{
    if (span.line_count != hpu::model::kOnlinePmulParameterLines) {
        throw std::invalid_argument("online PMUL parameters require exactly two HPU_MEM lines");
    }
    return hpu::model::expand_online_pmul_factors(
        memory.read(span, hpu::model::kOnlinePmulParameterWords), modulus, degree);
}

// Run the loader/BF stages exactly as the target instruction stream does.
// Coefficients and explicit PMUL factors already use bit-reversed physical
// positions; no boundary permutation is hidden inside this helper.
inline std::vector<std::uint32_t> execute_transform_stages(
    std::vector<std::uint32_t> words,
    const std::vector<std::vector<std::uint32_t>>& stages,
    std::uint32_t modulus,
    bool inverse)
{
    std::size_t expected_stages = 0;
    for (std::size_t remaining = words.size(); remaining > 1; remaining >>= 1U) {
        ++expected_stages;
    }
    if (words.size() < 128 || (words.size() & (words.size() - 1)) != 0
        || stages.size() != expected_stages) {
        throw std::invalid_argument("raw NTT transform has an invalid degree or stage count");
    }
    for (std::size_t stage = 0; stage < stages.size(); ++stage) {
        words = hpu::model::execute_ntt_stage(
            words, stages[stage], modulus, stage, inverse);
    }
    return words;
}

} // namespace hpu::seal_adapter::detail
