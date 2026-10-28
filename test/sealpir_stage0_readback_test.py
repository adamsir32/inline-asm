#!/usr/bin/env python3
"""Exercise the external stage0 image adapter with a labelled synthetic image.

The image is assembled from package Golden. This tests the boundary parser,
BR conversion and stage1 use, not an independent software or hardware readback.
"""

import argparse
import csv
import json
import subprocess
import tempfile
from pathlib import Path

from pir_record_oracle import expected_record


def run(command):
    return subprocess.run(command, capture_output=True, text=True, check=False)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--example", required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        delivery = root / "delivery"
        first = run([args.example, "--profile", "small", "--record-index", "0",
                     "--emit-dir", str(delivery)])
        require(first.returncode == 0, f"fixture generation failed: {first.stderr}")
        package_root = delivery / "stage0_concentrated_000"
        package = json.loads((package_root / "package.json").read_text(encoding="utf-8"))
        image = bytearray((package_root / package["image"]).read_bytes())
        with (package_root / package["golden_manifest"]).open(encoding="utf-8", newline="") as stream:
            golden = list(csv.DictReader(stream))
        require(bool(golden), "fixture package has no stage0 Golden limbs")
        for limb in golden:
            data = (package_root / limb["path"]).read_bytes()
            start = int(limb["line_offset"]) * 256
            require(len(data) == int(limb["payload_words"]) * 4,
                    "fixture Golden size differs from manifest")
            image[start:start + len(data)] = data
        readback = root / "synthetic-stage0-readback.bin"
        readback.write_bytes(image)
        record = root / "record.bin"
        command = [args.example, "--profile", "small", "--record-index", "0",
                   "--stage0-readback", str(readback), "--test-output-record", str(record)]
        second = run(command)
        require(second.returncode == 0, f"synthetic readback was rejected: {second.stderr}")
        require("AUDIT_STAGE0_READBACK_FNV1A64" in second.stderr,
                "readback path did not run")
        require("AUDIT_STAGE1_INPUT_FNV1A64" in second.stderr and
                "AUDIT_RECORD_FNV1A64" in second.stderr,
                "stage input or recovered record checksum missing")
        manifest = json.loads((delivery / "pipeline_manifest.json").read_text())
        require(record.read_bytes() == expected_record(0, int(manifest["database_seed"]),
                                                       int(manifest["record_bytes"])),
                "stage1 result differs from independent record generator")
        start = int(golden[0]["line_offset"]) * 256
        image[start] ^= 1
        readback.write_bytes(image)
        bad = run(command)
        require(bad.returncode != 0 and "readback differs" in bad.stderr,
                "corrupt stage0 output was accepted")
        readback.write_bytes(image[:-256])
        truncated = run(command)
        require(truncated.returncode != 0, "truncated stage0 image was accepted")
    print("synthetic stage0 readback adapter: PASS (not independent execution evidence)")


if __name__ == "__main__":
    main()
