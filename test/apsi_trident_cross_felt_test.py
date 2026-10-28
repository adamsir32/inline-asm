#!/usr/bin/env python3
"""Check a Kuku-aligned Trident APSI cross-felt match."""

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


def run(example: str, fixture: str) -> None:
    completed = subprocess.run(
        [example, "--profile", "medium", "--match-mode", "trident",
         "--test-input", fixture],
        capture_output=True, text=True,
    )
    if completed.returncode:
        raise RuntimeError(f"trident returned {completed.returncode}: {completed.stderr}")
    raw = audit_values(completed.stderr, "AUDIT_EXPECTED")
    reference = audit_values(completed.stderr, "AUDIT_APSI_REFERENCE")
    recovered = audit_values(completed.stderr, "AUDIT_RECOVERED")
    expected = [3]
    if raw or reference != expected or recovered != expected:
        raise AssertionError(
            f"trident: raw={raw}, APSI reference={reference}, recovered={recovered}"
        )
    if "seal_oracle=PASS hpu_software=PASS instruction_model=PASS" not in completed.stderr:
        raise AssertionError("trident: missing HPU software verification")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--example", required=True)
    parser.add_argument("--fixture", required=True)
    args = parser.parse_args()
    run(args.example, args.fixture)
    print("native-Kuku cross-felt Trident APSI differential: PASS")


if __name__ == "__main__":
    main()
