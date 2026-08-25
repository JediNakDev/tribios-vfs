# Tribios prototype v1: throwaway experiment

> **This is a throwaway experiment.**
> It exists to answer one question with measured evidence, not to become the production implementation.
> Nothing in `src/`, `tests/` or `bench/` on this branch is intended to be merged into `main`.
> Production work starts from the evidence this experiment produces, not from this code.

Tracking issue: [#1](https://github.com/JediNakDev/tribios-vfs/issues/1).

## The question

> Can a userspace, whole-file copy-on-write design provide correct, persistent, isolated Workspaces for a real Git/CMake/Ninja workflow on macOS while meeting the agreed lifecycle, storage and runtime performance gates?

The prototype passes only on measured evidence.
See `experiment.md` for the gates and the results template.

## Design in one page

```text
                    tribios (CLI)
                          │  Unix-domain control socket
                    tribios_daemon
                          │
        ┌─────────────────┴─────────────────┐
     FUSE adapter                    Workspace engine
   (thin OS seam)      ──────────▶   (all filesystem behavior)
                                            │
                        ┌───────────────────┼───────────────────┐
                  immutable Base       upper tree per      tombstones +
                       state             Workspace         records (SQLite)
```

- `tribios configure` captures one Base state per Project in `<project>/.tribios/base`.
  It includes regular files, directories and symlinks regardless of Git tracking or ignore rules, so a Workspace can build without reinstalling dependencies.
  It excludes Git administrative metadata, Tribios metadata, special files, nested mounts and external symlink targets.
  It preserves symlinks without following them.
  Later Project changes do not affect the captured Base state.
- One mounted Project view exposes visible Workspaces as its immediate children, so tools use ordinary filesystem paths.
- Each Workspace has one sparse upper tree merged over the Base state.
  The first mutation copies the complete Base-state file into the upper tree.
  New files and directories exist only in that upper tree.
  Persistent tombstones keep removed Base-state paths hidden, including after a directory at the same path is re-created.
- Git linked worktrees give each Workspace its own branch, HEAD, index and administrative state while sharing objects and refs.
  Tribios writes a `.git` pointer file instead of checking out another physical tree.
- SQLite stores Project records, Workspace records, lifecycle state and tombstones in `<project>/.tribios/meta.db`.
  Private file data stays in ordinary directories.
- Removal happens in two phases.
  Logical removal makes the Workspace inaccessible and commits its removed state before returning.
  Physical reclamation runs in the background and is reported separately.

## Layout

```text
src/core/    Workspace engine, Base-state capture, metadata store, lifecycle
src/fuse/    the FUSE adapter, the only operating-system specific code
src/daemon/  long-running daemon and its Unix-domain control interface
src/cli/     the `tribios` command
tests/e2e/   behavior tests driven through the CLI and Workspace paths
bench/       fixture generator and benchmark harness
```

## Platform

This issue #1 prototype supports macOS only.
CMake rejects other operating systems so that development convenience cannot silently widen the experiment's specified scope.
The adapter uses the FUSE 2.x API provided by macFUSE's kernel backend.
The macFUSE FSKit backend is deliberately unused because its feature and performance differences would confound the verdict.

## Running on macOS

```sh
brew install cmake ninja sqlite pkg-config
brew install --cask macfuse       # then approve it and reboot, see below
git clone <this repo> && cd tribios-vfs
git checkout <throwaway-prototype-branch>
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig   # where macFUSE puts fuse.pc
cmake -S . -B build -G Ninja
ninja -C build                                    # expect: FUSE <version> found
ctest --test-dir build --output-on-failure
```

Three prerequisites decide whether this works at all:

- Use Xcode 16 or newer for `std::expected`.
  An alternative is `brew install llvm` plus `-DCMAKE_CXX_COMPILER=$(brew --prefix llvm)/bin/clang++`.
  Xcode 15 and earlier ship a libc++ without `<expected>`.
- Approve the macFUSE kernel extension in System Settings, Privacy & Security, then reboot.
  Apple Silicon also requires Reduced Security with user management of kernel extensions, configured from Recovery.
- Put `fuse.pc` on the pkg-config path.
  If CMake cannot find an installed macFUSE backend, check `PKG_CONFIG_PATH` first.

If the suite reports two skips instead of nine passes, the build found no FUSE backend.
The remaining tests use the engine directly, while the two mounted-path tests are skipped.

## Build

Requires macOS, C++23, CMake, Ninja, SQLite and macFUSE.

```sh
cmake -S . -B build -G Ninja
ninja -C build
```

The build reports whether it found macFUSE.
Without macFUSE, the daemon still serves the control interface and Workspace engine, but the mounted view is missing.

## Use

```sh
build/tribios configure /path/to/project      # capture the Base state once
build/tribios --project /path/to/project daemon start
build/tribios --project /path/to/project workspace create agent-1
ls /path/to/project/.tribios/mnt/agent-1      # an ordinary filesystem path
build/tribios --project /path/to/project workspace list
build/tribios --project /path/to/project workspace remove agent-1
build/tribios --project /path/to/project daemon stop
```

Captured Base states include ignored and untracked files, which may contain secrets.
`configure` warns about this explicitly.
Workspace isolation is a correctness boundary, not a security boundary.
Every local process that can reach a Workspace can read its contents.

## Test

```sh
ctest --test-dir build --output-on-failure
```

Tests observe externally visible filesystem and Git behavior, not SQLite rows, callback order or upper-tree layout.
With macFUSE, they drive mounted Workspace paths using unmodified Git, CMake, Ninja, the compiler and shell commands.
Without macFUSE, the same test bodies drive the Workspace engine through `tribios fs`, and the two tests that require mounted paths are skipped.

## Benchmark

```sh
python3 bench/generate_fixture.py /tmp/fixture          # ~100k files, ~2 GiB
build/tribios configure /tmp/fixture
build/tribios --project /tmp/fixture daemon start
python3 bench/benchmark.py --cli build/tribios --project /tmp/fixture \
    --scratch /tmp/bench-scratch --build-directory build \
    --output bench/results/latest.json
```

The baseline is an equivalent full directory copy with the same Workspace contents, including ignored and untracked data.
Each timed case reports raw samples, median and p95 at one Workspace and at eight concurrent Workspaces.
Base-state capture time is separate from Workspace creation time.
Physical reclamation and transient storage are separate from logical removal.
The benchmark samples transient storage immediately before timed removal after its own writes stop and before reclamation can begin.
Storage totals include the upper tree plus measured allocation growth in SQLite and its WAL sidecars.

The harness requires at least five lifecycle samples and three clean build and test samples.
It refuses to pass a fixture smaller than 100,000 regular files or roughly 2 GiB.
Storage gates use allocated backing-store bytes rather than logical file sizes.
Before timing, the harness verifies that the scratch filesystem can hold eight full-copy baselines plus a 1 GiB safety margin.

The run executes the correctness suite and evaluates every gate in issue #1.
A gate without a measurement fails, so skipped mounted-path tests or benchmark cases produce FAIL.

## Deliberate limits of this prototype

- Unsupported semantics fail with `ENOTSUP`: hard links, extended attributes, record locking, special files and cross-Workspace renames.
- The prototype does not claim crash consistency if a process or machine stops during a mutating operation.
  Issue #2 specifies that work.
- Renaming a Base-state directory materializes that subtree into the Workspace.
- A removed Workspace stops answering immediately, but the kernel may keep its cached directory entry until the mount's entry timeout expires.
  FUSE 2.x has no working cache-invalidation call for the high-level API.
- Transient storage measurement assumes a quiescent Workspace and is not atomic against external writes through already-open handles.
- A removed Workspace keeps its branch, so reusing the name requires handling that branch first.
- No Linux, Windows, FSKit, FUSE 3, chunk-level deduplication, content addressing, compression or garbage collection.

## Terminology gap

`CONTEXT.md` defines Project, Git Project, Workspace, Base state, Workspace contents, Capture exclusion, Workspace lifecycle and Workspace isolation.
This prototype uses those terms as defined.
The following implementation terms remain local to the prototype until `/domain-modeling` decides whether they belong in the glossary:

- `Project view` is the mounted path whose immediate children are the Project's visible Workspaces.
- `Upper tree` is sparse per-Workspace storage for copied-up files, new files and new directories.
- `Tombstone` is the persistent record that a Base-state path was removed in one Workspace.
