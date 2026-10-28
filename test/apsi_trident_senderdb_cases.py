#!/usr/bin/env python3
"""Select repeatable SenderDB cases from a fixed-key native OPRF/Kuku catalog."""

import argparse
import json
from collections import defaultdict
from pathlib import Path


def read_catalog(path: Path):
    items = {}
    for line in path.read_text(encoding="ascii").splitlines():
        parts = line.split()
        felts_at = parts.index("felts")
        locations_at = parts.index("locations")
        item = int(parts[0])
        items[item] = (
            tuple(map(int, parts[felts_at + 1:locations_at])),
            frozenset(map(int, parts[locations_at + 1:])),
        )
    if len(items) < 1000:
        raise ValueError("native catalog is too small for the boundary cases")
    return items


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--catalog", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    items = read_catalog(args.catalog)
    ids = sorted(items)
    args.output_dir.mkdir(parents=True, exist_ok=True)

    shared_location = min(items[1][1])
    shared = [item for item in ids if shared_location in items[item][1]]
    if len(shared) < 126:
        raise RuntimeError("catalog lacks 126 items at the selected Kuku location")
    different = []
    used_locations = set()
    for item in ids:
        if items[item][1].isdisjoint(used_locations):
            different.append(item)
            used_locations.update(items[item][1])
        if len(different) == 3:
            break
    if len(different) != 3:
        raise RuntimeError("cannot find three disjoint Kuku candidates")
    partial_groups = defaultdict(list)
    for item in ids:
        felts, locations = items[item]
        for location in locations:
            partial_groups[(location, felts[0])].append(item)
    partial = next(
        (group[:2] for _, group in sorted(partial_groups.items())
         if len(group) >= 2 and items[group[0]][0] != items[group[1]][0]),
        None,
    )
    if partial is None:
        raise RuntimeError("catalog lacks a shared-bin partial-field collision")
    boundary_low = next(item for item in ids if 0 in items[item][1])
    boundary_high = next(item for item in ids if 1637 in items[item][1]
                         and item != boundary_low)
    cases = {
        "single": ([1], [1, 2]),
        "different_bins": (different, different[:2] + [100000]),
        "shared_bin": (shared[:3], [shared[0], 100000]),
        "partial_felt": (partial, [partial[0], 100000]),
        "empty": ([1, 2, 3], [4, 5]),
        "single_match": ([1, 2, 3], [2, 4, 5]),
        "multiple_matches": ([1, 2, 3], [1, 2, 4]),
        "kuku_boundary": ([boundary_low, boundary_high],
                           [boundary_low, boundary_high]),
        "ps_degree_30": (shared[:30], [shared[0], 100000]),
        "near_capacity_124": (shared[:124], [shared[0], 100000]),
        "extra_bundle_125": (shared[:125], [shared[0], 100000]),
        "extra_bundle_126": (shared[:126], [shared[0], 100000]),
    }
    manifest = {}
    for name, (sender, receiver) in cases.items():
        sender = sorted(set(sender))
        receiver = sorted(set(receiver))
        if not all(item in items for item in sender + receiver):
            raise RuntimeError(f"{name} uses an item outside the native catalog")
        path = args.output_dir / f"{name}.raw"
        path.write_text(
            "# raw Item(id,0); fixed Trident OPRF scalar 1\n" +
            "".join(f"S {item}\n" for item in sender) +
            "".join(f"R {item}\n" for item in receiver),
            encoding="ascii",
        )
        manifest[name] = {
            "sender_size": len(sender), "receiver_size": len(receiver),
            "shared_location": shared_location if "capacity" in name or name.startswith(("shared", "ps_", "extra")) else None,
            "raw_input": path.name,
        }
    (args.output_dir / "cases.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {len(cases)} native SenderDB cases to {args.output_dir}")


if __name__ == "__main__":
    main()
