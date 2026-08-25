# Tribios VFS — Project Requirements and Scope

## 1. Project Overview

**Tribios VFS** is a virtual filesystem designed for **highly parallel, isolated, and overlapping development workspaces**, with parallel AI coding agents as its primary target workload.

Modern coding agents frequently operate by creating multiple isolated workspaces from the same repository. These workspaces are usually highly similar: they share most source files, dependencies, toolchains, and repository state, while each agent modifies only a relatively small subset of files.

Traditional filesystems treat these workspaces mostly as independent directory trees. As the number of concurrent agents increases, this can lead to unnecessary filesystem work, duplicated storage, metadata pressure, I/O contention, and expensive workspace creation and cleanup.

Tribios VFS aims to reduce that overhead.

Its central goal is:

> **Make parallel agent workspaces cheap to create, cheap to modify, cheap to run, and cheap to destroy.**

Tribios VFS should optimize the filesystem lifecycle of parallel agentic development rather than only accelerating a single command such as `git worktree add`.

---

## 2. Primary Problem

Parallel AI coding agents introduce a workload with several important characteristics:

- Many agents start from the same repository state.
- Workspaces are highly similar and diverge only slightly.
- Most files are read far more often than they are modified.
- Agents frequently create temporary or short-lived workspaces.
- Multiple agents may build, test, install dependencies, or run tooling concurrently.
- Workspaces contain large amounts of duplicated source, dependency, cache, and build data.
- Cleanup can involve deleting very large directory trees.
- The number of concurrent workspaces may scale from a few agents to dozens of agents.

A typical workload looks like:

```text
                         Repository
                             │
             ┌───────────────┼───────────────┐
             │               │               │
          Agent A         Agent B         Agent C
             │               │               │
        Workspace A     Workspace B     Workspace C
             │               │               │
         modify few       modify few       modify few
           files            files            files
             │               │               │
           build            test            build
             │               │               │
             └───────────────┴───────────────┘
```

Most data across these workspaces remains identical.

Tribios VFS should exploit that fact directly.

---

## 3. Core Project Goal

The project should provide a virtual filesystem optimized for the following pattern:

```text
shared base state
       ↓
many isolated workspaces
       ↓
small independent mutations
       ↓
parallel build/test/tool workloads
       ↓
frequent workspace destruction
```

The filesystem should minimize the cost of this pattern in terms of:

- workspace creation latency
- workspace deletion latency
- physical storage usage
- duplicated file content
- filesystem metadata operations
- unnecessary physical writes
- directory traversal overhead
- I/O contention between parallel agents
- repeated dependency and build-state materialization

---

## 4. Primary Target Workload

The primary workload is:

> **Parallel AI coding agents operating on isolated development workspaces derived from the same repository.**

The most important initial use case is agentic worktree-style development:

```text
base repository
    ├── agent workspace 1
    ├── agent workspace 2
    ├── agent workspace 3
    └── agent workspace N
```

However, Tribios VFS should optimize the more general **workspace pattern**, not depend directly on Git worktree semantics.

Git worktrees are therefore a primary use case, but not the fundamental abstraction of the project.

---

## 5. Workspace Characteristics

Tribios VFS should assume that typical workspaces have the following properties:

1. They are derived from a shared base state.
2. Most files remain identical across sibling workspaces.
3. Mutations are relatively sparse compared with the size of the repository.
4. Workspaces are isolated from each other.
5. Workspace lifetime may be short.
6. Workspace creation and destruction may happen frequently.
7. Multiple workspaces may be active concurrently.
8. Builds and tools may generate substantial temporary or derived data.
9. Read operations are usually more common than source-file writes.
10. The filesystem may need to support large numbers of small files.

These assumptions should guide optimization decisions later.

---

## 6. Main Optimization Targets

### 6.1 Workspace Creation

Creating a new workspace should be significantly cheaper than materializing an independent copy of the full repository.

The desired behavior is conceptually:

```text
existing workspace/base state
          ↓
      lightweight branch
          ↓
     new workspace
```

