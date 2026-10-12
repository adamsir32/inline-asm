#pragma once

#include "hpu/seal/operation_relocation.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

// Verify the delivered operands, including both accumulator components. A
// software KeySwitch oracle alone cannot detect omitted hardware rounding.
inline void require_keyswitch_rounding_bindings(
    const hpu::seal_adapter::CkksRelocationSchedule& schedule,
    const hpu::runtime::HpuMemImage& image,
    const hpu::seal_adapter::PreparedKeySwitchConstants& constants,
    const hpu::seal_adapter::CkksLevelDescriptor& level,
    const std::string& operation)
{
    using hpu::seal_adapter::CkksDmaDirection;
    auto contexts = level.rns_layout.q_mod_ids;
    contexts.push_back(level.rns_layout.p_mod_ids.front());
    for (std::size_t limb = 0; limb < contexts.size(); ++limb) {
        const auto suffix = "/mod" + std::to_string(contexts[limb]);
        const auto half_id = constants.hardware_prefix + "/half" + suffix;
        const auto& half = image.allocation(half_id);
        const auto modulus = limb < level.q_moduli.size()
            ? level.q_moduli[limb] : level.special_moduli.front();
        const auto expected = (level.special_moduli.front() / 2) % modulus;
        const auto first = image.words().begin()
            + half.span.line_offset * hpu::runtime::kHpuMemLineWords;
        if (!half.read_only || half.kind != hpu::runtime::AllocationKind::constant
            || !std::all_of(first, first + half.word_count,
                [&](std::uint32_t word) { return word == expected; })) {
            throw std::runtime_error("incorrect KeySwitch half-P DMA payload");
        }
        for (int component = 0; component < 2; ++component) {
            const auto accumulator = constants.hardware_prefix
                + "/workspace/accumulator/c" + std::to_string(component) + suffix;
            std::size_t matches = 0;
            for (std::size_t index = 1; index + 1 < schedule.bindings.size(); ++index) {
                const auto& load = schedule.bindings[index];
                const auto& before = schedule.bindings[index - 1];
                const auto& after = schedule.bindings[index + 1];
                if (load.operation_id != operation || load.allocation_id != half_id
                    || before.allocation_id != accumulator) {
                    continue;
                }
                if (load.direction != CkksDmaDirection::load || load.object_slot != 1
                    || load.span.line_offset != half.span.line_offset
                    || load.span.line_count != half.span.line_count
                    || before.operation_id != operation || before.object_slot != 0
                    || before.direction != CkksDmaDirection::load
                    || after.operation_id != operation || after.object_slot != 0
                    || after.direction != CkksDmaDirection::store
                    || after.allocation_id != accumulator
                    || image.allocation(accumulator).read_only) {
                    throw std::runtime_error("KeySwitch rounding DMA does not update its accumulator");
                }
                ++matches;
            }
            if (matches != 1) {
                throw std::runtime_error("KeySwitch omitted or duplicated a component's half-P operand");
            }
        }
    }
}
