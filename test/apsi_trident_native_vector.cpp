// Build this standalone driver against fixed Trident e3f819f and Kuku 2.1.
// It calls the original APSI encoding, polynomial, and PowersDag functions.
#include <apsi/item.h>
#include <apsi/powers.h>
#include <apsi/util/db_encoding.h>
#include <apsi/util/interpolate.h>
#include <apsi/util/utils.h>
#include <kuku/kuku.h>
#include <poseidon/basics/modulus.h>

#include <cstdint>
#include <iostream>
#include <set>
#include <vector>

int main()
{
    const poseidon::Modulus modulus(188417);
    const std::vector<kuku::item_type> tokens{
        kuku::make_item(UINT64_C(81985529216486895), UINT64_C(18364758544493064720)),
        kuku::make_item(UINT64_C(81985529216486895), UINT64_C(18364758544493064721)),
        kuku::make_item(UINT64_C(1), UINT64_C(0)),
        kuku::make_item(UINT64_C(81985529216486895), UINT64_C(18364758544493064722))
    };
    kuku::KukuTable table(1638, 0, 3, kuku::make_item(0, 0), 500,
                          kuku::make_item(0, 0));
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const auto& token = tokens[i];
        const apsi::BitstringView<const unsigned char> bits(token.data(), 85);
        const auto felts = apsi::util::bits_to_field_elts(bits, modulus);
        std::cout << "TOKEN " << i << " felts";
        for (const auto felt : felts) std::cout << ' ' << felt;
        std::cout << " locations";
        for (const auto location : table.all_locations(token))
            std::cout << ' ' << location;
        std::cout << '\n';
    }
    // The receiver fixture uses token indices 0, 1, and 3 in this order.
    for (const auto index : {0U, 1U, 3U}) {
        if (!table.insert(tokens[index])) return 2;
        std::cout << "PLACED " << index << ' '
                  << table.query(tokens[index]).location() << '\n';
    }
    const apsi::BitstringView<const unsigned char> bits0(tokens[0].data(), 85);
    const apsi::BitstringView<const unsigned char> bits1(tokens[1].data(), 85);
    const auto felts0 = apsi::util::bits_to_field_elts(bits0, modulus);
    const auto felts1 = apsi::util::bits_to_field_elts(bits1, modulus);
    for (std::size_t field = 0; field < 5; ++field) {
        const auto coefficients = apsi::util::polyn_with_roots(
            {felts0[field], felts1[field]}, modulus);
        std::cout << "POLY " << field << " roots " << felts0[field]
                  << ' ' << felts1[field] << " coefficients";
        for (const auto coefficient : coefficients)
            std::cout << ' ' << coefficient;
        std::cout << '\n';
    }
    apsi::PowersDag dag;
    const std::set<std::uint32_t> sources{1,2,3,4,5,6,18,30,42,54,60};
    if (!dag.configure(sources, apsi::util::create_powers_set(5,125))) return 3;
    dag.apply([](const auto& node) {
        std::cout << "POWER " << node.power << ' ' << node.depth << ' '
                  << node.parents.first << ' ' << node.parents.second << '\n';
    });
}
