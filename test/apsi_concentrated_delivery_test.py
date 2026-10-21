#!/usr/bin/env python3
"""Generate and validate the concentrated APSI-compatible PSI package."""

import argparse
import json
import pathlib
import shutil
import subprocess
import time

try:
    import resource
except ImportError:  # pragma: no cover - Windows developer builds
    resource = None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--example", required=True)
    parser.add_argument("--validator", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--profile", choices=("small", "medium"), required=True)
    parser.add_argument("--scenario", choices=("empty", "single", "multi", "boundary"), required=True)
    parser.add_argument("--optimize-resources", action="store_true")
    args = parser.parse_args()

    output = pathlib.Path(args.output)
    if output.exists():
        shutil.rmtree(output)
    command = [
        args.example,
        "--profile", args.profile,
        "--scenario", args.scenario,
        "--emit-dir", str(output),
    ]
    if args.optimize_resources:
        command.append("--optimize-resources")
    started = time.monotonic()
    subprocess.run(command, check=True)
    wall_seconds = time.monotonic() - started
    peak_kib = (
        resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
        if resource is not None else None
    )

    manifest_path = output / "pipeline_manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    if manifest.get("hpu_application_packages") != 1:
        raise RuntimeError("concentrated PSI delivery did not produce exactly one package")
    for field in (
        "intersection_match",
        "seal_oracle_per_node_verified",
        "hpu_software_executor_verified",
        "host_instruction_model_verified",
    ):
        if manifest.get(field) is not True:
            raise RuntimeError(f"APSI delivery reports {field}=false")
    if manifest.get("real_hardware_verified") is not False:
        raise RuntimeError("software delivery must not claim real hardware verification")
    if "fixture" not in manifest.get("oprf_mode", ""):
        raise RuntimeError("cropped OPRF boundary is not explicit")
    set_data = json.loads((output / "set_test_data.json").read_text(encoding="utf-8"))
    if set_data["expected_intersection"] != set_data["recovered_intersection"]:
        raise RuntimeError("recovered PSI intersection differs from the CPU golden")
    packages = manifest.get("packages", [])
    if len(packages) != 1 or packages[0].get("directory") != "hpu_query":
        raise RuntimeError("APSI pipeline manifest has an invalid package dependency")
    subprocess.run([args.validator, str(output / packages[0]["directory"])], check=True)

    manifest["delivery_process_wall_seconds"] = round(wall_seconds, 3)
    manifest["delivery_process_peak_rss_kib"] = peak_kib
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(
        f"validated one APSI-compatible HPU package in {output}; "
        f"wall={wall_seconds:.3f}s peak_rss={peak_kib}KiB"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
