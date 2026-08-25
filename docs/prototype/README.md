# Tribios prototype v1 — throwaway experiment

> **This is a throwaway experiment.**
> It exists to answer one question with measured evidence, not to become the
> production implementation. Nothing in `src/`, `tests/` or `bench/` on this
> branch is intended to be merged into `main`. Production work starts from the
> evidence this experiment produces, not from this code.

Tracking issue: [#1](https://github.com/JediNakDev/tribios-vfs/issues/1).

## The question

> Can a userspace, whole-file copy-on-write design provide correct, persistent,
> isolated Workspaces for a real Git/CMake/Ninja workflow on macOS while meeting
> the agreed lifecycle, storage and runtime performance gates?

The prototype passes only on measured evidence. See `experiment.md` for the
gates and the results template.

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

- **One Base state per Project.** `tribios configure` captures the Project's
  current regular files, directories and symlinks once into
  `<project>/.tribios/base`, regardless of Git tracking or ignore rules, so a
  Workspace can build and test without reinstalling dependencies. Git
  administrative metadata, Tribios metadata, special files, nested mounts and
  external symlink targets are excluded; symlinks themselves are preserved and
  never followed. Later changes to the Project source do not appear in an
  existing Base state.
- **One mounted Project view.** Its immediate children are the visible
  Workspaces, so every tool uses ordinary filesystem paths.
- **One sparse upper tree per Workspace.** Reads and directory listings merge
  the upper tree over the Base state. The first mutation of a Base-state file
  copies the whole file up. New files and directories exist only in the upper
  tree. Removals are persistent tombstones; a tombstone keeps hiding the
  Base-state subtree even after the path is re-created, so a removed directory
  never resurrects its old children.
- **Git linked worktrees.** Each Workspace gets its own branch, HEAD, index and
  worktree administrative state while sharing the Project's object database and
  refs. No second physical file tree is ever checked out: the Workspace exposes
  a `.git` pointer file, and Tribios supplies the working-tree files.
- **SQLite for metadata only.** Project records, Workspace records, lifecycle
  state and tombstones live in `<project>/.tribios/meta.db`. Private file data
  lives in ordinary directories.
- **Removal is two-phase.** Logical removal makes the Workspace inaccessible and
  commits the removed lifecycle state before returning. Physical reclamation of
  the upper tree runs in the background and is reported separately, so fast
  hiding cannot disguise expensive cleanup.

## Layout

```text
src/core/    Workspace engine, Base-state capture, metadata store, lifecycle
src/fuse/    the FUSE adapter — the only operating-system specific code
src/daemon/  long-running daemon and its Unix-domain control interface
src/cli/     the `tribios` command
tests/e2e/   behavior tests driven through the CLI and Workspace paths
bench/       fixture generator and benchmark harness
```

## Platforms

The adapter speaks the FUSE 2.x API, so the prototype builds, mounts and runs
its full test suite on both:

| | Backend | Install |
| --- | --- | --- |
| macOS | macFUSE | `brew install --cask macfuse` |
| Linux | libfuse 2.x | `apt-get install libfuse-dev` |

Three things differ, each behind an `__APPLE__` guard: the extended-attribute
callbacks take an extra offset on macFUSE, `volname` and `noappledouble` are
macFUSE-only mount options, and Linux unmounts through `fusermount -u`.

macOS with macFUSE is the platform whose measurements decide issue #1; Linux
numbers are reported separately and never substituted for it. See
`docs/adr/0003-prototype-runs-on-macos-and-linux.md`, which reopens the issue's
out-of-scope list. FUSE 3 is not supported: libfuse 3 does not offer the FUSE 2
API. The macFUSE FSKit backend is deliberately unused, since its current feature
and performance differences would confound the verdict.

## Running on macOS

```sh
brew install cmake ninja sqlite pkg-config
brew install --cask macfuse       # then approve it and reboot, see below
git clone <this repo> && cd tribios-vfs
git checkout claude/prototype-v1-issue-1-elho9b
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig   # where macFUSE puts fuse.pc
cmake -S . -B build -G Ninja
ninja -C build                                    # expect: FUSE <version> found
ctest --test-dir build --output-on-failure
```

Three prerequisites decide whether this works at all:

- **A compiler with `std::expected`.** Xcode 16 or newer, or
  `brew install llvm` plus
  `-DCMAKE_CXX_COMPILER=$(brew --prefix llvm)/bin/clang++`. Xcode 15 and
  earlier ship a libc++ without it and the build will fail on `<expected>`.
- **macFUSE approved by the system.** Its kernel extension needs approval in
  System Settings, Privacy & Security, and a reboot. On Apple Silicon it also
  needs Reduced Security with user management of kernel extensions enabled,
  set from Recovery. Without this the build succeeds and every mount fails.
- **`fuse.pc` on the pkg-config path.** If CMake reports no FUSE backend while
  macFUSE is installed, that export above is the reason.

If the suite reports two skips instead of eight passes, the build found no FUSE
backend: the tests ran against the engine directly and the mounted-path tests
were skipped.

## Build

Requires C++23, CMake, Ninja, SQLite and one of the FUSE backends above.

```sh
cmake -S . -B build -G Ninja
ninja -C build
```

The build reports which backend it found. Without one the daemon still serves
the control interface and the Workspace engine, so lifecycle and filesystem
semantics remain testable; only the mounted view is missing.

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

Captured Base states include ignored and untracked files, which may contain
secrets. `configure` says so explicitly. Workspace isolation is a correctness
boundary, not a security boundary: every local process that can reach a
Workspace can read its contents.

## Test

```sh
ctest --test-dir build --output-on-failure
```

Tests observe externally visible filesystem and Git behavior, never SQLite rows,
callback order or upper-tree layout. With a FUSE backend they drive mounted
Workspace paths using unmodified Git, CMake, Ninja, the compiler and ordinary
shell commands. Without one, the same test bodies drive the Workspace engine
through `tribios fs`, the seam the FUSE callbacks call, and the two tests that
need real tools against real paths report as skipped.

## Benchmark

```sh
python3 bench/generate_fixture.py /tmp/fixture          # ~100k files, ~2 GiB
build/tribios configure /tmp/fixture
build/tribios --project /tmp/fixture daemon start
python3 bench/benchmark.py --cli build/tribios --project /tmp/fixture \
    --scratch /tmp/bench-scratch --build-directory build \
    --output bench/results/latest.json
```

The baseline is an equivalent full directory copy reproducing the same Workspace
contents, including ignored and untracked data. Each timed case reports raw
samples, median and p95, at one Workspace and at eight concurrent Workspaces.
Base-state capture time is reported separately from Workspace creation time, and
physical reclamation and the storage a removed Workspace still holds are
reported separately from logical removal.

The run drives the correctness suite itself and evaluates every gate issue #1
decides on. A gate with no measurement behind it fails, so a run whose
mounted-path tests or cases were skipped reports FAIL rather than PASS.

## Deliberate limits of this prototype

- Unsupported semantics fail explicitly with `ENOTSUP` rather than pretending:
  hard links, extended attributes, record locking, special files and
  cross-Workspace renames.
- No crash consistency is claimed if a process or machine is interrupted during
  an individual mutating operation. That work is specified in issue #2.
- Renaming a Base-state directory materializes that subtree into the Workspace.
- A removed Workspace stops answering immediately, but the kernel may keep its
  cached directory entry until the mount's entry timeout expires. FUSE 2.x has
  no working cache-invalidation call for the high-level API.
- A Workspace keeps its branch when it is removed, so re-using the name later
  needs the branch dealt with first.
- No Windows, FSKit, FUSE 3, chunk-level deduplication, content addressing,
  compression or garbage collection.

## Terminology gap

`CONTEXT.md` defines Project, Git Project, Workspace, Base state, Workspace
contents, Capture exclusion, Workspace lifecycle and Workspace isolation, and
this prototype uses them as defined. It introduces three terms the glossary does
not yet cover, recorded here for `/domain-modeling` rather than silently
promoted:

- **Project view** — the single mounted path whose immediate children are the
  Project's visible Workspaces.
- **Upper tree** — the sparse per-Workspace storage holding copied-up files, new
  files and new directories.
- **Tombstone** — the persistent record that a Base-state path was removed in
  one Workspace.
