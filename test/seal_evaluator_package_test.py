#!/usr/bin/env python3
"""Run frontend regressions and independently parse a package with scale views."""

import csv
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile


def main():
    test, validator = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="hpu-evaluator-test-") as directory:
        package = Path(directory) / "package"
        subprocess.run([test, str(package)], check=True)
        subprocess.run([validator, str(package)], check=True)
        for path in package.rglob("*.json"):
            json.loads(path.read_text())
        descriptor = json.loads((package / "package.json").read_text())
        parameters = json.loads((package / descriptor["parameters"]).read_text())
        table_abi = parameters["ntt_table_abi"]
        assert table_abi["version"] == 2
        assert table_abi["coefficient_order"] == "natural"
        assert table_abi["pre_twist_order"] == "natural"
        assert table_abi["post_factor_order"] == "natural"
        assert table_abi["stage_twiddle_order"] == "reference_loader_batch_lane"
        assert table_abi["stage_twiddle_format"] == "hpu-twiddle-k8-v1"
        assert table_abi["twiddle_chains"] == 8
        assert table_abi["stage_parameter_lines"] == 2
        assert table_abi["normalization"] == "explicit_post_pmul"
        assert table_abi["twist_fused"] is False
        memory_abi = json.loads((package / descriptor["memory_abi"]).read_text())
        for field in ("stage_twiddle_format", "twiddle_chains", "stage_parameter_lines"):
            assert memory_abi[field] == table_abi[field]
        image = (package / descriptor["image"]).read_bytes()
        with (package / descriptor["memory_manifest"]).open() as manifest:
            allocations = list(csv.DictReader(manifest))
        stages = [row for row in allocations
                  if re.search(r"/(ntt|intt)/stage\d+$", row["allocation_id"])]
        assert stages
        for row in stages:
            assert int(row["payload_words"]) == 128
            assert int(row["line_count"]) == 2
            offset = int(row["line_offset"]) * 256
            payload = image[offset:offset + 512]
            assert len(payload) == 512
            if row["allocation_id"].endswith("/stage0"):
                assert struct.unpack("<128I", payload) == (1,) * 128
                assert hashlib.sha256(payload).hexdigest() == (
                    "d585e10d1e2240e9af79fc1cf8d11e11420b5306480b469b587e85630fcb0c9f"
                )
        factors = [row for row in allocations if row["allocation_id"].endswith(
            ("/pre_twist", "/post_untwist_scale"))]
        assert factors
        assert all(int(row["payload_words"]) == parameters["poly_modulus_degree"]
                   for row in factors)
        graph = json.loads((package / descriptor["operation_graph"]).read_text())
        assert graph["frontend"] == "seal_evaluator"
        assert graph["metadata_views"]
        assert graph["marked_outputs"]
        for view in graph["metadata_views"]:
            assert view["kind"] == "set_scale"
            assert isinstance(view["input"], str) and view["input"]
            assert isinstance(view["physical_output"], str) and view["physical_output"]
        report = json.loads((package / descriptor["oracle_report"]).read_text())
        assert report["optimization_equivalence_verified"] is True
        assert report["host_instruction_model"]["required"] is False
        assert report["host_instruction_model_verified"] == report["host_instruction_model"]["verified"]
        assert report["host_instruction_model"]["ntt_contract"] == "natural_coefficient_boundaries"
        assert report["host_instruction_model_verified"] is True
        assert report["host_instruction_model"]["hardware_contract_verified"] is False
        assert report["host_instruction_model"]["forward_boundary_conversions"] > 0
        assert report["host_instruction_model"]["inverse_boundary_conversions"] > 0
        assert report["compiler_resources"]["ir_source"] == "operator"
        assert report["compiler_resources"]["peak_regular_banks"] <= 5
        assert report["compiler_resources"]["peak_object_slots"] <= 8
        buffers = report["compiler_resources"]["logical_buffers"]
        assert any(buffer["layout"] == "canonical_ntt_physical" for buffer in buffers)
        assert any(buffer["layout"] == "unspecified" for buffer in buffers)


if __name__ == "__main__":
    main()
