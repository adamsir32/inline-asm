#include "../examples/pir_physical_binding.hpp"

#include <stdexcept>
#include <string>
#include <vector>

int main()
{
    using hpu::runtime::AllocationKind;
    hpu::runtime::HpuMemImage image(16);
    const std::vector<std::uint32_t> first{1, 2, 3, 4};
    const std::vector<std::uint32_t> second{5, 6, 7, 8};
    image.add("arbitrary/alpha/mod0", first, AllocationKind::plaintext, true);
    image.add("arbitrary/beta/mod0", second, AllocationKind::plaintext, true);
    const auto& found = pir_physical_binding::unique_plaintext_limb(image, 0, second);
    if (found.id != "arbitrary/beta/mod0") return 1;
    try {
        (void)pir_physical_binding::unique_plaintext_limb(image, 1, second);
        return 2;
    } catch (const std::runtime_error&) {}
    try {
        (void)pir_physical_binding::unique_plaintext_limb(image, 0, {5, 6});
        return 3;
    } catch (const std::runtime_error&) {}
    image.add("arbitrary/duplicate/mod0", second, AllocationKind::plaintext, true);
    try {
        (void)pir_physical_binding::unique_plaintext_limb(image, 0, second);
        return 4;
    } catch (const std::runtime_error& error) {
        if (std::string(error.what()).find("ambiguous") == std::string::npos) return 5;
    }
    return 0;
}
