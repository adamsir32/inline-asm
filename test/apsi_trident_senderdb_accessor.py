#!/usr/bin/env python3
"""Add a read-only audit accessor to an isolated Trident SenderDB header copy.

The accessor exposes actual BinBundle instances and their native caches. It
does not alter insertion, hashing, polynomial generation, or query behavior.
"""

import argparse
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    source = args.source.read_text(encoding="utf-8")
    anchor = "            auto get_cache_at(std::uint32_t bundle_idx)\n"
    if source.count(anchor) != 1:
        raise ValueError("fixed Trident SenderDB header did not match the audit anchor")
    addition = (
        "            // Audit-only read access in the isolated reference copy.\n"
        "            const std::vector<BinBundle> &audit_bundles_at(\n"
        "                std::uint32_t bundle_idx) const\n"
        "            {\n"
        "                return bin_bundles_.at(bundle_idx);\n"
        "            }\n\n"
    )
    args.output.write_text(source.replace(anchor, addition + anchor), encoding="utf-8")


if __name__ == "__main__":
    main()
