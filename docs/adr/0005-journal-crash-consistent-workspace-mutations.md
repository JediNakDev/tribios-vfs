# Journal Workspace mutations with same-filesystem staged publication

Status: accepted.
Date: 2026-08-25.
Tracking issue: #4.

Tribios must recover every interrupted Workspace mutation before it exposes the affected Project.
The Project source, Base state, and sibling Workspaces must remain unchanged during normal operations and recovery.
Successful Workspace lifecycle commands and successful `fsync` calls must survive a supported machine restart.
Unsynced writes may recover to the old or new state, but recovery must never expose a mixture of those states.

Every mutating engine operation receives a durable SQLite journal identifier before Tribios changes visible upper-tree data.
The journal records the Workspace, operation kind, source path, destination path, tombstone effects, and phase.
The metadata format starts at version 1.

Tribios prepares new file or directory data under the Workspace's private `recovery` directory.
This directory shares a backing filesystem with the final upper-tree path.
Tribios flushes staged regular files and directories before changing the journal phase from `prepared` to `publishing`.
It then publishes the staged entry with an atomic rename, flushes the parent directory, applies tombstones, and removes the journal record in one SQLite transaction.

Recovery rolls back a `prepared` operation by removing its staged entry.
Recovery completes a `publishing` operation by publishing any remaining staged entry, removing a rename source or deleted upper entry, applying its tombstone effects, and deleting its journal record.
Each recovery action accepts an already-completed previous action, so another crash during recovery remains safe.

Workspace creation and logical removal use the same journal identifier scheme.
An interrupted creation before the active-state commit rolls back its upper tree, Git linked-worktree registration, and new branch.
An active-state commit makes creation durable even if the daemon dies before replying.
A removed-state commit makes logical removal durable before asynchronous reclamation starts.
The removal journal remains until Git cleanup, upper-tree cleanup, tombstone cleanup, and the reclaimed-state commit finish.
Startup completes an interrupted reclamation before exposing the Project.

The Workspace engine owns these rules.
The macFUSE and Linux FUSE callbacks forward operations and `fsync` requests without defining recovery policy.
Per-Workspace engine locks preserve operation order, while unrelated Workspaces proceed independently.
SQLite serializes only short metadata transactions, so Tribios does not use one global filesystem mutation lock.

Tribios validates the metadata database, Base-state directory, Workspace names, active upper trees, Git pointer files, Git object connectivity, tombstone paths, and the absence of unsettled operations before mounting.
An unsafe state stops startup and creates one stable diagnostic identifier.
`tribios recovery inspect` reads the version, pending operations, and retained diagnostics without starting recovery or mounting the Project.

Deterministic failpoints exist before and after journal commits, staged-data flushes, publication, source removal, tombstone commits, Workspace lifecycle commits, recovery commits, and command reply boundaries.
Before terminating the process, a failpoint flushes its name, process identifier, optional seed, operation identifier, Workspace, and path to `recovery.trace`.

We rejected SQLite-only metadata transactions because SQLite cannot atomically commit an ordinary filesystem rename.
We rejected in-place copy-up and writes because a crash can expose a partial file or make Base-state content reappear between upper-tree deletion and tombstone insertion.
We rejected whole-Workspace snapshots because each small mutation would copy unrelated private data and complicate concurrent Workspace progress.

This decision adds one staged copy for mutations that replace existing file data.
That cost buys a small engine interface, atomic visible publication, local recovery logic, and deterministic failure testing.
Temporary data and completed journal entries are reclaimed after publication or recovery.
Deterministic I/O injection covers journal writes, staged-data flushes, rename preparation, directory flushes, lifecycle state, and short writes.
A two-phase disposable-host harness under `tests/restart/` verifies successful file and directory sync across a real machine restart.
