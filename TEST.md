# Tests

Tests are sorted into four tiers.
Each tier has a wall-clock budget and one gate it runs at.
`docs/adr/0004-test-tiers.md` records why.

Run the tier that matches what you are doing.
Do not run the full benchmark to check a code change, and do not skip a tier because it looks slow without measuring it first.

## Commands

```sh
cmake -S . -B build -G Ninja
ninja -C build
```

While writing code, after every build:

```sh
ctest --test-dir build -L unit --output-on-failure --no-tests=error
```

Before every commit:

```sh
ctest --test-dir build -L unit --output-on-failure --no-tests=error
ctest --test-dir build -L invariant --output-on-failure --no-tests=error
ctest --test-dir build -L design --output-on-failure --no-tests=error
```

The pre-commit gate runs tier 2 as well as tiers 0 and 1, even though CI defers tier 2 to the pull request.
The three tiers together currently cost about 11 seconds, so splitting them locally would save a few seconds and buy a class of "passed locally, failed on the pull request" surprises.
Stop running tier 2 locally once tiers 0 and 1 together exceed their 60s budget, and not before.

`--no-tests=error` is not optional.
Without it a label that matches nothing reports success, so a broken registration looks like a passing gate.

## The tiers

Tier 0 is unit tests, under `tests/unit/`, labeled `unit`.
They run on every build with a 2s budget.
They cover self-contained logic with no externally observable seam, and they use Catch2 v3, fetched at configure time.
No daemon, no FUSE mount, no Git, no sleeps.

Tier 1 is forced-invariant end-to-end tests, under `tests/e2e/`, labeled `invariant`.
They run before every commit and on every push, with a 60s budget.
They include lifecycle, filesystem, isolation, restart, crash-recovery, injected-I/O, seeded-sequence, and mounted-recovery coverage.
CI runs independent invariant scripts with `ctest --parallel 4` so deterministic restart matrices do not serialize unrelated fixtures.

Tier 2 is designed-decision end-to-end tests, under `tests/e2e/`, labeled `design`.
They run on every pull request, with a 10 minute budget.
They are `configure_contract`, `unsupported_operations`, `git_workflow`, `end_to_end_build` and `packaging_install`.

Tier 3 is the benchmark, under `bench/`.
It runs nightly against a small fixture and on release tags against the full 100,000 file, 2 GiB fixture that `docs/prototype/experiment.md` specifies.
It is never a development-loop gate.
`bench/benchmark.py` runs the whole correctness suite itself and fails any gate that has no measurement behind it, so a benchmark run is also a correctness run.

## Choosing a tier for a new test

Ask what a failure would mean.

If the code is wrong, it is tier 1, and its expected value traces to a guarantee in `CONTEXT.md`.
Editing a tier 1 assertion to make it pass is never allowed.

If either the code is wrong or a decision changed, it is tier 2, and its expected value traces to an ADR under `docs/adr/` or to a deliberate limit documented in `docs/prototype/README.md`.
Editing a tier 2 assertion is allowed only alongside an ADR edit in the same pull request.
A pull request that edits a tier 2 assertion without touching `docs/adr/` is a review flag.

If it needs no daemon, no mount, no Git and no sleeps, it is tier 0.
Tier 0 covers pure logic only.
Engine behavior belongs at the `tribios fs` seam in tiers 1 and 2, not in a unit test.
Do not refactor production code purely to make it reachable from tier 0.

Every test must be registered in a tier.
Add end-to-end scripts to `TRIBIOS_INVARIANT_TESTS` or `TRIBIOS_DESIGN_TESTS` in `tests/CMakeLists.txt`, which is what applies the ctest label.
An unregistered script never runs.

## Skips

`tests/e2e/lib.sh` exits 77 to report a skip, and `tests/CMakeLists.txt` sets `SKIP_RETURN_CODE 77`.
A build with no FUSE backend therefore reports green with the mounted-path tests skipped, which is right on a laptop and wrong as a gate.

Set `TRIBIOS_REQUIRE_MOUNT=1` to turn every skip into a failure, including a missing build tool.
CI sets it on the strict Linux job.
Set it locally when you need to know that a mounted run actually happened.

Hosted macOS runners cannot mount.
macFUSE needs manual kernel extension approval, a reboot, and Reduced Security on Apple Silicon.
So the strict mounted gate runs on Linux, the hosted macOS job builds and exercises the unmounted engine paths only, and the macOS measurements that decide issue #1 come from a self-hosted runner.

## Budgets

A tier that exceeds its budget is a problem to fix, not a reason to move the gate later.
`ctest` currently runs serially, 10.6s wall against 1.4s user, so `ctest -j` is the first thing to reach for.
Each end-to-end script builds its own Project under `mktemp`, so parallel runs do not share state.

Moving a tier boundary is a change to `docs/adr/0004-test-tiers.md`, not a local judgment call.
