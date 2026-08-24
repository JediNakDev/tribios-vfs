# Experiment record — prototype v1

THROWAWAY EXPERIMENT. Tracking issue: #1.

## Question

Can a userspace, whole-file copy-on-write design provide correct, persistent,
isolated Workspaces for a real Git/CMake/Ninja workflow on macOS while meeting
the lifecycle, storage and runtime performance gates below?

## Method

1. Generate the performance fixture: `python3 bench/generate_fixture.py <path>` —
   a reproducibly generated Git Project of 100,000 files and approximately 2 GiB
   of logical data, roughly 60 percent of it ignored dependency content.
2. Configure the Project and start the daemon.
3. Run the benchmark: `python3 bench/benchmark.py … --build-directory build`,
   which runs the correctness suite, then writes raw samples, medians, p95
   values, the environment and the gate evaluation to `bench/results/`. Every
   gate must have a measurement behind it: a missing or skipped one fails.
4. Attach the environment, the raw results file and the verdict to issue #1.

The full-copy baseline reproduces the same Workspace contents, including ignored
and untracked data, and excludes only Tribios' own storage.

## Gates

The prototype passes only if every gate below passes.

| Gate | Threshold |
| --- | --- |
| Correctness suite failures, and suites skipped for want of a mount | zero |
| Median Workspace creation vs full-copy baseline | at least 10x faster |
| Median logical removal vs full-copy baseline | at least 10x faster |
| Untouched Workspace physical storage | at most 1 percent of the Base state |
| Storage growth after mutations | at most 5 percent above the copied-up file sizes |
| Median `git status` vs native full-copy baseline | at most 1.5x |
| Representative build and test duration vs native baseline | at most 1.5x |
| Concurrency | measured at 1 and at 8 concurrent Workspaces |

Base-state capture time is reported separately and is not part of the creation
gate. Physical reclamation time and transient storage are measured and reported
but are not included in logical removal latency.
Storage measurements use allocated backing-store bytes, including the upper tree's private metadata file.
The harness requires at least five lifecycle samples and three build and test samples.
It also requires enough free scratch space for eight full-copy baselines plus a 1 GiB safety margin.

## Environment

Fill in from the `environment` block of the results file.

| Field | Value |
| --- | --- |
| macOS version | |
| Hardware | |
| Backing filesystem | |
| macFUSE version | |
| Compiler | |
| Git / CMake / Ninja versions | |
| Fixture files / bytes | |
| Commit | |

## Raw results

Attach `bench/results/latest.json` and the `ctest` output.

## Verdict

Pending the recorded macOS benchmark run.
