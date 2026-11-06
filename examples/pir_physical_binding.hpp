#pragma once

#include "hpu/runtime/memory_image.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

// Application-level binding. Physical content and the public allocation table
// are the evidence; frontend plain_N creation order is deliberately ignored.
namespace pir_physical_binding {

inline const hpu::runtime::HpuMemAllocation& logical_plaintext_limb(
    const hpu::runtime::HpuMemImage& image, std::size_t import_rank,
    std::uint8_t modulus_id, const std::vector<std::uint32_t>& encoded_words)
{
    if (import_rank >= 32 || modulus_id >= 3 || encoded_words.empty())
        throw std::runtime_error("invalid stage1 logical plaintext limb");
    const auto prefix = "frontend/plains/plain_" +
                        std::to_string(3 + import_rank) + "/multiply/";
    const auto suffix = "/c0/mod" + std::to_string(modulus_id);
    const hpu::runtime::HpuMemAllocation* match = nullptr;
    for (const auto& allocation : image.allocations()) {
        if (allocation.id.compare(0, prefix.size(), prefix) != 0 ||
            allocation.id.size() < suffix.size() ||
            allocation.id.compare(allocation.id.size() - suffix.size(),
                                  suffix.size(), suffix) != 0)
            continue;
        if (match)
            throw std::runtime_error("duplicate stage1 logical allocation: " + prefix);
        match = &allocation;
    }
    if (!match || match->kind != hpu::runtime::AllocationKind::plaintext ||
        !match->read_only || match->word_count != encoded_words.size())
        throw std::runtime_error("missing or malformed stage1 logical allocation: " + prefix);
    const auto begin = match->span.line_offset * hpu::runtime::kHpuMemLineWords;
    if (match->span.line_count * hpu::runtime::kHpuMemLineWords < encoded_words.size() ||
        begin > image.words().size() || encoded_words.size() > image.words().size() - begin)
        throw std::runtime_error("stage1 logical allocation exceeds initial image");
    if (!std::equal(encoded_words.begin(), encoded_words.end(),
                    image.words().begin() + begin))
        throw std::runtime_error("stage1 logical allocation encoded content mismatch: " + match->id);
    return *match;
}

} // namespace pir_physical_binding
