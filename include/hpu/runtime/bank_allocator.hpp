#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>

namespace hpu::runtime {

// The v1.1 target has 64 logical objects and six independently allocated banks.
// Copying this value type gives the compiler an atomic speculative layout.
struct BankAllocatorOptions {
    std::size_t regular_bank_count = 5;
    std::size_t regular_bank_lines = 1024;
    std::size_t allocation_granule_lines = 128;
    std::size_t small_bank_lines = 32;
    std::size_t mod_table_lines = 4;
};

struct BankAllocation {
    std::size_t bank = 0;
    std::size_t base_line = 0;
    std::size_t capacity_lines = 0;
    std::size_t actual_lines = 0;
    std::uint8_t type = 1;
};

struct BankAllocatorStats {
    std::size_t resident_objects = 0;
    std::size_t regular_banks = 0;
    std::size_t regular_lines = 0;
    std::size_t small_lines = 0;
    std::size_t peak_resident_objects = 0;
    std::size_t peak_regular_banks = 0;
    std::size_t peak_regular_lines = 0;
    std::size_t peak_small_lines = 0;
};

class BankAllocationError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class BankAllocator {
public:
    explicit BankAllocator(BankAllocatorOptions options = {});

    // Mask zero selects banks 0..4 for TYPE 0/1 and Bank 5 for TYPE 2/3.
    // A mask constrains this load only. Failed replacement preserves the object.
    BankAllocation load(std::uint8_t id, std::size_t lines,
                        std::uint8_t type, std::uint8_t bank_mask = 0);
    // No rhs means an immediate PMUL/PMAC operand. Matching ordinary
    // destinations reuse their region; PMAC reserves a separate result first.
    BankAllocation arithmetic(std::uint8_t dst, std::uint8_t lhs,
                              std::optional<std::uint8_t> rhs = {},
                              bool accumulate = false);
    // Every stage reserves a new region outside the old input bank.
    BankAllocation ntt(std::uint8_t data);
    void release(std::uint8_t id);

    const std::optional<BankAllocation>& allocation(std::uint8_t id) const;
    const BankAllocatorStats& stats() const noexcept { return stats_; }
    const BankAllocatorOptions& options() const noexcept { return options_; }

private:
    using Allocations = std::array<std::optional<BankAllocation>, 64>;
    BankAllocation find(const Allocations& search, std::size_t lines,
                        std::uint8_t type, std::uint8_t allowed,
                        std::size_t preference,
                        const std::array<std::size_t, 5>& read_cost) const;
    BankAllocation commit(std::uint8_t id, BankAllocation next,
                          bool old_and_new_coexist);
    void account(const Allocations& state,
                 const std::optional<BankAllocation>& extra = {});
    const BankAllocation& require(std::uint8_t id) const;

    BankAllocatorOptions options_;
    Allocations objects_{};
    BankAllocatorStats stats_{};
};

} // namespace hpu::runtime
