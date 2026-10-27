#!/usr/bin/env python3
"""Compare HPU trace fields with fixed Trident APSI and Kuku native functions."""

import argparse
import pathlib
import subprocess


def parse_hpu(lines):
    tokens = {}
    powers = {}
    polynomials = {}
    for line in lines.splitlines():
        if line.startswith("VECTOR_TOKEN "):
            fields = line.split()
            side, item = fields[1], int(fields[2])
            felts_at = fields.index("felts")
            locations_at = fields.index("locations")
            placed_at = fields.index("placed")
            tokens[(side, item)] = (
                tuple(map(int, fields[felts_at + 1:locations_at])),
                tuple(sorted(map(int, fields[locations_at + 1:placed_at]))),
                int(fields[placed_at + 1]),
            )
        elif line.startswith("VECTOR_POWER "):
            values = tuple(map(int, line.split()[1:]))
            powers[values[0]] = values[1:]
        elif line.startswith("VECTOR_SYNTH_POLY "):
            fields = line.split()
            split = fields.index("coefficients")
            polynomials[int(fields[1])] = (
                tuple(map(int, fields[3:split])),
                tuple(map(int, fields[split + 1:])),
            )
    return tokens, powers, polynomials


def parse_native(lines):
    tokens = {}
    placements = {}
    powers = {}
    polynomials = {}
    for line in lines.splitlines():
        fields = line.split()
        if not fields:
            continue
        if fields[0] == "TOKEN":
            index = int(fields[1])
            locations_at = fields.index("locations")
            tokens[index] = (
                tuple(map(int, fields[3:locations_at])),
                tuple(sorted(map(int, fields[locations_at + 1:]))),
            )
        elif fields[0] == "PLACED":
            placements[int(fields[1])] = int(fields[2])
        elif fields[0] == "POWER":
            values = tuple(map(int, fields[1:]))
            powers[values[0]] = values[1:]
        elif fields[0] == "POLY":
            split = fields.index("coefficients")
            polynomials[int(fields[1])] = (
                tuple(map(int, fields[3:split])),
                tuple(map(int, fields[split + 1:])),
            )
    return tokens, placements, powers, polynomials


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--example", required=True)
    parser.add_argument("--native", required=True)
    parser.add_argument("--fixture", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    output = pathlib.Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    native = subprocess.run([args.native], capture_output=True, text=True)
    (output / "native.log").write_text(native.stdout + native.stderr, encoding="utf-8")
    if native.returncode:
        raise RuntimeError(f"native vector program failed: {native.returncode}")
    hpu = subprocess.run([
        args.example, "--profile", "medium", "--match-mode", "trident",
        "--test-input", args.fixture, "--trace-vectors",
    ], capture_output=True, text=True)
    (output / "hpu.log").write_text(hpu.stdout + hpu.stderr, encoding="utf-8")
    if hpu.returncode:
        raise RuntimeError(f"HPU vector program failed: {hpu.returncode}")
    native_tokens, placements, native_powers, native_polynomials = parse_native(native.stdout)
    hpu_tokens, hpu_powers, hpu_polynomials = parse_hpu(hpu.stderr)
    mapping = {("S", 1): 0, ("S", 2): 1, ("S", 3): 2,
               ("R", 1): 0, ("R", 2): 1, ("R", 5): 3}
    if set(hpu_tokens) != set(mapping):
        raise AssertionError(f"unexpected HPU traced tokens: {set(hpu_tokens)}")
    for key, index in mapping.items():
        felts, locations, placed = hpu_tokens[key]
        if (felts, locations) != native_tokens[index]:
            raise AssertionError(f"Trident field/position mismatch: {key}")
        if key[0] == "R" and placed != placements[index]:
            raise AssertionError(f"Trident receiver placement mismatch: {key}")
    if hpu_powers != native_powers:
        raise AssertionError("Trident PowersDag topology differs")
    if hpu_polynomials != native_polynomials:
        raise AssertionError("Trident polynomial roots or coefficients differ")
    print(f"native field/location/placement/PowersDag/polynomial comparison: PASS ({output})")


if __name__ == "__main__":
    main()
