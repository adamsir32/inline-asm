#!/usr/bin/env python3
"""Run deterministic medium PSI cases against the external set intersection."""

import argparse
import json
import re
import subprocess
from pathlib import Path

from generate_psi_hpu_cases import make_case
from psi_cpu_model import token


def parse_audit_items(text: str, marker: str) -> list[int]:
    match = re.search(rf"^{marker}([^\n]*)$", text, re.MULTILINE)
    if match is None:
        raise RuntimeError(f"missing {marker} in application log")
    return [int(value) for value in match.group(1).split()]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--example", type=Path, required=True)
    parser.add_argument("--out-dir", type=Path, required=True)
    parser.add_argument("--count", type=int, default=20)
    parser.add_argument("--base-seed", type=int, default=91827000)
    args = parser.parse_args()
    if args.count < 1:
        raise ValueError("--count must be positive")
    args.out_dir.mkdir(parents=True, exist_ok=True)
    summary = []
    for index in range(args.count):
        seed, mode, sender, receiver, model = make_case(index, args.base_seed)
        input_path = args.out_dir / f"case_{index:02d}.input"
        with input_path.open("w", encoding="ascii") as stream:
            stream.write(f"# genuine test_oprf tokens seed={seed} mode={mode}\n")
            for side, items in (("S", sender), ("R", receiver)):
                for item in items:
                    low, high = token(item)
                    stream.write(f"{side} {item} {low} {high}\n")
        command = [str(args.example), "--profile", "medium", "--test-input",
                   str(input_path), "--test-require-fixture-tokens"]
        completed = subprocess.run(command, text=True, capture_output=True)
        log = completed.stdout + completed.stderr
        (args.out_dir / f"case_{index:02d}.log").write_text(log, encoding="utf-8")
        if completed.returncode:
            raise RuntimeError(f"case {index} exited {completed.returncode}; see log")
        expected = sorted(model["expected"])
        audit_expected = parse_audit_items(completed.stderr, "AUDIT_EXPECTED")
        recovered = parse_audit_items(completed.stderr, "AUDIT_RECOVERED")
        if audit_expected != expected or recovered != expected:
            raise RuntimeError(f"case {index} disagrees with independent set intersection")
        if "seal_oracle=PASS hpu_software=PASS instruction_model=PASS" not in log:
            raise RuntimeError(f"case {index} lacks software verification evidence")
        summary.append({"case": index, "seed": seed, "mode": mode,
                        "sender_size": len(sender), "receiver_size": len(receiver),
                        "intersection_size": len(expected),
                        "old_approximate_cpu_model_correct": model["correct"],
                        "max_bin_load": model["max_bin_load"],
                        "max_cuckoo_chain": model["max_cuckoo_chain"],
                        "hpu_exact_result_match": True})
        print(f"case {index:02d}: PASS, intersection={len(expected)}", flush=True)
    (args.out_dir / "summary.json").write_text(json.dumps(summary, indent=2) + "\n",
                                                encoding="utf-8")
    print(f"{len(summary)}/{len(summary)} random medium PSI cases passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
