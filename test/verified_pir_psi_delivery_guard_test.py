#!/usr/bin/env python3
"""Negative tests for the application-level release provenance guard."""

import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile


source = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "verified_pir_psi_delivery", source / "tools/verified_pir_psi_delivery.py")
guard = importlib.util.module_from_spec(spec)
spec.loader.exec_module(guard)


def rejects(function, fragment):
    try:
        function()
    except RuntimeError as error:
        if fragment not in str(error):
            raise AssertionError(f"wrong rejection: {error}") from error
    else:
        raise AssertionError(f"accepted {fragment}")


with tempfile.TemporaryDirectory(prefix="pir-psi-delivery-guard-") as temp:
    root = Path(temp)
    checkout = root / "source"
    checkout.mkdir()
    subprocess.run(["git", "init", "-q", checkout], check=True)
    (checkout / "input.txt").write_text("original\n")
    subprocess.run(["git", "-C", checkout, "add", "input.txt"], check=True)
    subprocess.run(["git", "-C", checkout, "-c", "user.name=Test",
                    "-c", "user.email=test@example.invalid", "commit", "-qm",
                    "fixture"], check=True)
    commit = guard.git(checkout, "rev-parse", "HEAD")
    build = root / "build"
    output = root / "delivery"
    guard.preflight(checkout, build, output, commit)
    rejects(lambda: guard.preflight(checkout, build, output, "0" * 40),
            "source HEAD")
    build.mkdir()
    rejects(lambda: guard.preflight(checkout, build, output, commit),
            "fresh, nonexistent build")
    build.rmdir()
    (checkout / "input.txt").write_text("changed\n")
    rejects(lambda: guard.preflight(checkout, build, output, commit), "dirty")
    (checkout / "input.txt").write_text("original\n")
    package = root / "packages" / "one"
    (package / "provenance").mkdir(parents=True)
    (package / "package.json").write_text("{}")
    (package / "provenance/build.json").write_text(json.dumps({
        "schema": "hpu-application-package-build", "repository": "inline-asm",
        "commit": "0" * 40, "worktree_state": "clean-at-configure"}))
    rejects(lambda: guard.validate_packages(root / "packages", "unused", commit),
            "incorrect package provenance")

print("VERIFIED_PIR_PSI_DELIVERY_GUARD_PASS stale/dirty/old-build/old-package rejected")
