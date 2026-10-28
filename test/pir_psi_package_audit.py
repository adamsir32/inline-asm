#!/usr/bin/env python3
"""Read-only manifest of existing PIR/PSI package v1 artifacts."""

import argparse
import csv
import json
from pathlib import Path


REQUIRED = ("parameters", "operation_graph", "program_source", "program_header",
            "program_asm", "program_inst32", "dma_relocation_manifest", "image",
            "line_map", "memory_manifest", "memory_abi", "memory_config",
            "golden_manifest", "oracle_report")


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def rows(path):
    with path.open(encoding="utf-8", newline="") as stream:
        return list(csv.DictReader(stream))


def audit(root):
    package = json.loads((root / "package.json").read_text(encoding="utf-8"))
    require(package["schema"] == "hpu-application-package" and package["schema_version"] == 1,
            f"{root}: unsupported schema")
    require((package["word_bits"], package["line_words"], package["line_bytes"],
             package["byte_order"]) == (32, 64, 256, "little-endian"), f"{root}: word ABI")
    for key in REQUIRED:
        relative = Path(package[key])
        require(not relative.is_absolute() and ".." not in relative.parts,
                f"{root}: unsafe {key} path")
        require((root / relative).is_file(), f"{root}: missing {key} ({relative})")
    config = json.loads((root / package["memory_config"]).read_text(encoding="utf-8"))
    provenance = json.loads((root / "provenance/build.json").read_text(encoding="utf-8"))
    used, capacity = config["used_lines"], config["capacity_lines"]
    require(0 < used <= capacity and (root / package["image"]).stat().st_size == used * 256,
            f"{root}: image size/config mismatch")
    graph = json.loads((root / package["operation_graph"]).read_text(encoding="utf-8"))
    allocations = rows(root / package["line_map"])
    dma = rows(root / package["dma_relocation_manifest"])
    golden = rows(root / package["golden_manifest"])
    words = (root / package["program_inst32"]).read_text(encoding="ascii").split()
    require(len(dma) == sum(int(word, 2) & 0x7f == 0x2b for word in words),
            f"{root}: DMA/instruction count mismatch")
    require(all((root / row["path"]).is_file() for row in golden), f"{root}: missing golden")
    require(bool(graph["marked_outputs"] and golden), f"{root}: no marked outputs")
    output_ids = sorted({row["object_id"] for row in golden})
    require(len(output_ids) == len(graph["marked_outputs"]), f"{root}: output map mismatch")
    by_output = {row["output"]: row["golden_object_id"] for row in graph["operations"]
                 if row.get("exported")}
    marked_order = [by_output[row["physical_output"]] for row in graph["marked_outputs"]]
    golden_order = list(dict.fromkeys(row["object_id"] for row in golden))
    require(marked_order == golden_order, f"{root}: marked/golden output order mismatch")
    inputs = sorted((row["allocation_id"], row["kind"]) for row in allocations
                    if row["read_only"] == "true" and row["kind"] in
                    ("ciphertext", "plaintext", "evaluation_key", "twiddle", "constant"))
    kinds = {kind: sum(row_kind == kind for _, row_kind in inputs)
             for kind in sorted({kind for _, kind in inputs})}
    return {"package": root.name, "case_name": package["case_name"],
            "partition": int(root.name.rsplit("_", 1)[1]) if root.name.startswith("hpu_query_partition_") else "",
            "producer_commit": provenance["commit"], "producer_worktree": provenance["worktree_state"],
            "input_objects": len(inputs), "input_kinds": json.dumps(kinds, sort_keys=True),
            "input_manifest": package["line_map"] + " (read_only=true)",
            "output_objects": len(output_ids), "output_ids": ";".join(output_ids),
            "golden_limb_files": len(golden), "golden_manifest": package["golden_manifest"],
            "golden_spans": ";".join(
                f"{row['object_id']}:c{row['component']}:m{row['modulus_id']}@"
                f"{row['line_offset']}+{row['line_count']}" for row in golden),
            "instructions": len(words), "dma": len(dma), "used_lines": used,
            "capacity_lines": capacity, "image_bytes": used * 256,
            "window_bytes": capacity * 256,
            "oracle_status": json.loads((root / package["oracle_report"]).read_text())["overall_status"]}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pir", type=Path, required=True)
    parser.add_argument("--psi", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    pir = sorted(args.pir.glob("*/package.json"))
    psi = sorted(args.psi.glob("*/package.json"),
                 key=lambda p: int(p.parent.name.rsplit("_", 1)[1]))
    require(len(pir) == 2 and len(psi) == 17, "expected two PIR and 17 PSI packages")
    records = [audit(path.parent) for path in pir + psi]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(records[0]), lineterminator="\n")
        writer.writeheader()
        writer.writerows(records)
    print(f"PASS: {len(pir)} PIR and {len(psi)} PSI packages; {args.output}")


if __name__ == "__main__":
    main()
