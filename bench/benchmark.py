#!/usr/bin/env python3
"""Benchmark the Tribios prototype against equivalent full directory copies.

THROWAWAY PROTOTYPE - see docs/prototype/README.md.

Every timed case reports raw samples, median and p95. Base-state capture time is
reported separately from Workspace creation time, and physical reclamation and
the storage a removed Workspace still holds are reported separately from logical
removal. The run also drives the correctness suite, because issue #1 decides the
verdict on correctness and performance together.
"""

import argparse
import concurrent.futures
import json
import os
import platform
import shutil
import statistics
import subprocess
import time
from pathlib import Path

GATES = {
    "workspace_create_speedup": 10.0,
    "logical_remove_speedup": 10.0,
    "untouched_storage_fraction": 0.01,
    "mutated_storage_overhead_fraction": 0.05,
    "runtime_ratio": 1.5,
}

# Every gate issue #1 decides on. A gate with no measurement behind it fails, so
# a run with a failed or skipped correctness suite, or with the mounted-path
# cases missing, can never report PASS.
REQUIRED_GATE_NAMES = [
    "correctness_suite",
    "untouched_storage_fraction",
    "mutated_storage_overhead",
] + [
    f"{gate}_concurrency_{concurrency}"
    for concurrency in (1, 8)
    for gate in ("create_speedup", "logical_remove_speedup", "git_status_ratio", "build_ratio")
]


def run(command, **kwargs):
    return subprocess.run(command, check=True, capture_output=True, text=True, **kwargs)


def milliseconds(action):
    started = time.perf_counter()
    action()
    return (time.perf_counter() - started) * 1000.0


def summarize(samples):
    ordered = sorted(samples)
    return {
        "samples_ms": [round(value, 3) for value in samples],
        "median_ms": round(statistics.median(ordered), 3),
        "p95_ms": round(ordered[min(len(ordered) - 1, int(round(0.95 * (len(ordered) - 1))))], 3),
    }


def directory_bytes(path: Path) -> int:
    total = 0
    for current, _directories, files in os.walk(path):
        for name in files:
            try:
                total += os.lstat(os.path.join(current, name)).st_size
            except OSError:
                pass
    return total


