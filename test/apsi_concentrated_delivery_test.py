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
    parser.add_argument("--profile", choices=("small", "medium", "target"), required=True)
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
        "--match-mode", "trident",
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
    packages = manifest.get("packages", [])
    if manifest.get("match_mode") != "trident":
        raise RuntimeError("PSI package match mode differs from request")
    if not packages or manifest.get("hpu_application_packages") != len(packages):
        raise RuntimeError("concentrated PSI delivery package count is inconsistent")
    if args.profile != "target" and len(packages) != 1:
        raise RuntimeError("small and medium PSI delivery must contain one package")
    for field in (
        "intersection_match",
        "apsi_reference_match",
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
    expected = set_data["apsi_reference_matches"]
    if expected != set_data["recovered_intersection"]:
        raise RuntimeError("recovered PSI intersection differs from the selected CPU reference")
    for package in packages:
        if not package.get("directory"):
            raise RuntimeError("APSI pipeline manifest has an unnamed package")
        subprocess.run([args.validator, str(output / package["directory"])], check=True)

    manifest["delivery_process_wall_seconds"] = round(wall_seconds, 3)
    manifest["delivery_process_peak_rss_kib"] = peak_kib
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(
        f"validated {len(packages)} APSI-style HPU packages in {output}; "
        f"wall={wall_seconds:.3f}s peak_rss={peak_kib}KiB"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
