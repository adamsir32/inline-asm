#!/usr/bin/env python3
"""Run real stage0 DDR export, CPU handoff and fixed-instruction stage1 replay."""

import argparse
import csv
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

from pir_record_oracle import expected_record


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def run(command, log):
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    log.write_text(result.stdout + "\n[stderr]\n" + result.stderr)
    return result


def package_data(path):
    manifest = json.loads((path / "package.json").read_text())
    image = (path / manifest["image"]).read_bytes()
    line_map = list(csv.DictReader((path / manifest["line_map"]).open(newline="")))
    golden = list(csv.DictReader((path / manifest["golden_manifest"]).open(newline="")))
    config = json.loads((path / manifest["memory_config"]).read_text())
    require(len(image) == config["used_lines"] * 256, "source package image length")
    return manifest, image, line_map, golden, config


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--example", required=True, type=Path)
    parser.add_argument("--stage1-patch-script", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    root = args.output.resolve()
    require(not root.exists(), "output directory already exists")
    root.mkdir(parents=True)
    example = str(args.example.resolve())
    script = str(args.stage1_patch_script.resolve())
    stage0 = root / "stage0-executed.u32.bin"
    command = [example, "--profile", "small", "--record-index", "8",
               "--stage0-executed-image", str(stage0),
               "--emit-dir", str(root / "packages"),
               "--dynamic-replay-dir", str(root / "replay"),
               "--stage1-patch-script", script]
    result = run(command, root / "main.log")
    require(result.returncode == 0, "dynamic handoff failed; see main.log")
    require(result.stderr.count("AUDIT_STAGE1_REPLAY_PASS") == 2 and
            "AUDIT_DYNAMIC_TWO_INPUTS_PASS" in result.stderr,
            "two fixed-instruction numerical replays did not complete")

    stage0_package = root / "packages" / "stage0_concentrated_000"
    stage1_package = root / "packages" / "stage1_concentrated_000"
    stage0_manifest, initial0, _, golden0, config0 = package_data(stage0_package)
    stage1_manifest, initial1, rows1, _, _ = package_data(stage1_package)
    final0 = stage0.read_bytes()
    require(len(final0) == len(initial0) == config0["used_lines"] * 256,
            "executed stage0 image length mismatch")
    require(golden0 and len(golden0) == 16, "stage0 output coverage mismatch")
    output_mask = bytearray(len(initial0))
    for limb in golden0:
        offset = int(limb["line_offset"]) * 256
        expected = (stage0_package / limb["path"]).read_bytes()
        require(final0[offset:offset + len(expected)] == expected,
                "executed stage0 output differs from independent Golden")
        output_mask[offset:offset + len(expected)] = b"\x01" * len(expected)
    outside_changed = sum(a != b for a, b, mask in
                          zip(initial0, final0, output_mask) if not mask)
    require(outside_changed > 0,
            "no non-output memory changed; suspected Golden-only reconstruction")

    baseline = root / "replay" / "baseline"
    changed = root / "replay" / "changed"
    baseline_image = (baseline / "stage1-patched.u32.bin").read_bytes()
    changed_image = (changed / "stage1-patched.u32.bin").read_bytes()
    require(baseline_image == initial1,
            "baseline physical encoding differs from compiler source image")
    require(len(changed_image) == len(initial1), "changed stage1 image length mismatch")
    mapping = dict(line.split("\t") for line in
                   (changed / "dynamic" / "dynamic_inputs.tsv").read_text().splitlines())
    require(len(mapping) == 96, "expected 96 dynamic physical input limbs")
    dynamic_mask = bytearray(len(initial1))
    for row in rows1:
        if row["allocation_id"] in mapping:
            offset = int(row["line_offset"]) * 256
            size = int(row["line_count"]) * 256
            dynamic_mask[offset:offset + size] = b"\x01" * size
    require(any(a != b for a, b in zip(initial1, changed_image)),
            "changed input did not modify stage1 image")
    require(all(a == b for a, b, mask in
                zip(initial1, changed_image, dynamic_mask) if not mask),
            "stage1 patch altered fixed data or addresses")
    for label in ("baseline", "changed"):
        replay = json.loads((root / "replay" / label / "replay_summary.json").read_text())
        require(replay["status"] == "PASS" and replay["fixed_instructions"] == 26700,
                f"{label} fixed instruction replay summary invalid")

    original_seed = int(json.loads((root / "packages" /
        "pipeline_manifest.json").read_text())["database_seed"])
    changed_seed = original_seed ^ 0x9E3779B97F4A7C15
    require((baseline / "record.bin").read_bytes() == expected_record(8, original_seed, 1024),
            "baseline recovered record differs from independent record oracle")
    require((changed / "record.bin").read_bytes() == expected_record(8, changed_seed, 1024),
            "changed recovered record differs from independent record oracle")

    # These are file boundary checks. Duplicate metadata and modulus-ID mismatch
    # are checked in C++ against the in-memory compiled package on every run.
    readback_command = [example, "--profile", "small", "--record-index", "8",
                        "--stage0-readback", str(root / "invalid-readback.bin")]
    first_offset = int(golden0[0]["line_offset"]) * 256
    invalid_cases = {
        "truncated": final0[:-256],
        "corrupted-output": final0[:first_offset] +
            bytes([final0[first_offset] ^ 1]) + final0[first_offset + 1:],
        "wrong-run": (root / "replay" / "changed-stage0-executed.u32.bin").read_bytes(),
        "wrong-bit-order": final0[:first_offset] +
            final0[first_offset + 4:first_offset + 8] +
            final0[first_offset:first_offset + 4] + final0[first_offset + 8:],
        "missing-output": final0[:first_offset] + b"\x00" * 16384 +
            final0[first_offset + 16384:],
    }
    for name, data in invalid_cases.items():
        (root / "invalid-readback.bin").write_bytes(data)
        bad = run(readback_command, root / f"negative-{name}.log")
        require(bad.returncode != 0, f"{name} stage0 image was accepted")
    (root / "invalid-readback.bin").unlink()

    for name in ("missing-dynamic-limb", "noncanonical-dynamic-limb"):
        bad_dir = root / name
        shutil.copytree(changed / "dynamic", bad_dir,
                        ignore=shutil.ignore_patterns("complete_patch"))
        mapping_path = bad_dir / "dynamic_inputs.tsv"
        lines = mapping_path.read_text().splitlines()
        if name == "missing-dynamic-limb":
            mapping_path.write_text("\n".join(lines[:-1]) + "\n")
        else:
            first_file = lines[0].split("\t")[1]
            (bad_dir / first_file).write_bytes(b"\xff" * 16384)
        command = ["python3", script, "materialize", "--package", str(stage1_package),
                   "--directory", str(bad_dir), "--output", str(root / (name + ".bin"))]
        bad = run(command, root / f"negative-{name}.log")
        require(bad.returncode != 0, f"{name} stage1 patch was accepted")

    summary = {
        "status": "PASS", "model": "encoded_instruction_executor",
        "stage0_package_json_sha256": digest((stage0_package / "package.json").read_bytes()),
        "stage0_initial_image_sha256": digest(initial0),
        "stage0_executed_image_sha256": digest(final0),
        "stage0_used_lines": config0["used_lines"],
        "stage0_output_limbs": len(golden0),
        "stage0_nonoutput_changed_bytes": outside_changed,
        "stage1_package_json_sha256": digest((stage1_package / "package.json").read_bytes()),
        "stage1_inst32_sha256": digest(
            (stage1_package / stage1_manifest["program_inst32"]).read_bytes()),
        "stage1_baseline_input_sha256": digest(baseline_image),
        "stage1_changed_input_sha256": digest(changed_image),
        "stage1_baseline_executed_sha256": digest(
            (baseline / "stage1-executed.u32.bin").read_bytes()),
        "stage1_changed_executed_sha256": digest(
            (changed / "stage1-executed.u32.bin").read_bytes()),
        "dynamic_input_limbs": len(mapping),
        "negative_cases": sorted(invalid_cases) +
            ["missing-dynamic-limb", "noncanonical-dynamic-limb"],
        "two_input_numeric_replay": "PASS",
        "rtl": "NOT RUN", "hardware": "NOT RUN",
    }
    (root / "handoff_summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print("PIR real dynamic stage handoff: PASS")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"PIR dynamic handoff: FAIL: {error}", file=sys.stderr)
        raise
