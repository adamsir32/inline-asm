// Deterministically construct a cross-felt APSI match using the native Kuku
// location functions. Build separately against Kuku 2.1; no HPU APIs needed.
#include <kuku/locfunc.h>

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

int main()
{
    constexpr std::size_t table_size = 1638;
    constexpr std::uint64_t low_a = UINT64_C(0x0123456789abcdef);
    constexpr std::uint64_t high_a = UINT64_C(0x123456);
    constexpr std::uint64_t low_b = low_a ^ (UINT64_C(1) << 10);
    constexpr std::uint64_t effective_high_b = high_a ^ (UINT64_C(1) << 6);
    std::vector<kuku::LocFunc> hashes;
    for (std::uint64_t i = 0; i < 3; ++i)
        hashes.emplace_back(table_size, kuku::make_item(i, 0));
    const auto target = hashes[0](kuku::make_item(low_a, high_a));
    std::uint64_t high_b = 0, high_receiver = 0;
    for (std::uint64_t nonce = 1; nonce < UINT64_C(1000000); ++nonce) {
        const auto candidate = effective_high_b | (nonce << 26);
        bool found = false;
        for (const auto& hash : hashes)
            found = found || hash(kuku::make_item(low_b, candidate)) == target;
        if (found) { high_b = candidate; break; }
    }
    for (std::uint64_t nonce = 1; nonce < UINT64_C(1000000); ++nonce) {
        const auto candidate = effective_high_b | (nonce << 26);
        if (hashes[0](kuku::make_item(low_a, candidate)) == target) {
            high_receiver = candidate;
            break;
        }
    }
    if (!high_b || !high_receiver)
        throw std::runtime_error("Kuku cross-felt search exhausted its nonce range");
    // A and B are distinct in felt 0 and felt 3. The receiver uses felt 0
    // from A and felt 3 from B, while all three share a Kuku table location.
    // The high-word nonce lies above the 90 APSI item bits.
    std::cout << "# Kuku shared location " << target << '\n';
    std::cout << "S 1 " << low_a << ' ' << high_a << '\n';
    std::cout << "S 2 " << low_b << ' ' << high_b << '\n';
    std::cout << "R 3 " << low_a << ' ' << high_receiver << '\n';
}
