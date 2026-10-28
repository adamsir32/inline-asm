#!/usr/bin/env python3
"""Check raw-item set normalization before APSI sender polynomial creation."""

import argparse
import subprocess
import tempfile
from pathlib import Path


BASE = "S 1 11 21\nS 2 12 22\nR 1 11 21\nR 3 13 23\n"


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def run(example, root, name, contents):
    source = root / (name + ".input")
    dump = root / (name + ".dump")
    source.write_text(contents, encoding="utf-8")
    result = subprocess.run([example, "--profile", "small", "--match-mode", "trident",
                             "--test-input", str(source), "--dump-preprocessing", str(dump),
                             "--preprocess-only"], text=True, capture_output=True, check=False)
    return result, dump.read_text(encoding="utf-8") if dump.exists() else ""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--example", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        baseline, baseline_dump = run(args.example, root, "baseline", BASE)
        require(baseline.returncode == 0, baseline.stderr)
        require("META sender=2 receiver=2" in baseline_dump, "baseline count differs")
        for name, changed in (
            ("sender_duplicate", "S 1 11 21\n" + BASE),
            ("receiver_duplicate", BASE + "R 1 11 21\n"),
        ):
            result, dump = run(args.example, root, name, changed)
            require(result.returncode == 0, f"{name}: {result.stderr}")
            require(dump == baseline_dump, f"{name} changed sender roots or query layout")
        for name, contents in (
            ("conflicting_token", BASE + "S 1 99 21\n"),
            ("same_token_distinct_item", BASE + "S 4 11 21\n"),
            ("empty", "# no items\n"),
            ("missing_receiver", "S 1 11 21\n"),
            ("invalid", BASE + "X 4 14 24\n"),
        ):
            result, _ = run(args.example, root, name, contents)
            require(result.returncode != 0, f"{name} was accepted")
    print("APSI raw-item normalization: PASS")


if __name__ == "__main__":
    main()