Workspace creation should ideally scale with the amount of new metadata required, rather than with the total number of unchanged files.

Target:

> **Workspace creation cost should be largely independent of repository size when the new workspace initially contains no unique data.**

---

### 6.2 Workspace Mutation

Each workspace must remain isolated while sharing unchanged content.

When an agent modifies a file:

```text
shared file
    ↓
workspace-specific version
```

Only the changed data should need to diverge.

The system should avoid duplicating unrelated files.

Target:

> **The physical cost of a workspace should be approximately proportional to the data that differs from its parent/shared state, not the total logical size of the workspace.**

---

### 6.3 Workspace Deletion

Deleting an ephemeral agent workspace should not require synchronously traversing and unlinking every logical file.

Traditional behavior may resemble:

```text
unlink file 1
unlink file 2
unlink file 3
...
unlink file N
```

Tribios VFS should aim for workspace-level removal semantics where the visible workspace can disappear quickly and storage reclamation can happen separately.

Target:

> **Workspace destruction should be fast even when the logical workspace contains a very large number of files.**

---

### 6.4 Shared Data

Unchanged data should be shared between workspaces where possible.

Examples include:

- repository source files
- vendored dependencies
- package manager dependencies
- SDK files
- toolchain files
- generated data that is identical across workspaces
- immutable build outputs
- cached artifacts

Target:

> **Identical immutable content should not require multiple physical copies solely because it appears in multiple workspaces.**

---

### 6.5 Small-File Workloads

Developer repositories and dependency trees can contain very large numbers of small files.

Tribios VFS should aim to reduce overhead associated with:

- file creation
- file deletion
- metadata lookup
- directory traversal
- repeated `stat`/lookup operations
- many concurrent readers
- managing large physical directory trees

This is especially important on backing filesystems where metadata-heavy workloads are relatively expensive.

---

### 6.6 Parallel Access

The system should remain useful when multiple agents operate concurrently.

Important workloads include:

```text
multiple readers
multiple independent writers
parallel builds
parallel tests
parallel package operations
parallel workspace creation/destruction
```

Target:

> **Performance should degrade gracefully as the number of concurrent workspaces increases.**

Tribios VFS should seek to reduce shared-storage contention rather than introducing a new global bottleneck.

---

### 6.7 Storage Efficiency

Physical disk consumption should grow more slowly than logical workspace count.

For example, if eight agents work on almost-identical 2 GB repositories, the ideal storage footprint should be much closer to:

```text
shared base
+
workspace-specific changes
+
workspace-specific artifacts
```

than:

```text
8 × complete workspace
```

Target:

> **Storage amplification should primarily reflect workspace divergence, not workspace count.**

---

## 7. Workloads to Support

Tribios VFS should not be language-specific.

The same workspace model should provide value across different development ecosystems.

### TypeScript / JavaScript

Typical filesystem pressure:

- very large dependency trees
- `node_modules`
- package installation
- large monorepos
- generated build directories
- frequent directory traversal

Representative operations:

```text
pnpm install
npm install
git status
git checkout
build
test
rm -rf node_modules
```

---

### C / C++

Typical filesystem pressure:

- source/header traversal
- many `open` / `stat` operations
- parallel compiler processes
- object files
- generated files
- build directories

Representative operations:

```text
cmake / configure
make -j
ninja
clang/gcc compilation
linking
clean builds
incremental builds
```

---

### Rust

Typical filesystem pressure:

- Cargo registry and dependencies
- `target/`
- `.rlib` files
- object files
- incremental compilation state
- generated build artifacts

Representative operations:

```text
cargo build
cargo test
cargo check
cargo clean
```

---

### OCaml

Typical filesystem pressure:

- Dune `_build/`
- compiler artifacts
- dependency scanning
- incremental build state
- many generated files

Representative operations:

```text
dune build
dune test
dune clean
```

---

### Other Ecosystems

The design should remain applicable to:

