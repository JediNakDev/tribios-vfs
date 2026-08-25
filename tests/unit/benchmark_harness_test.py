#!/usr/bin/env python3

import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace


REPOSITORY = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "tribios_benchmark", REPOSITORY / "bench/benchmark.py"
)
benchmark = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(benchmark)


class BenchmarkHarnessTest(unittest.TestCase):
    def test_checkpoint_is_never_final_verdict_eligible(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "results.json"
            arguments = SimpleNamespace(
                output=str(output),
                project=directory,
                repetitions=5,
                build_repetitions=3,
                smoke=False,
            )
            harness = SimpleNamespace(
                attempt_ids=["attempt-one"],
                results={"correctness": {"exit_code": 0}},
            )

            benchmark.checkpoint_report(arguments, harness)

            checkpoint = json.loads(output.read_text())
            self.assertEqual("running", checkpoint["state"])
            self.assertFalse(checkpoint["final_verdict_eligible"])
            self.assertFalse(Path(str(output) + ".tmp").exists())

    def test_phase_is_complete_only_when_every_result_is_present(self):
        keys = benchmark.PHASE_RESULT_KEYS["lifecycle concurrency 1"]
        results = {key: {} for key in keys}

        self.assertTrue(benchmark.phase_is_complete(results, "lifecycle concurrency 1"))
        results.pop(keys[-1])
        self.assertFalse(benchmark.phase_is_complete(results, "lifecycle concurrency 1"))

    def test_checkpoint_resume_rejects_different_arguments(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "results.json"
            arguments = SimpleNamespace(
                output=str(output),
                project=directory,
                repetitions=5,
                build_repetitions=3,
                smoke=False,
            )
            harness = SimpleNamespace(attempt_ids=["attempt-one"], results={})
            benchmark.checkpoint_report(arguments, harness)
            changed = benchmark.benchmark_configuration(arguments) | {"smoke": True}

            with self.assertRaisesRegex(ValueError, "resume arguments do not match"):
                benchmark.load_checkpoint(output, changed)

    def test_checkpoint_resume_restores_completed_results(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "results.json"
            arguments = SimpleNamespace(
                output=str(output),
                project=directory,
                repetitions=5,
                build_repetitions=3,
                smoke=False,
            )
            expected_results = {"correctness": {"exit_code": 0}}
            harness = SimpleNamespace(
                attempt_ids=["attempt-one"],
                results=expected_results,
            )
            benchmark.checkpoint_report(arguments, harness)

            results, attempt_ids = benchmark.load_checkpoint(
                output, benchmark.benchmark_configuration(arguments))

            self.assertEqual(expected_results, results)
            self.assertEqual(["attempt-one"], attempt_ids)

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
                "mutated_total_physical_bytes": expected + 4096,
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

    def test_smoke_mode_accepts_one_repetition(self):
        completed = subprocess.run(
            [
                sys.executable,
                str(REPOSITORY / "bench/benchmark.py"),
                "--cli", "unused",
                "--project", "unused",
                "--scratch", "unused",
                "--build-directory", "unused",
                "--repetitions", "1",
                "--build-repetitions", "1",
                "--smoke",
            ],
            capture_output=True,
            text=True,
        )

        self.assertNotEqual(2, completed.returncode)
        self.assertNotIn("must be at least", completed.stderr)

    def test_allocated_bytes_reports_backing_blocks(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "one-byte"
            path.write_bytes(b"x")

            self.assertGreaterEqual(benchmark.allocated_bytes(Path(directory)), 512)

    def test_space_requirement_excludes_tribios_storage(self):
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            (project / "source").write_bytes(b"x")
            (project / ".tribios").mkdir()
            (project / ".tribios" / "base").write_bytes(b"x" * 8192)

            requirement = benchmark.benchmark_space_requirement(project)

            self.assertEqual(
                benchmark.allocated_bytes(project, excluded_top_level={".tribios"}),
                requirement["full_copy_physical_bytes"],
            )
            self.assertEqual(
                requirement["full_copy_physical_bytes"] * benchmark.CONCURRENT_WORKSPACES
                + benchmark.FREE_SPACE_SAFETY_BYTES,
                requirement["required_free_bytes"],
            )

    def test_metadata_store_bytes_includes_database_sidecars(self):
        with tempfile.TemporaryDirectory() as directory:
            project = Path(directory)
            tribios = project / ".tribios"
            tribios.mkdir()
            (tribios / "meta.db").write_bytes(b"x")
            (tribios / "meta.db-wal").write_bytes(b"x")
            (tribios / "meta.db-shm").write_bytes(b"x")

            expected = sum(
                benchmark.allocated_file_bytes(path)
                for path in tribios.iterdir()
            )
            self.assertEqual(expected, benchmark.metadata_store_bytes(project))


if __name__ == "__main__":
    unittest.main()
