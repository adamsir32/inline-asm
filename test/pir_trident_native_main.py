#!/usr/bin/env python3
"""Prepare an isolated Trident PIR main with the HPU test database bytes.

Only the reference example's input/output harness is changed. The fixed
Trident pir.cpp, pir_client.cpp, and pir_server.cpp remain untouched.
"""

import argparse
from pathlib import Path


def replace_once(source: str, old: str, new: str) -> str:
    if source.count(old) != 1:
        raise ValueError(f"expected exactly one occurrence: {old[:55]!r}")
    return source.replace(old, new)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source = args.source.read_text(encoding="utf-8")
    source = replace_once(source, "#include <memory>\n", "#include <memory>\n#include <fstream>\n")
    source = replace_once(source, "using namespace poseidon::pir;\n", """using namespace poseidon::pir;

static uint64_t fixture_splitmix64(uint64_t value)
{
    value += UINT64_C(0x9e3779b97f4a7c15);
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}
""")
    start = source.index("    random_device rd;")
    end = source.index("\n    // Measure database setup", start)
    source = source[:start] + """    constexpr uint64_t database_seed = UINT64_C(0x5345414c50495232);
    for (uint64_t i = 0; i < number_of_items; i++)
    {
        for (uint64_t j = 0; j < size_per_item; j++)
        {
            const uint64_t word = fixture_splitmix64(database_seed ^
                (i << 32) ^ (j / 8));
            const uint8_t val = static_cast<uint8_t>(word >> (8 * (j % 8)));
            db.get()[(i * size_per_item) + j] = val;
            db_copy.get()[(i * size_per_item) + j] = val;
        }
    }
""" + source[end:]
    source = replace_once(
        source,
        "uint64_t ele_index = rd() % number_of_items;        // element in DB at random position",
        "uint64_t ele_index = argc > 1 ? std::stoull(argv[1]) : 65535;\n"
        "    if (ele_index >= number_of_items) return 2;",
    )
    source = replace_once(source, "    assert(elems.size() == size_per_item);\n", """    assert(elems.size() == size_per_item);
    if (argc > 2)
    {
        std::ofstream output(argv[2], std::ios::binary);
        output.write(reinterpret_cast<const char *>(elems.data()),
                     static_cast<std::streamsize>(elems.size()));
        if (!output) return 3;
    }
""")
    args.output.write_text(source, encoding="utf-8")


if __name__ == "__main__":
    main()
