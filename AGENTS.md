## Agent skills

This repository uses the Matt Pocock engineering skills, always used unslop and karpathy-guidelines if installed.
Before starting an engineering workflow, verify that the relevant Matt Pocock skills are available.
If they are unavailable, ask the user to install the Matt Pocock skills bundle before continuing.

## Common mistake to avoid

- When told to implemented, do not verify it by full benchmark or e2e test, it tooks very loang and causes friction in development.
- You should verify it by small unit test or integration test instead i.e. test that use less than 5 minutes.
- If needed to verify with full benchmark or e2e test, provide user the command snippet to run.
  - Setup command in a way that you can access logs and results easily.

## Local environment

Before building, testing, benchmarking, or using machine-specific tooling, read and follow `AGENTS.local.md` if it exists.
The file contains local-only instructions and must remain uncommitted.

## Code preferences

- Write direct, purpose-built code for the current requirements.
  Avoid abstractions, options, and generalization intended only for hypothetical reuse or future requirements.
- Prefer the simplest readable control flow with the fewest useful layers of indirection.
- Limit comments to short inline comments that explain nontrivial behavior.
- Use precise, descriptive names, even when they are long.
  Name a function after its exact operation, such as `open_socket_file` instead of `open_fd` or `open_file`.
- Use `glb_` for global-variable prefixes when a prefix is useful.
  Avoid opaque prefixes such as `g_`.
- Keep short, single-use logic inline, especially when a helper would only wrap another function in one or two lines.
- Extract a helper when logic is reused, forms a meaningful operation, or would otherwise make a function longer than roughly one screen, about 40 to 60 lines.
  A clear length or responsibility boundary can justify a single-use helper.
- Keep each file focused on one responsibility.
  Split unrelated responsibilities into separate files.
- Put declarations shared by multiple files in the same directory in a private header.
  Expose them publicly only when code outside that directory needs them.

### Tests

Run the tier that matches what you are doing, and never the full suite or the benchmark by reflex.
Run `ctest -L unit` after every build, and `-L unit`, `-L invariant` and `-L design` before every commit.
Every new test must be registered in a tier.
See `TEST.md`.

### Issue tracker

Track issues in this repository's GitHub Issues.
See `docs/agents/issue-tracker.md`.

### Triage labels

Use the five default Matt Pocock triage labels.
See `docs/agents/triage-labels.md`.

### Domain docs

Use the single-context domain-documentation layout.
See `docs/agents/domain.md`.
