#!/usr/bin/env python3
"""Build PIR/PSI from a clean checkout in a fresh directory and publish verified packages.

This is the supported release path for PIR/PSI. Direct use of the upstream
package writer retains upstream behaviour and does not have this release guard.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def run(command, *, cwd=None, env=None):
    subprocess.run([str(part) for part in command], cwd=cwd, env=env, check=True)


def git(source, *arguments):
    return subprocess.check_output(
        ["git", "-C", str(source), *arguments], text=True).strip()


def clean_revision(source, expected):
    require(re.fullmatch(r"[0-9a-f]{40}", expected) is not None,
            "expected commit must be a full lowercase SHA-1")
    actual = git(source, "rev-parse", "--verify", "HEAD")
    require(actual == expected,
            f"source HEAD {actual} differs from expected commit {expected}")
    require(not git(source, "status", "--porcelain=v1", "--untracked-files=normal"),
            "PIR/PSI release source tree is dirty")
    return actual


def preflight(source, build, output, expected):
    source = source.resolve(strict=True)
    build = build.absolute()
    output = output.absolute()
    require(not build.exists(), "release requires a fresh, nonexistent build directory")
    require(not output.exists(), "refusing to overwrite an existing delivery")
    require(not build.is_symlink() and not output.is_symlink(),
            "build and output paths must not be symlinks")
    require(source not in build.parents and source not in output.parents,
            "build and output must be outside the source checkout")
    clean_revision(source, expected)
    return source, build, output


def configured_binary(build, source, executable, commit):
    cache = (build / "CMakeCache.txt").read_text()
    require(f"CMAKE_HOME_DIRECTORY:INTERNAL={source}" in cache,
            "CMake cache source root differs from checked checkout")
    flags = list(build.glob("CMakeFiles/**/flags.make"))
    require(any(commit in path.read_text(errors="replace") for path in flags),
            "configured compile definitions do not contain the checked commit")
    require(commit.encode() in executable.read_bytes(),
            "producer binary does not contain the configured source commit")


def validate_packages(root, validator, commit):
    builds = sorted(root.rglob("provenance/build.json"))
    require(builds, "runner produced no application package provenance")
    records = []
    for path in builds:
        package = path.parent.parent
        info = json.loads(path.read_text())
        require(info.get("schema") == "hpu-application-package-build" and
                info.get("repository") == "inline-asm" and
                info.get("commit") == commit and
                info.get("worktree_state") == "clean-at-configure",
                f"incorrect package provenance: {path}")
        run([validator, package])
        records.append({"package": str(package.relative_to(root)),
                        "package_json_sha256": hashlib.sha256(
                            (package / "package.json").read_bytes()).hexdigest(),
                        "provenance_sha256": hashlib.sha256(path.read_bytes()).hexdigest()})
    return records


def deliver(args):
    source, build, output = preflight(
        args.source_root, args.build_dir, args.output, args.expected_commit)
    require(args.mode != "pir-dynamic" or args.stage1_patch_script is not None,
            "PIR dynamic replay requires --stage1-patch-script")
    require(args.mode != "psi-small" or args.prefix is not None,
            "PSI build requires --prefix")
    build.parent.mkdir(parents=True, exist_ok=True)
    output.parent.mkdir(parents=True, exist_ok=True)
    cmake = ["cmake", "-S", source, "-B", build,
             "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=ON",
             "-DHPU_ENABLE_SEAL_INTEGRATION=ON",
             "-DHPU_ENABLE_TRIDENT_PSI=ON",
             f"-DHPU_SEAL_SOURCE_DIR={source / 'third_party/modified-SEAL'}",
             f"-DHPU_TEST_CONFIG={source / 'config/fhe_test.conf'}"]
    if args.prefix is not None:
        cmake.append(f"-DCMAKE_PREFIX_PATH={args.prefix.resolve(strict=True)}")
    cmake.extend(args.cmake_arg)
    run(cmake)
    target = ("hpu_bfv_apsi_concentrated_example" if args.mode == "psi-small"
              else "hpu_bfv_sealpir_2d_concentrated_example")
    run(["cmake", "--build", build, "--target", target,
         "hpu_validate_package", "-j", str(args.jobs)])
    clean_revision(source, args.expected_commit)
    example = build / target
    validator = build / "hpu_validate_package"
    configured_binary(build, source, example, args.expected_commit)

    scratch = Path(tempfile.mkdtemp(prefix=f".{output.name}.staging-", dir=output.parent))
    staging = scratch / "delivery"
    environment = os.environ.copy()
    # Upstream application_delivery permits these overrides. The release tool
    # fixes them to the checked source and validates the serialized result.
    environment["HPU_DELIVERY_COMMIT"] = args.expected_commit
    environment["HPU_DELIVERY_WORKTREE_STATE"] = "clean-at-configure"
    if args.mode == "pir-dynamic":
        command = [sys.executable, source / "test/pir_dynamic_stage_handoff_test.py",
                   "--example", example, "--stage1-patch-script",
                   args.stage1_patch_script.resolve(strict=True), "--output", staging]
    elif args.mode == "pir-small":
        command = [sys.executable, source / "test/sealpir_2d_delivery_test.py",
                   "--example", example, "--validator", validator,
                   "--output", staging, "--profile", "small",
                   "--record-index", str(args.record_index)]
    else:
        command = [sys.executable, source / "test/apsi_concentrated_delivery_test.py",
                   "--example", example, "--validator", validator,
                   "--output", staging, "--profile", "small", "--scenario", "multi"]
    run(command, env=environment)
    clean_revision(source, args.expected_commit)
    records = validate_packages(staging, validator, args.expected_commit)
    receipt = {"schema": "pir-psi-verified-delivery-v1", "commit": args.expected_commit,
               "mode": args.mode, "source": str(source), "build": str(build),
               "configure_command": [str(item) for item in cmake],
               "build_target": target, "run_command": [str(item) for item in command],
               "cmake_version": subprocess.check_output(["cmake", "--version"], text=True).splitlines()[0],
               "python_version": sys.version.split()[0],
               "example_sha256": hashlib.sha256(example.read_bytes()).hexdigest(),
               "validator_sha256": hashlib.sha256(validator.read_bytes()).hexdigest(),
               "packages": records}
    (staging / "verified_delivery_receipt.json").write_text(
        json.dumps(receipt, indent=2) + "\n")
    os.rename(staging, output)
    scratch.rmdir()
    print(f"VERIFIED_PIR_PSI_DELIVERY_PASS {args.mode} {args.expected_commit} {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--expected-commit", required=True)
    parser.add_argument("--mode", required=True,
                        choices=("pir-dynamic", "pir-small", "psi-small"))
    parser.add_argument("--stage1-patch-script", type=Path)
    parser.add_argument("--prefix", type=Path)
    parser.add_argument("--record-index", type=int, default=8)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--cmake-arg", action="append", default=[])
    arguments = parser.parse_args()
    try:
        deliver(arguments)
    except (OSError, RuntimeError, subprocess.CalledProcessError, ValueError) as error:
        parser.exit(1, f"VERIFIED_PIR_PSI_DELIVERY_REJECTED: {error}\n")