class Harness:
    def __init__(self, cli: Path, project: Path, scratch: Path):
        self.cli = str(cli)
        self.project = str(project)
        self.scratch = scratch
        self.results = {}

    def tribios(self, *arguments):
        return run([self.cli, "--project", self.project, *arguments]).stdout

    def mounted(self) -> bool:
        return "mount backend: mounted" in self.tribios("info")

    def workspace_path(self, name: str) -> Path:
        for line in self.tribios("info").splitlines():
            if line.startswith("mount: "):
                return Path(line.split(": ", 1)[1]) / name
        raise RuntimeError("no mount point reported")

    # --- lifecycle -------------------------------------------------------
    def create_workspace(self, name: str) -> float:
        return milliseconds(lambda: self.tribios("workspace", "create", name))

    def remove_workspace(self, name: str) -> float:
        return milliseconds(lambda: self.tribios("workspace", "remove", name))

    def full_copy(self, destination: Path) -> float:
        # The baseline reproduces the same Workspace contents, including ignored
        # and untracked data, and excludes only Tribios' own storage.
        def copy():
            source = subprocess.Popen(
                ["tar", "-C", self.project, "--exclude=./.tribios", "-cf", "-", "."],
                stdout=subprocess.PIPE)
            extract = subprocess.Popen(["tar", "-C", str(destination), "-xf", "-"],
                                       stdin=source.stdout)
            source.stdout.close()
            extract.communicate()
            source.wait()
        return milliseconds(copy)

    def full_delete(self, destination: Path) -> float:
        return milliseconds(lambda: shutil.rmtree(destination, ignore_errors=True))

    # --- cases -----------------------------------------------------------
    def case_lifecycle(self, repetitions: int, concurrency: int):
        label = f"concurrency_{concurrency}"
        create_samples, remove_samples = [], []
        baseline_copy, baseline_delete = [], []

        for repetition in range(repetitions):
            names = [f"bench-{concurrency}-{repetition}-{index}" for index in range(concurrency)]
            with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as pool:
                create_samples.extend(pool.map(self.create_workspace, names))
            with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as pool:
                remove_samples.extend(pool.map(self.remove_workspace, names))
            self.tribios("workspace", "wait-reclaim")

            copies = [self.scratch / f"copy-{concurrency}-{repetition}-{index}"
                      for index in range(concurrency)]
            for copy in copies:
                copy.mkdir(parents=True, exist_ok=True)
            with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as pool:
                baseline_copy.extend(pool.map(self.full_copy, copies))
            with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as pool:
                baseline_delete.extend(pool.map(self.full_delete, copies))

        self.results[f"workspace_create_{label}"] = summarize(create_samples)
        self.results[f"logical_remove_{label}"] = summarize(remove_samples)
        self.results[f"full_copy_create_{label}"] = summarize(baseline_copy)
        self.results[f"full_copy_delete_{label}"] = summarize(baseline_delete)

    def case_correctness(self, build_directory: Path):
        # A skipped mounted-path suite is not a pass: it means the evidence the
        # verdict needs was never produced.
        completed = subprocess.run(["ctest", "--test-dir", str(build_directory),
                                    "--output-on-failure"], capture_output=True, text=True)
        output = completed.stdout + completed.stderr
        self.results["correctness"] = {
            "exit_code": completed.returncode,
            "failed_tests": output.count("***Failed"),
            "skipped_tests": output.count("***Skipped"),
        }

    def case_reclamation(self, repetitions: int):
        # Physical reclamation, and the storage a logically removed Workspace
        # still holds until it finishes, are measured but never folded into
        # logical removal.
        reclaim_samples, transient_samples = [], []
        for repetition in range(repetitions):
            name = f"reclaim-probe-{repetition}"
            self.tribios("workspace", "create", name)
            self.tribios("fs", "write", name, "src/module0000/file000000.cpp", "x" * 4096, "0")
            # Reading the upper tree before removal, not after, keeps the
            # transient figure off the race with the reclaiming thread.
            transient_samples.append(int(self.tribios("upper-bytes", name).strip()))
            self.tribios("workspace", "remove", name)
            self.tribios("workspace", "wait-reclaim")
            for line in self.tribios("workspace", "list").splitlines():
                fields = line.split("\t")
                if fields[0] == name:
                    reclaim_samples.append(int(fields[5]) / 1000.0)
        self.results["physical_reclaim"] = summarize(reclaim_samples)
        # Storage a Workspace still holds when logical removal returns, which
        # reclamation frees afterwards.
        self.results["transient_storage"] = {
            "samples_bytes": transient_samples,
            "median_bytes": int(statistics.median(transient_samples)),
        }

    def case_storage(self):
        self.tribios("workspace", "create", "storage-untouched")
        untouched = int(self.tribios("upper-bytes", "storage-untouched").strip())
        base_bytes = directory_bytes(Path(self.project) / ".tribios" / "base")

        self.tribios("workspace", "create", "storage-mutated")
        payload = "m" * 65536
        mutated_files = [f"src/module0000/file{index:06d}.cpp" for index in range(16)]
        for path in mutated_files:
            self.tribios("fs", "write", "storage-mutated", path, payload, "0")
        mutated = int(self.tribios("upper-bytes", "storage-mutated").strip())

        self.results["storage"] = {
            "base_physical_bytes": base_bytes,
            "untouched_upper_bytes": untouched,
            "untouched_fraction_of_base": round(untouched / base_bytes, 6) if base_bytes else 0,
            "mutated_upper_bytes": mutated,
            "expected_copied_up_bytes": len(payload) * len(mutated_files),
        }

    def case_runtime(self, repetitions: int, build_repetitions: int, concurrency: int):
        if not self.mounted():
            self.results["runtime"] = {
                "skipped": "this build has no macFUSE backend, so mounted-path runtime "
                           "measurements cannot run"
            }
            return

        def git_status(path: Path) -> float:
            return milliseconds(lambda: run(["git", "-C", str(path), "status", "--porcelain"]))

        def build(path: Path) -> float:
            # Every sample is a build from scratch, so repeated samples measure
            # the same work rather than an already-built tree.
            build_directory = path / "build"
            shutil.rmtree(build_directory, ignore_errors=True)

            def compile_project():
                run(["cmake", "-S", str(path), "-B", str(build_directory), "-G", "Ninja"])
                run(["ninja", "-C", str(build_directory)])
            return milliseconds(compile_project)

        # Names are unique per run: a removed Workspace keeps its branch.
        names = [f"runtime-{concurrency}-{index}" for index in range(concurrency)]
        for name in names:
            self.tribios("workspace", "create", name)
        paths = [self.workspace_path(name) for name in names]

        baselines = []
        for index in range(concurrency):
            destination = self.scratch / f"runtime-baseline-{concurrency}-{index}"
            destination.mkdir(parents=True, exist_ok=True)
            self.full_copy(destination)
            baselines.append(destination)

        status_workspace, status_baseline = [], []
        for _ in range(repetitions):
            with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as pool:
                status_workspace.extend(pool.map(git_status, paths))
            with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as pool:
                status_baseline.extend(pool.map(git_status, baselines))

        build_workspace, build_baseline = [], []
        for _ in range(build_repetitions):
            with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as pool:
                build_workspace.extend(pool.map(build, paths))
            with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as pool:
                build_baseline.extend(pool.map(build, baselines))

        self.results[f"git_status_workspace_concurrency_{concurrency}"] = summarize(status_workspace)
        self.results[f"git_status_full_copy_concurrency_{concurrency}"] = summarize(status_baseline)
        self.results[f"build_workspace_concurrency_{concurrency}"] = summarize(build_workspace)
        self.results[f"build_full_copy_concurrency_{concurrency}"] = summarize(build_baseline)

        for name in names:
            self.tribios("workspace", "remove", name)
        self.tribios("workspace", "wait-reclaim")


