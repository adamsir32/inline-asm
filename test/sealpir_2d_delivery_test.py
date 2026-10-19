#!/usr/bin/env python3
"""Generate and validate every package in a two-stage SealPIR delivery."""

import argparse
import json
import pathlib
import shutil
import subprocess


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--example", required=True)
    parser.add_argument("--validator", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--profile", choices=("small", "full"), required=True)
    parser.add_argument("--record-index", type=int, required=True)
    args = parser.parse_args()

    output = pathlib.Path(args.output)
    if output.exists():
        shutil.rmtree(output)
    subprocess.run(
        [
            args.example,
            "--profile",
            args.profile,
            "--record-index",
            str(args.record_index),
            "--emit-dir",
            str(output),
        ],
        check=True,
    )

    manifest_path = output / "pipeline_manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("semantic_record_match") is not True:
        raise RuntimeError("SealPIR pipeline manifest reports a record mismatch")
    if manifest.get("cross_stage_source") != "BFV software executor memory (not SEAL oracle)":
        raise RuntimeError("SealPIR pipeline did not preserve the executor data dependency")
    if (output / "expected_record.bin").read_bytes() != (output / "actual_record.bin").read_bytes():
        raise RuntimeError("SealPIR binary golden differs from the recovered record")
    packages = manifest.get("packages", [])
    if len(packages) < 4:
        raise RuntimeError("SealPIR delivery is missing one or more HPU stages")
    for package in packages:
        directory = output / package["directory"]
        subprocess.run([args.validator, str(directory)], check=True)
    print(f"validated {len(packages)} SealPIR HPU packages in {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
