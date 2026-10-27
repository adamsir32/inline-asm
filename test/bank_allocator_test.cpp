#include "hpu/runtime/bank_allocator.hpp"

#include <iostream>
#include <stdexcept>

namespace {

using hpu::runtime::BankAllocationError;
using hpu::runtime::BankAllocator;

void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

template<class Operation>
void require_capacity_fault(Operation operation)
{
    bool failed = false;
    try { operation(); } catch (const BankAllocationError&) { failed = true; }
    require(failed, "expected an atomic allocation capacity fault");
}

void normal_capacity_and_fragmentation()
{
    BankAllocator allocator;
    for (std::uint8_t id = 0; id < 40; ++id) {
        const auto a = allocator.load(id, 128, 0);
        require(a.bank == id % 5 && a.base_line == (id / 5) * 128,
                "normal allocation did not follow preference/lowest-fit rules");
    }
    require(allocator.stats().resident_objects == 40
                && allocator.stats().regular_lines == 5120,
            "N8192 objects did not pack forty regions");
    require_capacity_fault([&] { allocator.load(63, 128, 0); });
    const auto before = *allocator.allocation(0);
    require_capacity_fault([&] { allocator.ntt(0); });
    require(allocator.allocation(0)->bank == before.bank
                && allocator.allocation(0)->base_line == before.base_line,
            "failed NTT changed the old mapping");
    allocator.release(1);
    const auto migrated = allocator.ntt(0);
    require(migrated.bank == 1 && migrated.base_line == 0,
            "NTT did not use the newly available other-bank region");
    require(allocator.stats().peak_regular_lines == 5120,
            "transient ownership accounting exceeded real capacity");

    BankAllocator fragmented;
    for (std::uint8_t id = 0; id < 8; ++id) fragmented.load(id, 128, 0, 1);
    fragmented.release(0);
    fragmented.release(2);
    require_capacity_fault([&] { fragmented.load(63, 129, 0, 1); });
    fragmented.release(1);
    const auto larger = fragmented.load(63, 129, 0, 1);
    require(larger.base_line == 0 && larger.capacity_lines == 256,
            "normal intervals were not rounded/aligned to power-of-two units");
}

void bank5_and_atomic_loads()
{
    BankAllocator ordinary;
    require(ordinary.load(0, 2, 0).bank == 0,
            "MOD=0 without an explicit mask must select ordinary banks");
    require(ordinary.load(1, 3, 0, 0x20).bank == 5,
            "ordinary Bank5 data was incorrectly restricted to twiddle length");
    bool invalid_mod = false;
    try { ordinary.load(2, 2, 2, 0x20); }
    catch (const std::invalid_argument&) { invalid_mod = true; }
    require(invalid_mod, "old TYPE2 was accepted as a v1.2 MOD bit");
    bool invalid_mask = false;
    try { ordinary.load(2, 1, 1, 1); }
    catch (const std::invalid_argument&) { invalid_mask = true; }
    require(invalid_mask, "MOD=1 accepted an ordinary bank mask");

    BankAllocator allocator;
    const auto table = allocator.load(63, 1, 1);
    require(table.bank == 5 && table.base_line == 0 && table.capacity_lines == 4,
            "short modulus table did not reserve the complete configured prefix");
    for (std::uint8_t id = 0; id < 14; ++id) {
        const auto a = allocator.load(id, 2, 0, 0x20);
        require(a.bank == 5 && a.base_line == 4U + 2U * id,
                "twiddle parameters did not use lowest even Bank5 intervals");
    }
    require(allocator.stats().small_lines == 32,
            "small-bank reservations did not include the modulus prefix");
    require_capacity_fault([&] { allocator.load(20, 2, 0, 0x20); });
    require_capacity_fault([&] { allocator.load(62, 1, 1); });
    // Replacement can reuse its own old region, but preserves that mapping if
    // no legal replacement can be found.
    require_capacity_fault([&] { allocator.load(0, 3, 0, 0x20); });
    require(allocator.allocation(0)->actual_lines == 2
                && allocator.allocation(0)->base_line == 4,
            "failed load destroyed the old object");
    const auto reloaded = allocator.load(0, 2, 0, 0x20);
    require(reloaded.base_line == 4, "atomic same-ID reload did not reuse its old interval");
    const auto fallback = allocator.load(20, 2, 0, 0x21);
    require(fallback.bank == 0, "mixed load mask failed to fall back to an ordinary bank");
    allocator.release(0);
    const auto preferred_small = allocator.load(21, 2, 0, 0x21);
    require(preferred_small.bank == 5, "mixed load mask did not try Bank5 first");

    hpu::runtime::BankAllocatorOptions options;
    options.mod_table_lines = 8;
    BankAllocator larger_table(options);
    larger_table.load(63, 8, 1, 0x20);
    for (std::uint8_t id = 0; id < 12; ++id) larger_table.load(id, 2, 0, 0x20);
    require_capacity_fault([&] { larger_table.load(12, 2, 0, 0x20); });
    options.mod_table_lines = 3;
    BankAllocator odd_prefix(options);
    require(odd_prefix.load(0, 2, 0, 0x20).base_line == 4,
            "odd modulus reservation did not round the small-object start upward");
}

void read_cost_and_result_replacement()
{
    BankAllocator small_accumulator;
    small_accumulator.load(0, 2, 0, 0x20);
    require(small_accumulator.arithmetic(0, 0, {}, true).bank == 1,
            "PMAC from Bank5 did not use the specified modulo-five preference");
    BankAllocator allocator;
    allocator.load(0, 128, 0, 1);
    allocator.load(1, 128, 0, 2);
    const auto sum = allocator.arithmetic(5, 0, std::uint8_t{1});
    require(sum.bank == 2, "new arithmetic result ignored source read costs");
    const auto same = allocator.arithmetic(5, 5, std::uint8_t{1});
    require(same.bank == sum.bank && same.base_line == sum.base_line,
            "matching destination was unexpectedly migrated");
    // A DLOAD's single-bank mask does not constrain a later result allocation.
    const auto transformed = allocator.ntt(0);
    require(transformed.bank == 1, "load mask persisted into NTT placement");
    // PMAC always reserves an extra interval, including dst==lhs==rhs.
    const auto before = *allocator.allocation(1);
    const auto mac = allocator.arithmetic(1, 1, std::uint8_t{1}, true);
    require(mac.bank == 2 && (mac.bank != before.bank || mac.base_line != before.base_line),
            "PMAC did not avoid the three counted reads and reserve a new interval");
    require(allocator.stats().peak_regular_lines > allocator.stats().regular_lines,
            "PMAC transient physical reservation was omitted");

    // Tie-breaking is cyclic from preference, after minimizing read cost.
    BankAllocator tie;
    tie.load(0, 1024, 0, 1);
    tie.load(1, 1024, 0, 2);
    tie.load(2, 1024, 0, 4);
    const auto result = tie.arithmetic(1, 0, std::uint8_t{2}, true);
    require(result.bank == 3, "PMAC tie-breaking preference is incorrect");
    tie.release(1);
    tie.load(1, 1024, 0, 2);
    tie.load(3, 1024, 0, 8);
    tie.load(4, 1024, 0, 16);
    require_capacity_fault([&] { tie.arithmetic(1, 0, std::uint8_t{2}, true); });
    require(tie.allocation(1)->bank == 1, "failed PMAC changed the accumulator mapping");

    BankAllocator fallback;
    fallback.load(0, 128, 0, 1);
    for (std::uint8_t id = 1; id < 5; ++id)
        fallback.load(id, 1024, 0, static_cast<std::uint8_t>(1U << id));
    const auto conflict = fallback.arithmetic(5, 0, std::uint8_t{0});
    require(conflict.bank == 0 && conflict.base_line == 128,
            "ordinary arithmetic did not fall back to the only bank with capacity");
    require_capacity_fault([&] { fallback.ntt(0); });
}

void online_multiplier_parameters()
{
    BankAllocator allocator;
    allocator.load(0, 128, 0, 1);
    allocator.load(1, 2, 0, 2);
    const auto result = allocator.online_multiply(6, 0, 1);
    require(result.bank == 1 && result.base_line == 128,
            "online parameters incorrectly contributed sustained read cost");
    const auto original = *allocator.allocation(0);
    const auto inplace = allocator.online_multiply(0, 0, 1);
    require(inplace.bank == original.bank && inplace.base_line == original.base_line,
            "online in-place result moved the source allocation");
    allocator.load(63, 1, 1, 0x20);
    allocator.load(2, 1, 0, 0x20);
    allocator.load(3, 3, 0, 0x20);
    auto rejects = [&](std::uint8_t dst, std::uint8_t src, std::uint8_t param) {
        bool rejected = false;
        try { allocator.online_multiply(dst, src, param); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "online PMUL accepted an invalid operand allocation");
    };
    rejects(1, 0, 1); // Parameter must differ from destination.
    rejects(6, 1, 1); // Parameter must differ from source.
    rejects(6, 0, 2); // Parameter must be exactly two lines.
    rejects(6, 0, 63); // Modulus table cannot be a parameter.
    rejects(6, 63, 1); // Modulus table cannot be data.
    rejects(63, 0, 1); // Modulus table cannot be a destination.
    rejects(6, 2, 1); // One-line data is outside the supported degree.
    rejects(6, 3, 1); // Non-power-of-two data is unsupported.
    require(allocator.allocation(6)->bank == result.bank
                && allocator.allocation(6)->base_line == result.base_line,
            "invalid online PMUL changed the destination mapping");
}

} // namespace

int main()
{
    try {
        normal_capacity_and_fragmentation();
        bank5_and_atomic_loads();
        read_cost_and_result_replacement();
        online_multiplier_parameters();
        std::cout << "Bank allocator tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Bank allocator test failed: " << error.what() << '\n';
        return 1;
    }
}