def evaluate(results):
    verdict = {}
    for concurrency in (1, 8):
        label = f"concurrency_{concurrency}"
        if f"workspace_create_{label}" not in results:
            continue
        create = results[f"full_copy_create_{label}"]["median_ms"] / \
            max(results[f"workspace_create_{label}"]["median_ms"], 1e-6)
        remove = results[f"full_copy_delete_{label}"]["median_ms"] / \
            max(results[f"logical_remove_{label}"]["median_ms"], 1e-6)
        verdict[f"create_speedup_{label}"] = {
            "value": round(create, 2), "gate": GATES["workspace_create_speedup"],
            "pass": create >= GATES["workspace_create_speedup"]}
        verdict[f"logical_remove_speedup_{label}"] = {
            "value": round(remove, 2), "gate": GATES["logical_remove_speedup"],
            "pass": remove >= GATES["logical_remove_speedup"]}
        for kind in ("git_status", "build"):
            workspace_key = f"{kind}_workspace_{label}"
            baseline_key = f"{kind}_full_copy_{label}"
            if workspace_key in results and baseline_key in results:
                ratio = results[workspace_key]["median_ms"] / \
                    max(results[baseline_key]["median_ms"], 1e-6)
                verdict[f"{kind}_ratio_{label}"] = {
                    "value": round(ratio, 2), "gate": GATES["runtime_ratio"],
                    "pass": ratio <= GATES["runtime_ratio"]}

    correctness = results.get("correctness")
    if correctness:
        verdict["correctness_suite"] = {
            "value": correctness,
            "gate": "zero failed and zero skipped tests",
            "pass": correctness["exit_code"] == 0 and correctness["skipped_tests"] == 0}

    storage = results.get("storage")
    if storage:
        fraction = storage["untouched_fraction_of_base"]
        verdict["untouched_storage_fraction"] = {
            "value": fraction, "gate": GATES["untouched_storage_fraction"],
            "pass": fraction <= GATES["untouched_storage_fraction"]}
        expected = storage["expected_copied_up_bytes"]
        overhead = (storage["mutated_upper_bytes"] - expected) / max(expected, 1)
        verdict["mutated_storage_overhead"] = {
            "value": round(overhead, 6), "gate": GATES["mutated_storage_overhead_fraction"],
            "pass": 0 <= overhead <= GATES["mutated_storage_overhead_fraction"]}

    for name in REQUIRED_GATE_NAMES:
        verdict.setdefault(name, {"value": None, "gate": "measured", "pass": False,
                                  "note": "not measured in this run"})
    return verdict


