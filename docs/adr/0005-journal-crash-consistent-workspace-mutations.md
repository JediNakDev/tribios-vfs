# Journal crash-consistent Workspace lifecycle transitions

Status: amended by issue #28.
Date: 2026-08-25.
Amended: 2026-08-27.
Tracking issues: #4 and #28.

ADR 0005 originally journaled every file mutation performed by the Prototype 1 userspace Workspace engine.
ADR 0007 removes that engine from the production data path.
The selected native filesystem now owns file-level atomicity, rename behavior, writeback, and sync semantics.

Tribios retains durable journaling only for Project and Workspace lifecycle transitions.
Workspace creation records intent before creating native storage, registering the Git linked worktree, writing the `.git` pointer, and committing the active state.
Recovery either validates a complete active Workspace or removes its Git registration and native storage.

Workspace removal records intent before detaching the native storage.
The removed state is committed only after the public Workspace path becomes inaccessible.
Git cleanup and physical storage reclamation then run asynchronously before the reclaimed state is committed.
Each detach, Git cleanup, and reclamation action accepts an already-completed previous action.

Startup settles every lifecycle journal record before exposing a Project.
It then restores active storage attachments, validates their Git linked-worktree state, and checks the metadata database.
An uncertain mismatch stops startup and records a stable recovery diagnostic instead of guessing.

The metadata format is version 2.
Prototype 1 metadata and upper-tree layouts are intentionally unsupported because that prototype was declared throwaway.
