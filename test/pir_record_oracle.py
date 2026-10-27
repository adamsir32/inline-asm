#!/usr/bin/env python3
"""External deterministic-database record oracle for phase-2 PIR tests."""

import argparse
import hashlib
import json
from pathlib import Path

MASK = (1 << 64) - 1
DEFAULT_SEED = 0x5345414C50495232


def splitmix64(value):
    value = (value + 0x9E3779B97F4A7C15) & MASK
    value = ((value ^ (value >> 30)) * 0xBF58476D1CE4E5B9) & MASK
    value = ((value ^ (value >> 27)) * 0x94D049BB133111EB) & MASK
    return value ^ (value >> 31)


def expected_record(index, seed, record_bytes):
    result = bytearray()
    for offset in range(record_bytes):
        word = splitmix64(seed ^ ((index << 32) & MASK) ^ (offset // 8))
        result.append((word >> (8 * (offset % 8))) & 0xFF)
    return bytes(result)


def compare(index, seed, filename, record_bytes):
    expected = expected_record(index, seed, record_bytes)
    actual = filename.read_bytes()
    return {"index": index, "seed": seed, "file": str(filename),
            "size": len(actual), "expected_sha256": hashlib.sha256(expected).hexdigest(),
            "actual_sha256": hashlib.sha256(actual).hexdigest(),
            "match": expected == actual}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--records", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    result = []
    for path in sorted(args.records.glob("*.bin")):
        # Names: <profile>_<index>_seed_<seed>.bin.
        fields = path.stem.split("_")
        if len(fields) != 4 or fields[2] != "seed":
            continue
        record_bytes = 288 if fields[0] == "trident" else 1024
        item = compare(int(fields[1]), int(fields[3]), path, record_bytes)
        item["profile"] = fields[0]
        result.append(item)
    args.out.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps({"checked": len(result),
                      "matched": sum(item["match"] for item in result),
                      "failed": [item for item in result if not item["match"]]}, indent=2))


if __name__ == "__main__":
    main()
