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

// Involutive coefficient-index permutation, applied to one polynomial at a
// time: physical[j] = natural[bit_reverse(j, log2(N))]. It does not change word
// bits, RNS modulus order, or the canonical NTT-domain layout.
std::vector<std::uint32_t> bit_reverse_coefficients(
    const std::vector<std::uint32_t>& coefficients);

// Exact compression of a complete physical PMUL factor vector. Every entry
// must be nonzero and canonical. Arbitrary vectors need not be representable
// by K=8; reconstruction checks every coefficient and rejects such vectors.
std::vector<std::uint32_t> pack_online_pmul_factors(
    const std::vector<std::uint32_t>& physical_factors,
    std::uint32_t modulus);

// Layout-aware convenience for c*r^i, including independent inverse twist
// and normalization. Unlike the natural-order convenience above, this packs
// c*r^BR(j) and must be used with bit-reversed coefficient-domain data.
std::vector<std::uint32_t> pack_bit_reversed_online_pmul_parameters(
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

    // Mathematical API adapter for a natural-order post factor. Production
    // application images already store physical bit-reversed factors and must
    // assign post_scale directly. Normalization/untwist is included once.
    void set_natural_post_scale(std::vector<std::uint32_t> factors);
};

// Complete physical post-PMUL vector for a negacyclic inverse. Preserve the
// supplied raw inverse generator's lazy scale and N^-1 correction; multiply
// its entry j by inverse_psi^BR(j). This also supports modified Galois roots.
std::vector<std::uint32_t> inverse_post_factors(
    const InverseNttTables& tables,
    std::uint32_t inverse_psi,
    std::uint32_t modulus);

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

    // Mathematical convenience APIs consume/return natural coefficient order
    // and explicitly convert to/from the raw bit-reversed coefficient ABI.
    // These conversions belong to the API adapter, not to a PNTT/PINTT stage.
    // NTT-domain results remain in HPU physical order.
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
