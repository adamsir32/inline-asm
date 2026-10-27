#!/usr/bin/env python3
"""Generate and validate every package in a two-stage SealPIR delivery."""

import argparse
import json
import pathlib
import shutil
import subprocess

from pir_record_oracle import expected_record


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--example", required=True)
    parser.add_argument("--validator", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--profile", choices=("small", "full", "trident"), required=True)
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
    expected_source = (
        "SEAL oracle ciphertext accepted only after coefficient-exact HPU software "
        "and instruction-model verification"
    )
    if manifest.get("cross_stage_source") != expected_source:
        raise RuntimeError("SealPIR pipeline did not enforce verified stage-boundary data")
    actual_record = (output / "actual_record.bin").read_bytes()
    if (output / "expected_record.bin").read_bytes() != actual_record:
        raise RuntimeError("SealPIR binary golden differs from the recovered record")
    independent_record = expected_record(args.record_index,
        int(manifest["database_seed"]), int(manifest["record_bytes"]))
    if actual_record != independent_record:
        raise RuntimeError("SealPIR recovered record differs from the external byte oracle")
    packages = manifest.get("packages", [])
    if len(packages) != 2:
        raise RuntimeError(f"expected 2 SealPIR packages, got {len(packages)}")
    if manifest.get("package_strategy") != "concentrated recursive stages":
        raise RuntimeError("two-package delivery is not marked as concentrated")
    pipeline_steps = [step.get("step") for step in manifest.get("execution_pipeline", [])]
    if pipeline_steps != [
        "hpu_stage0",
        "cpu_decompose",
        "hpu_stage1",
        "cpu_recompose_decrypt",
    ]:
        raise RuntimeError("concentrated delivery has an incomplete execution pipeline")
    for package in packages:
        directory = output / package["directory"]
        subprocess.run([args.validator, str(directory)], check=True)
    print(f"validated {len(packages)} SealPIR HPU packages in {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