- Python
- Go
- Java
- Zig
- Swift
- other source-based development workflows

The project should optimize general workspace behavior rather than hardcoding specific package managers or languages.

---

## 8. Scope

Tribios VFS is intended to provide a **workspace-oriented virtual filesystem layer**.

The project scope includes:

- isolated development workspaces
- shared underlying data
- efficient branching of workspaces
- efficient mutation of workspaces
- efficient workspace destruction
- reduced duplicated storage
- improved behavior for large numbers of small files
- support for concurrent agent workspaces
- compatibility with development tools that expect normal filesystem semantics
- operation on top of existing host filesystems

The project may expose ordinary filesystem paths to tools such as:

```text
Git
compilers
package managers
build systems
test runners
editors
AI coding agents
shell tools
```

These tools should not require substantial changes solely to use Tribios VFS.

---

## 9. Backing Filesystem Scope

Tribios VFS should not be APFS-specific.

APFS may be an important initial motivation and benchmark target, but the project should be designed to operate on top of multiple POSIX-like backing filesystems.

Expected backing filesystems include:

```text
APFS
XFS
ext4
```

The intended relationship is:

```text
Applications
     ↓
Tribios VFS
     ↓
Host filesystem
     ↓
Storage device
```

Tribios VFS should not rely on one backing filesystem's internal implementation unless an optional optimization is explicitly introduced later.

Target:

> **Core Tribios behavior should remain portable across POSIX-like backing filesystems.**

---

## 10. Platform Scope

The project should conceptually separate:

- workspace/filesystem logic
- operating-system integration
- physical storage backend

Initial platform support may be narrower, but the architecture should not unnecessarily prevent future operation on multiple systems.

Potential environments include:

```text
Linux
macOS
```

Windows support may be considered later but is not required as an initial goal.

---

## 11. Compatibility Requirements

Tribios VFS should aim to behave like a normal development filesystem from the point of view of applications.

Important compatibility areas include:

- regular files
- directories
- file metadata
- permissions
- file descriptors
- rename
- unlink
- mkdir/rmdir
- symlinks
- hard links where required
- timestamps
- concurrent access
- directory traversal
- file locking where required by development tools
- filesystem notifications if needed for IDE/tool compatibility

The exact compatibility level and POSIX semantics will be specified later.

The current requirement is:

> **Existing developer tools should be able to operate on Tribios VFS without requiring Tribios-specific versions of those tools.**

---

## 12. Agent Awareness

Tribios VFS is optimized for agentic workloads, but the filesystem should not need to understand LLM-specific concepts.

Tribios does **not** need to know about:

```text
prompts
tokens
model names
conversations
reasoning state
LLM provider
```

The filesystem should instead understand useful storage/workspace concepts such as:

```text
workspace
parent/child relationship
shared state
private mutation
ephemeral state
persistent state
content identity
workspace lifetime
workspace lineage
```

This keeps the project useful beyond a specific AI provider or agent framework.

---

## 13. Worktree vs Workspace

Git worktrees are a major target workload, but the project should not be limited to implementing Git worktree acceleration.

Tribios VFS should optimize this underlying pattern:

```text
one shared base
      ↓
many parallel isolated views
      ↓
small independent changes
```

A Git worktree is one example of such a workspace.

Other possible users include:

- AI agent sandboxes
- CI workers
- parallel test environments
- temporary developer branches
- build workers
- automated refactoring jobs

Therefore:

> **Agentic worktrees are the primary use case; workspace-oriented parallel development is the broader system model.**

---

## 14. Desired Properties

Tribios VFS should aim for the following properties.

### Cheap Branching

Creating a child workspace should be inexpensive.

### Isolation

Changes in one workspace must not unexpectedly affect another workspace.

### Sharing

Unchanged data should remain physically shared when possible.

### Sparse Divergence

Physical storage usage should grow primarily with changed data.

### Fast Destruction

Ephemeral workspaces should be cheap to discard.

### Concurrency

Many workspaces should be able to operate in parallel.

### Transparency

