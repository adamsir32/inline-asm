#include "../examples/pir_physical_binding.hpp"

#include <stdexcept>
#include <string>
#include <vector>

using hpu::runtime::AllocationKind;
using hpu::runtime::HpuMemImage;

static std::string name(unsigned rank, unsigned mod) {
    return "frontend/plains/plain_" + std::to_string(rank + 3) +
           "/multiply/level_2/c0/mod" + std::to_string(mod);
}

static bool rejects(const HpuMemImage& image, unsigned rank, unsigned mod,
                    const std::vector<std::uint32_t>& words) {
    try {
        (void)pir_physical_binding::logical_plaintext_limb(image, rank, mod, words);
        return false;
    } catch (const std::runtime_error&) { return true; }
}

int main() {
    HpuMemImage image(4096);
    const std::vector<std::uint32_t> zero(64, 0);
    const std::vector<std::uint32_t> repeated(64, 7);
    image.add("frontend/plains/plain_1/multiply/level_2/c0/mod0",
              repeated, AllocationKind::plaintext, true);
    image.add("frontend/plains/plain_2/multiply/level_2/c0/mod0",
              zero, AllocationKind::plaintext, true);
    for (unsigned rank = 0; rank < 32; ++rank)
        for (unsigned mod = 0; mod < 3; ++mod)
            image.add(name(rank, mod), rank % 2 ? zero : repeated,
                      AllocationKind::plaintext, true);
    for (unsigned rank = 0; rank < 32; ++rank)
        for (unsigned mod = 0; mod < 3; ++mod) {
            const auto& expected = rank % 2 ? zero : repeated;
            const auto& allocation = pir_physical_binding::logical_plaintext_limb(
                image, rank, mod, expected);
            if (allocation.id != name(rank, mod) || allocation.word_count != 64)
                return 1;
        }
    if (!rejects(image, 32, 0, zero) || !rejects(image, 0, 3, repeated) ||
        !rejects(image, 0, 0, zero) || !rejects(image, 0, 0, {7, 7}) ||
        !rejects(image, 1, 0, repeated)) return 2;
    HpuMemImage missing(128);
    missing.add(name(0, 0), repeated, AllocationKind::plaintext, true);
    if (!rejects(missing, 1, 0, zero)) return 3;
    HpuMemImage duplicate(128);
    duplicate.add(name(0, 0), repeated, AllocationKind::plaintext, true);
    duplicate.add("frontend/plains/plain_3/multiply/level_3/c0/mod0",
                  repeated, AllocationKind::plaintext, true);
    if (!rejects(duplicate, 0, 0, repeated)) return 4;
    return 0;
}
