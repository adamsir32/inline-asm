#pragma once

#include <seal/seal.h>

#include <cstdint>
#include <vector>

namespace hpu::seal_adapter {

// Reference-stage tables for one modulus of toy_fhe_auto.py's INTT-fused form:
// NTT_psi(a) -> INTT_{psi^(1/k)} -> a(X^k) in coefficient order.
struct FusedInverseAutomorphismTables {
    std::uint32_t modulus = 0;
    std::uint32_t canonical_psi = 0;
    std::uint32_t modified_psi = 0;
    std::vector<std::vector<std::uint32_t>> stages;
    // Legacy raw inverse coefficient order. ApplicationImage builders convert
    // this factor to natural order before exposing it to explicit post PMUL.
    std::vector<std::uint32_t> post_untwist_scale;
};

// Builds the modified-root tables for every active data modulus at parms_id.
// Stage payloads retain reference batch/lane order. A caller using natural
// coefficient boundaries must bit-reverse the returned post factor before
// preloading it. No modified-root representation crosses the completed INTT.
std::vector<FusedInverseAutomorphismTables>
create_fused_inverse_automorphism_tables(
    ::seal::parms_id_type parms_id,
    std::uint32_t galois_element,
    const ::seal::SEALContext& context);

} // namespace hpu::seal_adapter
