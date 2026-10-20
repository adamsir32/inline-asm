#!/usr/bin/env python3
"""Measure representative full-profile SealPIR concentrated packages."""

import argparse
import json
import os
import pathlib
import signal
import subprocess
import tempfile
import time


CANDIDATES = (1, 4, 8, 16, 32, 85)
ASSESSMENT_PREFIX = "SHARD_ASSESSMENT "


def process_rss_kib(pid: int) -> int | None:
    status = pathlib.Path(f"/proc/{pid}/status")
    if not status.exists():
        return None
    for line in status.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("VmRSS:"):
            return int(line.split()[1])
    return None


def assess(example: str, outputs: int, timeout_seconds: int, optimize: bool) -> dict:
    result = {
        "outputs_per_package": outputs,
        "resource_optimizer": optimize,
        "projected_stage0_packages": (85 + outputs - 1) // outputs,
        "projected_stage1_packages": (8 + outputs - 1) // outputs,
    }
    result["projected_total_packages"] = (
        result["projected_stage0_packages"] + result["projected_stage1_packages"]
    )
    with tempfile.TemporaryDirectory(prefix="sealpir-assessment-") as temporary:
        stdout_path = pathlib.Path(temporary) / "stdout.txt"
        stderr_path = pathlib.Path(temporary) / "stderr.txt"
        command = [
            example,
            "--profile",
            "full",
            "--record-index",
            "65535",
            "--assessment-output-count",
            str(outputs),
        ]
        if optimize:
            command.append("--optimize-resources")
        started = time.monotonic()
        peak_rss = 0
        timed_out = False
        with stdout_path.open("w", encoding="utf-8") as stdout_file, stderr_path.open(
            "w", encoding="utf-8"
        ) as stderr_file:
            process = subprocess.Popen(
                command,
                text=True,
                stdout=stdout_file,
                stderr=stderr_file,
                start_new_session=True,
            )
            deadline = started + timeout_seconds
            while process.poll() is None:
                rss = process_rss_kib(process.pid)
                if rss is not None:
                    peak_rss = max(peak_rss, rss)
                if time.monotonic() >= deadline:
                    timed_out = True
                    os.killpg(process.pid, signal.SIGTERM)
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.wait()
                    break
                time.sleep(0.25)
            rss = process_rss_kib(process.pid)
            if rss is not None:
                peak_rss = max(peak_rss, rss)
        stdout = stdout_path.read_text(encoding="utf-8", errors="replace")
        stderr = stderr_path.read_text(encoding="utf-8", errors="replace")
        if timed_out:
            result.update(
                success=False,
                status="timeout",
                wall_seconds=round(time.monotonic() - started, 3),
                max_rss_kib=peak_rss or None,
                stdout=stdout[-2000:],
                stderr=stderr[-2000:],
            )
            return result
        result["wall_seconds"] = round(time.monotonic() - started, 3)
        result["max_rss_kib"] = peak_rss or None
        result["returncode"] = process.returncode
        payload = None
        for line in stdout.splitlines():
            if line.startswith(ASSESSMENT_PREFIX):
                payload = json.loads(line[len(ASSESSMENT_PREFIX) :])
        if process.returncode == 0 and payload and payload.get("success"):
            result.update(payload)
            result["success"] = True
            result["status"] = "passed"
        else:
            result.update(
                success=False,
                status="failed",
                stdout=stdout[-2000:],
                stderr=stderr[-2000:],
            )
        return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--example", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--timeout-seconds", type=int, default=180)
    args = parser.parse_args()

    rows = []
    for candidate in CANDIDATES:
        print(f"assessing {candidate} outputs/package", flush=True)
        row = assess(args.example, candidate, args.timeout_seconds, optimize=True)
        rows.append(row)
        print(json.dumps(row, sort_keys=True), flush=True)

    passed = [row for row in rows if row.get("success")]
    optimized_recommendation = max(
        passed, key=lambda row: row["outputs_per_package"], default=None
    )
    fallback = None
    if not optimized_recommendation or optimized_recommendation["outputs_per_package"] < 85:
        print("assessing 85 outputs/package without resource reuse", flush=True)
        fallback = assess(args.example, 85, args.timeout_seconds, optimize=False)
        print(json.dumps(fallback, sort_keys=True), flush=True)
    selected = fallback if fallback and fallback.get("success") else optimized_recommendation
    report = {
        "profile": "full",
        "matrix": [86, 85],
        "scope": "one representative first-stage package per candidate",
        "optimizer_enabled": True,
        "timeout_seconds_per_candidate": args.timeout_seconds,
        "selection_rule": "fewest packages that completes; retry all outputs without address-reuse optimization when optimized compilation times out",
        "selected_strategy": (
            {
                "outputs_per_package": selected["outputs_per_package"],
                "projected_total_packages": selected["projected_total_packages"],
                "resource_optimizer": selected["resource_optimizer"],
            }
            if selected
            else None
        ),
        "optimized_candidates": rows,
        "unoptimized_all_outputs_fallback": fallback,
    }
    output = pathlib.Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {output}")
    return 0 if selected else 1


if __name__ == "__main__":
    raise SystemExit(main())
