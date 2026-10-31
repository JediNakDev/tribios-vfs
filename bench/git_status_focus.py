#!/usr/bin/env python3
"""Focused Git status measurement for issue #32.

Times three Git commands in mounted Workspaces and in full-copy baselines:
default `git status`, `git status` without the untracked scan, and the
untracked scan alone. The full benchmark only times the first one, which hides
whether the slowdown comes from refreshing tracked files or from enumerating
directories.

Callback counts come from the daemon's FUSE debug log. Start the daemon by hand
with `tribios_daemon --project <path> --debug 2> fuse-debug.log`, run this
script with `--repetitions 1 --concurrency 1`, then count operations with
`grep -o 'opcode: [A-Z_]*' fuse-debug.log | sort | uniq -c`.
"""

import argparse
import concurrent.futures
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from benchmark import Harness, milliseconds, progress, run, summarize, write_json_atomically

GIT_STATUS_RATIO_GATE = 1.5

COMMANDS = {
    "default_status": ["status", "--porcelain"],
    "no_untracked_status": ["status", "--porcelain", "--untracked-files=no"],
    "untracked_scan": ["ls-files", "--others", "--exclude-standard", "--directory"],
}


def time_git_command(path: Path, arguments) -> float:
    return milliseconds(lambda: run(["git", "-C", str(path), *arguments]))


def measure_concurrency(harness: Harness, repetitions: int, concurrency: int):
    names, workspaces, baselines = harness.prepare_runtime_paths(concurrency)
    results = {}
    try:
        for label, paths in (("workspace", workspaces), ("full_copy", baselines)):
            # One untimed pass fills the kernel and daemon caches, so every
            # sample measures a warm tree.
            for path in paths:
                time_git_command(path, COMMANDS["default_status"])
            for command, arguments in COMMANDS.items():
                samples = []
                for repetition in range(repetitions):
                    progress(f"{command} {label} concurrency {concurrency}: "
                             f"{repetition + 1}/{repetitions}")
                    with concurrent.futures.ThreadPoolExecutor(max_workers=concurrency) as pool:
                        samples.extend(pool.map(
                            lambda path: time_git_command(path, arguments), paths))
                results[f"{command}_{label}"] = summarize(samples)
    finally:
        harness.remove_runtime_paths(names, baselines)

    for command in COMMANDS:
        workspace = results[f"{command}_workspace"]["median_ms"]
        baseline = results[f"{command}_full_copy"]["median_ms"]
        results[f"{command}_ratio"] = round(workspace / baseline, 3) if baseline else None
    return results


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", required=True, help="path to the tribios CLI")
    parser.add_argument("--project", required=True, help="a configured Project")
    parser.add_argument("--scratch", required=True, help="scratch directory for baselines")
    parser.add_argument("--repetitions", type=int, default=9)
    parser.add_argument("--concurrency", type=int, nargs="+", default=[1, 8])
    parser.add_argument("--output", default="bench/results/git-status-focus.json")
    arguments = parser.parse_args()
    if arguments.repetitions < 1:
        parser.error("--repetitions must be at least 1")

    scratch = Path(arguments.scratch)
    scratch.mkdir(parents=True, exist_ok=True)
    harness = Harness(Path(arguments.cli), Path(arguments.project).resolve(), scratch)
    if not harness.mounted():
        parser.error("the Project daemon is not serving a mount")

    report = {"repetitions": arguments.repetitions, "results": {}, "gate": {}}
    for concurrency in arguments.concurrency:
        results = measure_concurrency(harness, arguments.repetitions, concurrency)
        report["results"][f"concurrency_{concurrency}"] = results
        ratio = results["default_status_ratio"]
        report["gate"][f"default_status_concurrency_{concurrency}"] = {
            "ratio": ratio,
            "limit": GIT_STATUS_RATIO_GATE,
            "pass": ratio is not None and ratio <= GIT_STATUS_RATIO_GATE,
        }

    output = Path(arguments.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    write_json_atomically(output, report)
    for concurrency in arguments.concurrency:
        results = report["results"][f"concurrency_{concurrency}"]
        for command in COMMANDS:
            print(f"concurrency {concurrency} {command}: "
                  f"workspace {results[f'{command}_workspace']['median_ms']} ms, "
                  f"full copy {results[f'{command}_full_copy']['median_ms']} ms, "
                  f"ratio {results[f'{command}_ratio']}")
    passed = all(entry["pass"] for entry in report["gate"].values())
    print(f"git status gate: {'PASS' if passed else 'FAIL'} (raw samples in {output})")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
