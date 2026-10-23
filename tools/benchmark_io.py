#!/usr/bin/env python3
"""Generate validated three-scheme packages and summarize actual target DMA IO."""

import argparse
import csv
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

# Reuse managed publication without creating bytecode beside source files.
sys.dont_write_bytecode = True
from generate_application_package import generate


SCHEMES = ("ckks", "bfv", "bgv")
SCHEMA = "hpu-io-benchmark"
METRICS = (
    "dma_bytes", "dload_bytes", "dstore_bytes", "dma_count",
    "original_dma_bytes", "original_dma_count", "required_output_write_bytes",
    "all_checkpoint_dma_bytes", "all_checkpoint_dma_count", "output_policy_saved_dma_bytes",
    "spill_write_bytes", "reload_read_bytes", "exact_copy_bytes",
    "repeated_range_read_bytes", "removed_checkpoint_min_write_bytes",
    "initial_image_bytes", "spills", "reloads", "peak_regular_lines",
    "peak_small_lines", "peak_object_slots", "allocated_ddr_lines",
)


def read_json(path):
    return json.loads(path.read_text())


def read_csv(path):
    with path.open(newline="") as stream:
        return list(csv.DictReader(stream))


def observable_outputs(graph, golden):
    # Addresses and file names can change under valid optimization. The SEAL
    # oracle payload remains fixed for an exact seeded fixture comparison.
    marks = graph.get("marked_outputs")
    if not marks:
        raise ValueError("benchmark requires explicit frontend output metadata")
    limbs = [{key: row[key] for key in (
        "object_id", "component", "modulus_id", "modulus", "domain", "payload_words",
        "payload_fnv1a64")}
        for row in golden]
    return {
        "marked_outputs": sorted(marks, key=lambda value: json.dumps(value, sort_keys=True)),
        "limbs": sorted(limbs, key=lambda value: json.dumps(value, sort_keys=True)),
    }


def collect(package, fixture, optimized):
    descriptor = read_json(package / "package.json")
    parameters = read_json(package / descriptor["parameters"])
    graph = read_json(package / descriptor["operation_graph"])
    oracle = read_json(package / descriptor["oracle_report"])
    report = oracle["compiler_resources"]
    dma = read_csv(package / descriptor["dma_relocation_manifest"])
    memory = {row["allocation_id"]: row for row in
              read_csv(package / descriptor["memory_manifest"])}
    golden = read_csv(package / descriptor["golden_manifest"])
    line_bytes = descriptor["line_bytes"]
    totals = {"dload_bytes": 0, "dstore_bytes": 0}
    by_kind = {}
    for row in dma:
        direction = row["direction"]
        if direction not in ("dload", "dstore"):
            raise ValueError(f"unexpected DMA direction: {direction}")
        byte_count = int(row["line_count"]) * line_bytes
        key = direction + "_bytes"
        totals[key] += byte_count
        kind = memory[row["allocation_id"]]["kind"]
        counters = by_kind.setdefault(kind, {"reads": 0, "writes": 0,
                                             "read_bytes": 0, "write_bytes": 0})
        access = "read" if direction == "dload" else "write"
        counters[access + "s"] += 1
        counters[access + "_bytes"] += byte_count
    totals["dma_bytes"] = totals["dload_bytes"] + totals["dstore_bytes"]
    totals["dma_count"] = len(dma)
    for key, value in totals.items():
        if report[key] != value:
            raise ValueError(f"{package}: {key} disagrees with final DMA manifest")
    if parameters["poly_modulus_degree"] != fixture["degree"]:
        raise ValueError("generated package has unexpected polynomial degree")
    data_moduli = parameters["levels"][0]["moduli"]
    special_moduli = [q for q in parameters["key_moduli"] if q not in data_moduli]
    if len(data_moduli) != fixture["data_moduli"] or len(special_moduli) != 1:
        raise ValueError("generated package must have the requested data Q and one special P")
    required_metrics = {key: report[key] for key in METRICS}
    required_metrics["instruction_count"] = len(
        (package / descriptor["program_inst32"]).read_text().splitlines())
    return {
        "scheme": descriptor["scheme"], "case_name": descriptor["case_name"],
        "package": str(package), "fixture": fixture, "optimized": optimized,
        "N": parameters["poly_modulus_degree"], "Q": len(data_moduli),
        "P": len(special_moduli), "parameters": parameters,
        "required_output_set": observable_outputs(graph, golden),
        "provenance": read_json(package / "provenance/build.json"),
        "selected_schedule": report["selected_schedule"],
        "metrics": required_metrics,
        "candidate_schedules": report["candidate_schedules"],
        "io_by_allocation": report["io_by_allocation"],
        "io_by_value": report["io_by_value"],
        "io_by_kind": by_kind,
        "snapshot_backing_buffers": [buffer for buffer in report["logical_buffers"]
            if buffer["logical_id"].startswith("$compiler/ssa_copy/")],
        "verification": {key: oracle[key] for key in
                         ("host_instruction_model_verified", "optimization_equivalence_verified")},
    }