def command_output(command) -> str:
    try:
        return run(command).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def first_line(command) -> str:
    lines = command_output(command).splitlines()
    return lines[0] if lines else "unavailable"


def fuse_version() -> str:
    for package in ("osxfuse", "fuse", "fuse3"):
        version = command_output(["pkg-config", "--modversion", package])
        if version:
            return f"{package} {version}"
    return "unavailable"


def backing_filesystem(path: Path) -> str:
    rows = command_output(["df", "-P", str(path)]).splitlines()
    if len(rows) < 2:
        return "unavailable"
    device = rows[-1].split()[0]
    for line in command_output(["mount"]).splitlines():
        if line.startswith(device + " "):
            return line
    return device


def environment(project: Path, base_state):
    return {
        "platform": platform.platform(),
        "machine": platform.machine(),
        "cpu_count": os.cpu_count(),
        "backing_filesystem": backing_filesystem(project),
        "fuse": fuse_version(),
        "compiler": first_line(["c++", "--version"]),
        "python": platform.python_version(),
        "git": first_line(["git", "--version"]),
        "cmake": first_line(["cmake", "--version"]),
        "ninja": first_line(["ninja", "--version"]),
        "commit": command_output(
            ["git", "-C", str(Path(__file__).resolve().parent.parent), "rev-parse", "HEAD"]),
        "fixture_files": base_state.get("base entries", "unavailable"),
        "fixture_bytes": base_state.get("base bytes", "unavailable"),
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", required=True, help="path to the tribios CLI")
    parser.add_argument("--project", required=True, help="a configured Project")
    parser.add_argument("--scratch", required=True, help="scratch directory for baselines")
    parser.add_argument("--build-directory", required=True,
                        help="build directory the correctness suite runs in")
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--build-repetitions", type=int, default=3,
                        help="build samples per Workspace, each from a cleared build tree")
    parser.add_argument("--output", default="bench/results/latest.json")
    arguments = parser.parse_args()

    scratch = Path(arguments.scratch)
    scratch.mkdir(parents=True, exist_ok=True)
    harness = Harness(Path(arguments.cli), Path(arguments.project).resolve(), scratch)

    base_state = {
        line.split(": ", 1)[0]: line.split(": ", 1)[1]
        for line in harness.tribios("info").splitlines() if ": " in line
    }
    harness.results["base_state"] = base_state
    harness.case_correctness(Path(arguments.build_directory))
    harness.case_lifecycle(arguments.repetitions, concurrency=1)
    harness.case_lifecycle(arguments.repetitions, concurrency=8)
    harness.case_reclamation(arguments.repetitions)
    harness.case_storage()
    harness.case_runtime(max(3, arguments.repetitions), arguments.build_repetitions,
                         concurrency=1)
    harness.case_runtime(max(3, arguments.repetitions), arguments.build_repetitions,
                         concurrency=8)

    report = {
        "environment": environment(Path(arguments.project).resolve(), base_state),
        "results": harness.results,
        "verdict": evaluate(harness.results),
    }
    report["passed"] = all(entry["pass"] for entry in report["verdict"].values())

    output = Path(arguments.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report["verdict"], indent=2))
    print(f"verdict: {'PASS' if report['passed'] else 'FAIL'} (raw results in {output})")
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
