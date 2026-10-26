#pragma once

#include "hpu/compiler/operator_resources.hpp"

#include <set>

namespace hpu::resource {

struct SsaOptimizationReport {
    std::size_t original_operations = 0;
    std::size_t operations = 0;
    std::size_t merged_reads = 0;
    std::size_t forwarded_reads = 0;
    std::size_t dead_operations = 0;
    std::size_t lifetime_hints_removed = 0;
    bool reordered = false;
};

// Native SSA uses one immutable global modulus table; aliases of the same
// exact table range are allowed. Encoded ISA import remains independent.
// Normalize bound logical SSA without IO optimization: discard historical
// lifetime hints, retain read/write order, and materialize explicit contexts.
// required_outputs validates output identities. This full-source diagnostic
// retains obsolete checkpoint writes as well as observable output writes.
OperatorProgram canonical_operator_program(
    const OperatorProgram& source, const hpu::runtime::HpuMemImage& image,
    const std::set<std::string>& required_outputs = {});

// Apply only the observable-output policy to the canonical source. Remove
// unreachable work without merging reads, forwarding stores, or reordering.
// Required intermediate store/load roundtrips remain, providing an IO baseline
// with the same output roots as the reuse and scheduling candidates.
OperatorProgram output_root_operator_program(
    const OperatorProgram& source, const hpu::runtime::HpuMemImage& image,
    const std::set<std::string>& required_outputs = {},
    SsaOptimizationReport* report = nullptr);

// Optimize memory values before storage binding. An empty required_outputs set
// retains final writes to every non-workspace allocation. A nonempty set names
// the only externally observable buffers; internal stores survive only when a
// remaining load depends on them. Output aliases resolve to physical identity.
OperatorProgram optimize_operator_program(
    const OperatorProgram& source, const hpu::runtime::HpuMemImage& image,
    const std::set<std::string>& required_outputs = {}, bool reorder = false,
    SsaOptimizationReport* report = nullptr);

} // namespace hpu::resource