Normal development tools should continue to see conventional filesystem behavior.

### Portability

The filesystem should avoid unnecessary dependence on a particular host filesystem.

### Measurability

Every optimization should be evaluated against real development workloads.

### Correctness

Performance improvements must not come at the cost of silent filesystem corruption or incorrect isolation between workspaces.

---

## 15. Performance Metrics

Tribios VFS should be evaluated using workload-level metrics rather than only raw sequential I/O throughput.

Important metrics include:

- workspace creation latency
- workspace deletion latency
- workspace branch latency
- repository traversal latency
- `git status` latency
- build time
- test time
- dependency installation time
- incremental build time
- disk usage
- physical bytes written
- physical bytes read
- number of filesystem operations
- metadata operation count
- CPU overhead
- memory overhead
- throughput under concurrent agents
- latency scaling from 1 to many agents

---

## 16. Parallel Scaling Benchmarks

A key part of the project should be measuring performance as parallelism increases.

Representative test matrix:

```text
1 agent
2 agents
4 agents
8 agents
16 agents
32 agents
```

For each level, measure:

```text
workspace setup time
aggregate runtime
per-agent runtime
disk usage
physical write volume
metadata pressure
workspace cleanup time
CPU utilization
memory utilization
I/O contention
```

This is more important than demonstrating a single isolated microbenchmark win.

---

## 17. Representative Benchmark Scenarios

The project should eventually include representative workloads such as:

### Workspace Lifecycle

```text
create N workspaces
modify a small number of files
delete all workspaces
```

### Read-Heavy Workload

```text
many agents traverse/read the same repository concurrently
```

### Sparse Mutation Workload

```text
each agent modifies 1–5% of files
```

### Parallel Build Workload

```text
N agents build related versions of the same project
```

### Dependency-Heavy Workload

```text
multiple workspaces run package/dependency tooling
```

### Short-Lived Agent Task

```text
create workspace
make one small change
run test
destroy workspace
```

### Long-Running Agent Task

```text
create workspace
modify files repeatedly
run build/test loops
destroy workspace
```

---

## 18. Success Criteria

Tribios VFS should be considered successful if it provides meaningful improvements for real parallel agent workflows, not merely synthetic benchmarks.

Potential indicators of success include:

- significantly lower workspace creation time
- significantly lower workspace deletion time
- substantially reduced physical storage usage across sibling workspaces
- reduced physical write amplification
- reduced filesystem metadata pressure
- reduced contention when multiple agents operate concurrently
- no significant regression in ordinary source reads
- acceptable CPU and memory overhead
- compatibility with existing development tools
- useful performance improvements across more than one language ecosystem
- useful operation on more than one backing filesystem

The exact numeric targets should be defined after baseline benchmarking.

---

## 19. Non-Goals

At least initially, Tribios VFS is **not** intended to be:

- a replacement for APFS, XFS, or ext4
- a new general-purpose kernel filesystem
- a distributed filesystem
- a network filesystem
- an object storage service
- an LLM memory system
- an agent communication system
- a source-control replacement
- a Git replacement
- a package manager
- a build system
- a container runtime
- a full VM/sandbox platform
- an AI orchestration framework
- a filesystem that only works for JavaScript/TypeScript
- a filesystem hardcoded around `node_modules`
- a filesystem hardcoded around one AI agent product

These systems may interact with Tribios VFS, but they are not the core project.

---

## 20. Design Decisions Deferred for Later

This document intentionally does **not** define the implementation architecture in detail.

The following questions should be designed separately:

- exact filesystem interface
- FUSE vs other userspace filesystem mechanisms
- metadata representation
- inode representation
- snapshot representation
- copy-on-write mechanism
- content-addressed storage design
- file packing
- compression
- whole-file vs chunk deduplication
- garbage collection
- caching
- journaling
- crash consistency
- durability semantics
- locking model
- concurrency control
- memory management
- on-disk format
- Git integration
- workspace API
- build/dependency sharing policy
- platform-specific optimizations

