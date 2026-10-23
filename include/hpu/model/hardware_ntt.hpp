#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hpu::model {

// PNTT/PINTT src2 ABI. Payloads are exactly two 256 B little-endian u32
// lines, including for N < 1024. The first line is U[64]; the second is
// V[1..7], R[0..r_count-1], then ones. V[0] is implicitly one.
inline constexpr std::size_t kStageTwiddleParameterWords = 128;
inline constexpr std::size_t kStageTwiddleParameterLines = 2;
inline constexpr std::size_t kTwiddleChains = 8;
inline constexpr const char* kStageTwiddleParameterFormat = "hpu-twiddle-k8-v1";

// PMUL online factors use the same two-line storage shape as NTT parameters,
// but process 64 coefficients per batch (NTT processes 128). U[0] may be any
// canonical residue, including the inverse-transform normalization N^-1.
inline constexpr std::size_t kOnlinePmulParameterWords = 128;
inline constexpr std::size_t kOnlinePmulParameterLines = 2;
inline constexpr const char* kOnlinePmulParameterFormat = "hpu-pmul-online-k8-v1";

std::vector<std::uint32_t> pack_online_pmul_parameters(
    std::uint32_t coefficient,
    std::uint32_t ratio,
    std::uint32_t modulus,
    std::size_t degree);

// Expand the supplied U/V/R payload as the online multiplier does. No NTT
// permutation or extra normalization is applied, and state is reset per call.
std::vector<std::uint32_t> expand_online_pmul_factors(
    const std::vector<std::uint32_t>& parameters,
    std::uint32_t modulus,
    std::size_t degree);

// Compress an existing physical batch/BF-lane table. Reconstructs and checks
// every factor; unsupported physical recurrences throw instead of falling
// back to the old full-table representation.
std::vector<std::uint32_t> pack_stage_twiddles(
    const std::vector<std::uint32_t>& full_table,
    std::uint32_t modulus,
    std::size_t degree);

// Expand K=8 parameters for mathematical stage execution. State belongs to
// this call, so changing modulus, root, direction or stage cannot reuse a
// previous stage's chain values. Full-table stage APIs below remain oracles.
std::vector<std::uint32_t> expand_stage_twiddles(
    const std::vector<std::uint32_t>& parameters,
    std::uint32_t modulus,
    std::size_t degree);

struct NttBatch {
    bool interleaved = false;
    std::size_t first = 0;
    std::size_t second = 0;
};

struct InverseNttTables {
    // One N/2-entry table per inverse stage, in loader-batch/BF-lane order.
    std::vector<std::vector<std::uint32_t>> stages;

    // Per physical coefficient position after the last inverse stage.
    // Includes lazy-scale cancellation and N^-1 normalization.
    std::vector<std::uint32_t> post_scale;

    // Application images store the explicit post PMUL factor at natural
    // coefficient positions. Adapt that factor to this model's raw inverse
    // layout; the caller includes normalization/untwist exactly once.
    void set_natural_post_scale(std::vector<std::uint32_t> factors);
};

// Bit-accurate software model of hw_ntt_intt_complete.py. This class models
// the cyclic transform. CKKS negacyclic transforms are exposed by the free
// functions below and add the required psi twist/untwist.
class HardwareNttModel {
public:
    HardwareNttModel(
        std::size_t degree,
        std::uint32_t modulus,
        std::uint32_t omega);

    std::size_t degree() const noexcept;
    std::uint32_t modulus() const noexcept;
    std::uint32_t omega() const noexcept;
    std::size_t log_degree() const noexcept;

    std::vector<NttBatch> stage_batches(std::size_t forward_stage) const;
    std::vector<std::size_t> forward_layout() const;
    std::vector<std::vector<std::uint32_t>> forward_twiddles() const;
    InverseNttTables inverse_twiddles() const;

    // Forward consumes normal logical coefficient order, performs the DIT input
    // bit-reversal required by the hardware memory convention, and returns HPU
    // physical NTT order. Inverse consumes that physical order and returns
    // normal logical coefficient order.
    std::vector<std::uint32_t> forward(
        const std::vector<std::uint32_t>& coefficients) const;
    std::vector<std::uint32_t> forward(
        const std::vector<std::uint32_t>& coefficients,
        const std::vector<std::vector<std::uint32_t>>& stage_twiddles) const;
    std::vector<std::uint32_t> inverse(
        const std::vector<std::uint32_t>& physical_ntt) const;
    std::vector<std::uint32_t> inverse(
        const std::vector<std::uint32_t>& physical_ntt,
        const InverseNttTables& tables) const;

private:
    std::size_t degree_;
    std::uint32_t modulus_;
    std::uint32_t omega_;
    std::size_t log_degree_;
};

// Executes exactly one loader/BF/P stage with the supplied DMA twiddles. No
// input/output bit reversal, pre-twist, inverse normalization, or table
// generation is performed. Inverse stage k uses forward loader log2(N)-1-k.
std::vector<std::uint32_t> execute_ntt_stage(
    const std::vector<std::uint32_t>& input,
    const std::vector<std::uint32_t>& stage_twiddles,
    std::uint32_t modulus,
    std::size_t stage,
    bool inverse = false);

std::uint32_t pow_mod(
    std::uint32_t base,
    std::uint64_t exponent,
    std::uint32_t modulus);

std::uint32_t inverse_mod_prime(
    std::uint32_t value,
    std::uint32_t modulus);

std::vector<std::uint32_t> negacyclic_forward(
    const std::vector<std::uint32_t>& coefficients,
    std::uint32_t modulus,
    std::uint32_t psi);

std::vector<std::uint32_t> negacyclic_inverse(
    const std::vector<std::uint32_t>& physical_ntt,
    std::uint32_t modulus,
    std::uint32_t psi);

std::vector<std::uint32_t> automorphism_coefficients(
    const std::vector<std::uint32_t>& coefficients,
    std::uint64_t galois_element,
    std::uint32_t modulus);

// Implements toy_fhe_auto.py's two fused forms. Both return the canonical HPU
// NTT physical domain, so no modified-root state must survive a kernel boundary.
std::vector<std::uint32_t> automorphism_fused_inverse(
    const std::vector<std::uint32_t>& canonical_physical_ntt,
    std::uint64_t galois_element,
    std::uint32_t modulus,
    std::uint32_t psi);

std::vector<std::uint32_t> automorphism_fused_forward(
    const std::vector<std::uint32_t>& canonical_physical_ntt,
    std::uint64_t galois_element,
    std::uint32_t modulus,
    std::uint32_t psi);

} // namespace hpu::model
