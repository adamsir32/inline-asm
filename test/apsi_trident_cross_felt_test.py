#!/usr/bin/env python3
"""Check a Kuku-aligned APSI cross-felt match and the exact-mode contrast."""

import argparse
import subprocess


def audit_values(output: str, marker: str) -> list[int]:
    prefix = marker + " "
    for line in output.splitlines():
        if line == marker:
            return []
        if line.startswith(prefix):
            return [int(value) for value in line[len(prefix):].split()]
    raise AssertionError(f"missing {marker}")


def run(example: str, fixture: str, mode: str) -> None:
    completed = subprocess.run(
        [example, "--profile", "medium", "--match-mode", mode,
         "--test-input", fixture],
        capture_output=True, text=True,
    )
    if completed.returncode:
        raise RuntimeError(f"{mode} returned {completed.returncode}: {completed.stderr}")
    raw = audit_values(completed.stderr, "AUDIT_EXPECTED")
    reference = audit_values(completed.stderr, "AUDIT_APSI_REFERENCE")
    recovered = audit_values(completed.stderr, "AUDIT_RECOVERED")
    expected = [3] if mode == "trident" else []
    if raw or reference != expected or recovered != expected:
        raise AssertionError(
            f"{mode}: raw={raw}, APSI reference={reference}, recovered={recovered}"
        )
    if "seal_oracle=PASS hpu_software=PASS instruction_model=PASS" not in completed.stderr:
        raise AssertionError(f"{mode}: missing HPU software verification")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--example", required=True)
    parser.add_argument("--fixture", required=True)
    args = parser.parse_args()
    for mode in ("trident", "exact"):
        run(args.example, args.fixture, mode)
    print("native-Kuku cross-felt APSI/exact differential: PASS")


if __name__ == "__main__":
    main()