These are implementation/specification topics and should be decided after the project requirements and baseline workload are clear.

---

## 21. Working Project Definition

A concise definition of Tribios VFS:

> **Tribios VFS is a workspace-oriented virtual filesystem designed to reduce the storage, metadata, I/O, and lifecycle overhead of running many parallel AI coding agents over highly overlapping development workspaces.**

A shorter version:

> **A virtual filesystem optimized for parallel agentic development workspaces.**

A more technical version:

> **A virtual filesystem for parallel, isolated, highly overlapping development workspaces, optimized for cheap branching, sparse divergence, shared data, and fast destruction.**

---

## 22. Core Principle

The project is built around one key observation:

> **Parallel coding agents usually do not need independent copies of an entire development environment. They need isolated views of mostly shared state.**

Tribios VFS should turn that observation into filesystem-level performance and storage benefits.

Instead of treating:

```text
workspace A
workspace B
workspace C
workspace D
```

as four unrelated trees, Tribios should treat them as:

```text
                    Shared State
                         │
        ┌────────────────┼────────────────┐
        │                │                │
   Workspace A      Workspace B      Workspace C
        │                │                │
   small delta       small delta       small delta
```

The value of Tribios VFS will come from making this pattern efficient across the full agent workspace lifecycle.

---

## 23. First Prototype (issue #1)

The first prototype is a deliberately naive, throwaway experiment on macOS, tracked in issue #1.
It exists to produce a measured verdict, not production code, and it must not be merged into `main`.

Naive here means whole-file copy-on-write with nothing clever underneath.
No chunk deduplication, no content-addressed store, no compression, no reference counting, no garbage collection.
Private file data lives in ordinary directories on the backing filesystem, and SQLite holds the bookkeeping.

### Structure

```text
Project (a real Git repository)
    │
    │  configure once  ->  capture
    ▼
Base state (immutable; includes ignored/untracked content such as node_modules)
    │
    ├───────────────┬───────────────┐
    │               │               │
 Upper A         Upper B         Upper C        <- sparse, empty at creation
    │               │               │
    ▼               ▼               ▼
/mnt/proj/ws-a  /mnt/proj/ws-b  /mnt/proj/ws-c  <- ordinary paths; git/cmake/ninja work unmodified
```

### Read and write resolution

```text
read       -> present in Upper?  -- yes -->  serve from Upper
                    │
                    └-- no --> tombstoned?  -- yes -->  ENOENT
                                   │
                                   └-- no --> serve from Base (shared, zero copy)

write      -> present in Upper?  -- yes -->  write in place
                    │
                    └-- no --> copy the whole file up, then write

create     -> Upper only
delete     -> persistent tombstone in SQLite; Base untouched
rename     -> private to the workspace, same tombstone mechanism
unsupported-> explicit error, never a silent approximation
```

### Lifecycle

```text
tribios configure   ->  capture Base state (timed separately from creation)
tribios create ws   ->  insert record + git worktree --no-checkout
                        no traversal of the Base state
                        private branch, HEAD, and index; shared object database and refs
daemon restart      ->  remount every persisted workspace; identical visible state
tribios remove ws   ->  [1] logical: hide the workspace, commit removed state, return
                        [2] physical: reclaim the upper tree asynchronously, reported separately
```

Crash consistency during an interrupted mutating operation is out of scope for this prototype and is specified in issue #2.

### Pass or fail gates

The fixture is a reproducibly generated Git project of 100,000 files and roughly 2 GiB of logical data, including ignored dependency content.
Every case is measured for one workspace and for eight concurrent workspaces, against a full directory copy that reproduces the same contents.

- zero isolation, persistence, Git, build, and test failures
- median workspace creation and median logical removal each at least 10 times faster than the full-copy baseline
- an untouched workspace consumes no more than 1 percent of the Base-state physical storage
- median `git status` and representative build/test duration no worse than 1.5 times the native baseline

Missing any gate rejects the design on measurement rather than carrying it forward.
