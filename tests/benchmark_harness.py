#!/usr/bin/env python3

import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location("tribios_benchmark", REPOSITORY / "bench/benchmark.py")
benchmark = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(benchmark)


class BenchmarkHarnessTest(unittest.TestCase):
    def test_missing_measurements_cannot_pass(self):
        run_environment = {
            "platform": "Darwin-test",
            "macfuse_version": "5.3.3",
        }
        verdict = benchmark.evaluate({}, run_environment)

        self.assertEqual(set(benchmark.REQUIRED_GATE_NAMES), set(verdict))
        self.assertFalse(all(gate["pass"] for gate in verdict.values()))

    def test_toy_fixture_cannot_pass_scale_gate(self):
        results = {"base_state": {"base regular files": "104", "base bytes": "6292872"}}
        run_environment = {
            "platform": "Darwin-test",
            "macfuse_version": "5.3.3",
        }

        self.assertFalse(benchmark.evaluate(results, run_environment)["fixture_scale"]["pass"])

    def test_storage_gate_consumes_allocated_byte_fields(self):
        expected = 1024 * 1024
        results = {
            "base_state": {
                "base regular files": str(benchmark.MINIMUM_FIXTURE_FILES),
                "base bytes": str(benchmark.MINIMUM_FIXTURE_BYTES),
            },
            "storage": {
                "untouched_fraction_of_base": 0.001,
                "mutated_upper_physical_bytes": expected + 4096,
                "expected_copied_up_physical_bytes": expected,
            },
        }
        run_environment = {
            "platform": "Darwin-test",
            "macfuse_version": "5.3.3",
        }

        storage_gate = benchmark.evaluate(results, run_environment)["mutated_storage_overhead"]

        self.assertTrue(storage_gate["pass"])
        self.assertEqual(0.003906, storage_gate["value"])

    def test_cli_rejects_too_few_repetitions_before_running(self):
        completed = subprocess.run(
            [
                sys.executable,
                str(REPOSITORY / "bench/benchmark.py"),
                "--cli", "unused",
                "--project", "unused",
                "--scratch", "unused",
                "--build-directory", "unused",
                "--repetitions", "1",
            ],
            capture_output=True,
            text=True,
        )

        self.assertEqual(2, completed.returncode)
        self.assertIn("--repetitions must be at least 5", completed.stderr)

    def test_allocated_bytes_reports_backing_blocks(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "one-byte"
            path.write_bytes(b"x")

            self.assertGreaterEqual(benchmark.allocated_bytes(Path(directory)), 512)


if __name__ == "__main__":
    unittest.main()
