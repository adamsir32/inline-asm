#!/usr/bin/env python3
"""Failure-path regression for the SenderDB differential test driver."""

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("apsi_trident_senderdb_differential.py")


class DifferentialDriverFailureTest(unittest.TestCase):
    def run_driver(self, raw_names=(), full=()):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cases = root / "cases"
            cases.mkdir()
            for name in raw_names:
                (cases / f"{name}.raw").write_text("fixture\n", encoding="utf-8")
            output = root / "output"
            command = [sys.executable, str(SCRIPT), "--native", str(root / "missing-native"),
                       "--params", str(root / "missing-params"),
                       "--example", str(root / "missing-example"),
                       "--case-dir", str(cases), "--output", str(output)]
            if full:
                command += ["--full", *full]
            completed = subprocess.run(command, text=True, capture_output=True, check=False)
            report = json.loads((output / "summary.json").read_text(encoding="utf-8"))
            return completed, report

    def test_empty_directory_fails(self):
        result, report = self.run_driver()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(report["input_case_count"], 0)
        self.assertIn("no *.raw cases", report["configuration_errors"][0])

    def test_missing_full_case_fails_even_when_another_case_runs(self):
        result, report = self.run_driver(["present"], ["absent"])
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(report["input_case_count"], report["run_case_count"])
        self.assertEqual(report["full_hpu_verified_count"], 0)
        self.assertIn("absent", report["configuration_errors"][0])

    def test_case_exception_fails_and_is_recorded(self):
        result, report = self.run_driver(["broken"])
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(report["cases"][0]["status"], "FAIL")
        self.assertIn("error", report["cases"][0])


if __name__ == "__main__":
    unittest.main()