def compare(current, baseline):
    if baseline.get("schema") != SCHEMA or baseline.get("schema_version") != 1:
        raise ValueError("baseline must be a benchmark_io.py schema v1 summary")
    old_cases = {case["scheme"]: case for case in baseline["cases"]}
    if len(old_cases) != len(baseline["cases"]) or set(old_cases) != set(SCHEMES):
        raise ValueError("baseline must contain exactly one CKKS, BFV, and BGV case")
    result = []
    for case in current:
        old = old_cases[case["scheme"]]
        for key in ("parameters", "fixture", "required_output_set"):
            if old[key] != case[key]:
                raise ValueError(f"{case['scheme']}: baseline {key} differs; IO comparison refused")
        previous, actual = old["metrics"]["dma_bytes"], case["metrics"]["dma_bytes"]
        by_kind = {}
        for kind in sorted(set(old["io_by_kind"]) | set(case["io_by_kind"])):
            prior = old["io_by_kind"].get(kind, {})
            current_kind = case["io_by_kind"].get(kind, {})
            entry = {}
            for direction in ("read", "write"):
                before = prior.get(direction + "_bytes", 0)
                after = current_kind.get(direction + "_bytes", 0)
                entry["baseline_" + direction + "_bytes"] = before
                entry[direction + "_bytes"] = after
                entry["saved_" + direction + "_bytes"] = before - after
            entry["saved_dma_bytes"] = entry["saved_read_bytes"] + entry["saved_write_bytes"]
            by_kind[kind] = entry
        if sum(entry["saved_dma_bytes"] for entry in by_kind.values()) != previous - actual:
            raise ValueError(f"{case['scheme']}: data-class IO attribution disagrees with DMA totals")
        classified = {}
        for metric in ("exact_copy_bytes", "spill_write_bytes", "reload_read_bytes"):
            before, after = old["metrics"][metric], case["metrics"][metric]
            classified[metric] = {"baseline_bytes": before, "bytes": after,
                                  "saved_bytes": before - after}
        result.append({
            "scheme": case["scheme"], "baseline_dma_bytes": previous,
            "dma_bytes": actual, "saved_dma_bytes": previous - actual,
            "saved_percent": (100.0 * (previous - actual) / previous) if previous else None,
            "io_by_kind": by_kind,
            "classified_io": classified,
        })
    return result


def write_summary(path, summary):
    # A failed generation/comparison leaves the previous summary intact.
    descriptor, name = tempfile.mkstemp(prefix=".io-summary-", suffix=".json", dir=path.parent)
    try:
        with os.fdopen(descriptor, "w") as stream:
            json.dump(summary, stream, ensure_ascii=False, indent=2, sort_keys=True)
            stream.write("\n")
        os.replace(name, path)
    finally:
        if os.path.exists(name):
            os.unlink(name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--degree", type=int, default=8192)
    parser.add_argument("--data-moduli", type=int, default=4)
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--no-opt", action="store_true", help="generate the same-output original-order baseline")
    parser.add_argument("--baseline-json", type=Path, help="compare only identical parameters, fixture, and required outputs")
    args = parser.parse_args()
    if args.degree < 128 or args.degree & (args.degree - 1):
        parser.error("--degree must be a power of two >= 128")
    if not 2 <= args.data_moduli <= 16 or not 0 <= args.seed < 2 ** 64:
        parser.error("--data-moduli must be in [2, 16] and --seed must be uint64")
    baseline = read_json(args.baseline_json) if args.baseline_json else None
    build = args.build_dir.absolute()
    validator = build / "hpu_validate_package"
    binaries = [build / f"hpu_{scheme}_evaluator_example" for scheme in SCHEMES]
    for binary in [validator, *binaries]:
        if not binary.is_file() or not os.access(binary, os.X_OK):
            parser.error(f"missing executable {binary}; build with HPU_ENABLE_SEAL_INTEGRATION=ON first")
    root = args.output_root.absolute()
    if root.is_symlink():
        parser.error("--output-root must not be a symlink")
    root.mkdir(parents=True, exist_ok=True)
    fixture = {"degree": args.degree, "data_moduli": args.data_moduli, "seed": args.seed}
    cases = []
    for scheme, example in zip(SCHEMES, binaries):
        package = root / f"{scheme}_evaluator_application_n{args.degree}_q{args.data_moduli}"
        example_args = ["--degree", str(args.degree), "--data-moduli", str(args.data_moduli),
                        "--seed", str(args.seed)]
        if args.no_opt:
            example_args.append("--no-opt")
        generate(str(example), str(validator), package, example_args)
        case = collect(package, fixture, not args.no_opt)
        if case["scheme"] != scheme or not all(case["verification"].values()):
            raise ValueError(f"{scheme}: package did not pass required software verification")
        cases.append(case)
    summary = {"schema": SCHEMA, "schema_version": 1,
               "generated_at_utc": datetime.now(timezone.utc).isoformat(), "cases": cases,
               "measurement": "final encoded DMA transfer bytes; excludes LLC hits and hardware timing"}
    if baseline is not None:
        summary["comparison"] = compare(cases, baseline)
        summary["baseline_json"] = str(args.baseline_json.absolute())
    output = root / "io_summary.json"
    write_summary(output, summary)
    print("\nScheme  DMA bytes  Read bytes  Write bytes  Spill/Reload  Schedule")
    for case in cases:
        metric = case["metrics"]
        print(f"{case['scheme']:6}  {metric['dma_bytes']:9}  {metric['dload_bytes']:10}  "
              f"{metric['dstore_bytes']:11}  {metric['spills']}/{metric['reloads']:<11}  "
              f"{case['selected_schedule']}")
    for item in summary.get("comparison", []):
        print(f"{item['scheme']}: saved {item['saved_dma_bytes']} DMA bytes vs baseline")
    print(f"Summary: {output}")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as error:
        print(f"IO benchmark failed: {error}", file=sys.stderr)
        sys.exit(1)
