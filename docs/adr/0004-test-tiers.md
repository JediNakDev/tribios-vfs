# Sort tests into four tiers, each with a wall-clock budget and a gate

Tests are sorted into four tiers.
Each tier has a wall-clock budget and one gate it runs at, and a test's tier is decided by its measured cost and by what its expected value traces to.

Tier 0 is unit tests.
They run on every build with a 2s budget, and they cover self-contained logic that has no externally observable seam: path normalization and traversal rejection, control-protocol framing, capture-exclusion matching and tombstone resolution rules.
This is an explicit amendment to the principle stated at the top of `tests/CMakeLists.txt`, that tests observe behavior only through the external seam.
Engine behavior stays at the `tribios fs` seam.
Only pure logic goes in tier 0.

Tier 1 is forced-invariant end-to-end tests.
They run pre-commit with a 60s parallel wall-clock budget, and they include lifecycle, filesystem, isolation, restart, crash-recovery, injected-I/O, seeded-sequence, and mounted-recovery tests.
CI uses four CTest workers because each script owns an independent temporary Project.
`restart_persistence` is a forced invariant because Workspace lifecycle in `CONTEXT.md` guarantees that a Workspace remains available across command exits, Tribios restarts and machine reboots.

Tier 2 is designed-decision end-to-end tests.
They run on every pull request with a 10 minute budget, and they are `configure_contract`, `unsupported_operations`, `git_workflow` and `end_to_end_build`.
`configure_contract` is the ADR-traceable half of a split of `workspace_lifecycle`, and it takes the assertions that configure is once and immutable, the secrets warning, the Base state size report, the under-1-percent untouched storage check and the two-phase removal reporting.
`workspace_lifecycle` keeps the create, list, contents and remove invariants and stays in tier 1.
That split takes the two tiers from 8 tests to 9.

Tier 3 is benchmarks.
They run on release and nightly against a smaller fixture.
Release-only would mean finding a performance regression once, late, with a whole release to bisect through.

The rule that sorts tier 1 from tier 2 is about what a failure means.
If a tier 1 test fails the code is wrong, its expected value traces to a guarantee in a `CONTEXT.md`, and editing the assertion to make it pass is never allowed.
If a tier 2 test fails then either the code is wrong or a decision changed, its expected value traces to an ADR or to a documented deliberate limit, and editing the assertion is allowed only alongside an ADR edit in the same pull request.
A pull request that edits a tier 2 assertion without touching `docs/adr/` is a review flag.

The local pre-commit gate runs tiers 0, 1 and 2 together, until their combined wall clock exceeds the 60s tier 1 budget.
Splitting them locally today saves 7 seconds and buys a class of "passed locally, failed on the pull request" surprises.
The split is enforced in CI from the start, where macOS runner cost is real.
Moving the local cut point is triggered by the measured budget being exceeded, not by re-arguing the question.

The figures come from the pre-split suite on this commit, before `configure_contract` was carved out of `workspace_lifecycle`, so they are the cost of the tests as they stood and not of the tiers as they now stand.
The cold build was 1.3s and all 8 end-to-end tests ran in 10.6s wall and 1.4s user with `ctest` running serially, of which the tests now in tier 1 were 3.3s and those now in tier 2 were 7.2s.
No unit tests existed.

Alternatives considered and rejected: running everything on every change, which is affordable today but does not survive the crash-consistency work in issue #2; and gating by test type name rather than by measured cost and by what the assertion traces to, which puts fast invariant checks behind a slow gate.

This decision is recorded in a throwaway prototype (see `docs/prototype/README.md`), but unlike the prototype's design decisions it is not scoped to it.
The tiering is intended to carry into production work.
