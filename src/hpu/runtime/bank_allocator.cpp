#include "hpu/runtime/bank_allocator.hpp"

#include <algorithm>
#include <array>
#include <string>

namespace hpu::runtime {
namespace {

void check_id(std::uint8_t id)
{
    if (id >= 64) throw std::invalid_argument("object ID must be in [0, 63]");
}

bool intersects(std::size_t begin, std::size_t length, const BankAllocation& a)
{
    return begin < a.base_line + a.capacity_lines && a.base_line < begin + length;
}

} // namespace

BankAllocator::BankAllocator(BankAllocatorOptions options) : options_(options)
{
    if (options_.regular_bank_count == 0 || options_.regular_bank_count > 5
        || options_.regular_bank_lines != 1024
        || options_.allocation_granule_lines != 128
        || options_.small_bank_lines != 32
        || options_.mod_table_lines == 0 || options_.mod_table_lines > 16)
        throw std::invalid_argument("invalid v1.1 physical bank configuration");
    account(objects_);
}

const std::optional<BankAllocation>& BankAllocator::allocation(std::uint8_t id) const
{
    check_id(id);
    return objects_[id];
}

const BankAllocation& BankAllocator::require(std::uint8_t id) const
{
    const auto& a = allocation(id);
    if (!a) throw std::invalid_argument("object p" + std::to_string(id) + " is not resident");
    return *a;
}

BankAllocation BankAllocator::find(
    const Allocations& search, std::size_t lines, std::uint8_t type,
    std::uint8_t allowed, std::size_t preference,
    const std::array<std::size_t, 5>& read_cost) const
{
    if (lines == 0) throw std::invalid_argument("object length must be positive");
    if (type > 3) throw std::invalid_argument("DLOAD TYPE must be in [0, 3]");
    if (type == 3 && lines != 2)
        throw std::invalid_argument("TYPE 3 requires a two-line K=8 parameter object");
    auto fits = [&](std::size_t bank, std::size_t begin, std::size_t capacity) {
        for (const auto& a : search)
            if (a && a->bank == bank && intersects(begin, capacity, *a)) return false;
        return true;
    };
    if ((allowed & 0x20U) != 0) {
        if (type == 2) {
            if (lines > options_.mod_table_lines)
                throw std::invalid_argument("modulus table exceeds configured reserved lines");
            // One live modulus table owns the entire permanently reserved area.
            if (fits(5, 0, options_.mod_table_lines))
                return {5, 0, options_.mod_table_lines, lines, type};
        } else {
            const auto begin = (options_.mod_table_lines + 1U) & ~std::size_t{1};
            for (std::size_t line = begin; line <= options_.small_bank_lines
                 && lines <= options_.small_bank_lines - line; line += 2)
                if (fits(5, line, lines)) return {5, line, lines, lines, type};
        }
    }
    std::size_t capacity = options_.allocation_granule_lines;
    while (capacity < lines && capacity < options_.regular_bank_lines) capacity *= 2;
    std::optional<BankAllocation> best;
    std::size_t best_cost = 0;
    if (capacity >= lines && capacity <= options_.regular_bank_lines && type != 2) {
        // The RTL preference and tie-breaking domain remains five banks even
        // when a test disables some of them through regular_bank_count.
        for (std::size_t k = 0; k < 5; ++k) {
            const auto bank = (preference + k) % 5;
            if (bank >= options_.regular_bank_count || (allowed & (1U << bank)) == 0) continue;
            for (std::size_t line = 0; line + capacity <= options_.regular_bank_lines; line += capacity) {
                if (!fits(bank, line, capacity)) continue;
                if (!best || read_cost[bank] < best_cost) {
                    best = BankAllocation{bank, line, capacity, lines, type};
                    best_cost = read_cost[bank];
                }
                break;
            }
        }
    }
    if (!best) throw BankAllocationError("no allowed physical bank interval can hold the object");
    return *best;
}

BankAllocation BankAllocator::load(
    std::uint8_t id, std::size_t lines, std::uint8_t type, std::uint8_t bank_mask)
{
    check_id(id);
    if (bank_mask > 63) throw std::invalid_argument("DLOAD bank mask exceeds six bits");
    if (type == 2 && bank_mask != 0 && bank_mask != 0x20)
        throw std::invalid_argument("modulus table mask must be 0 or 0x20");
    const auto allowed = bank_mask != 0 ? bank_mask : (type >= 2 ? 0x20 : 0x1f);
    auto search = objects_;
    search[id].reset();
    const auto next = find(search, lines, type, static_cast<std::uint8_t>(allowed), id % 5, {});
    return commit(id, next, false);
}

BankAllocation BankAllocator::arithmetic(
    std::uint8_t dst, std::uint8_t lhs, std::optional<std::uint8_t> rhs, bool accumulate)
{
    check_id(dst);
    const auto left = require(lhs);
    std::array<std::size_t, 5> read_cost{};
    if (left.bank < 5) ++read_cost[left.bank];
    if (rhs) {
        const auto right = require(*rhs);
        if (right.actual_lines != left.actual_lines)
            throw std::invalid_argument("arithmetic operand lengths differ");
        if (right.bank < 5) ++read_cost[right.bank];
    }
    if (left.type == 2 || (rhs && require(*rhs).type == 2))
        throw std::invalid_argument("arithmetic cannot consume the modulus table");
    std::size_t preference = dst % 5;
    auto search = objects_;
    if (accumulate) {
        const auto old = require(dst);
        if (old.actual_lines != left.actual_lines || old.type == 2)
            throw std::invalid_argument("PMAC accumulator length/type differs");
        if (old.bank < 5) ++read_cost[old.bank];
        preference = (old.bank + 1) % 5;
    } else if (objects_[dst]) {
        if (objects_[dst]->type == 2)
            throw std::invalid_argument("arithmetic cannot overwrite the modulus table");
        if (objects_[dst]->actual_lines == left.actual_lines) return *objects_[dst];
        if (dst == lhs || (rhs && dst == *rhs))
            throw std::invalid_argument("length-changing destination is also a source");
        search[dst].reset();
    }
    const auto next = find(search, left.actual_lines, 1, 0x1f, preference, read_cost);
    return commit(dst, next, accumulate);
}

BankAllocation BankAllocator::ntt(std::uint8_t data)
{
    const auto old = require(data);
    if (old.type == 2) throw std::invalid_argument("NTT input cannot be the modulus table");
    auto allowed = std::uint8_t{0x1f};
    if (old.bank < 5) allowed &= static_cast<std::uint8_t>(~(1U << old.bank));
    const auto next = find(objects_, old.actual_lines, 1, allowed,
                           old.bank == 5 ? 0 : (old.bank + 1) % 5, {});
    return commit(data, next, true);
}

BankAllocation BankAllocator::commit(std::uint8_t id, BankAllocation next, bool coexist)
{
    if (coexist) account(objects_, next);
    objects_[id] = next;
    account(objects_);
    return next;
}

void BankAllocator::release(std::uint8_t id)
{
    require(id);
    objects_[id].reset();
    account(objects_);
}

void BankAllocator::account(const Allocations& state, const std::optional<BankAllocation>& extra)
{
    std::size_t resident = 0, regular_lines = 0;
    std::size_t small_lines = options_.mod_table_lines;
    std::array<bool, 5> banks{};
    auto count = [&](const BankAllocation& a) {
        if (a.bank < 5) {
            banks[a.bank] = true;
            regular_lines += a.capacity_lines;
        } else if (a.type != 2) small_lines += a.capacity_lines;
    };
    for (const auto& a : state) if (a) { ++resident; count(*a); }
    if (extra) count(*extra);
    const auto bank_count = static_cast<std::size_t>(std::count(banks.begin(), banks.end(), true));
    stats_.resident_objects = resident;
    stats_.regular_banks = bank_count;
    stats_.regular_lines = regular_lines;
    stats_.small_lines = small_lines;
    stats_.peak_resident_objects = std::max(stats_.peak_resident_objects, resident);
    stats_.peak_regular_banks = std::max(stats_.peak_regular_banks, bank_count);
    stats_.peak_regular_lines = std::max(stats_.peak_regular_lines, regular_lines);
    stats_.peak_small_lines = std::max(stats_.peak_small_lines, small_lines);
}

} // namespace hpu::runtime
