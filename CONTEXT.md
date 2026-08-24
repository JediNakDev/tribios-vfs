# Tribios VFS

Tribios VFS provides persistent, isolated workspaces for concurrent coding work within a configured project.
Its language describes workspace behavior independently of Git, agent products, and storage implementation.

## Language

**Project**:
A directory configured once for Tribios to manage, whether or not it uses Git.
_Avoid_: Mount, session

**Git Project**:
A Project whose Workspaces use Git's linked-worktree metadata model while Tribios manages their filesystem contents.
_Avoid_: Git Workspace, cloned repository

**Non-Git Project**:
A Project with no version-control behavior supplied by Tribios.
_Avoid_: Plain Project, untracked Project

**Workspace**:
A persistent, mutable filesystem view within a Project.
_Avoid_: Git worktree, checkout, session

**Base state**:
The point-in-time filesystem state from which a Workspace begins.
Later changes to the source of the Base state do not appear in the Workspace.
_Avoid_: Live base, shared working tree

**Workspace contents**:
All regular files, directories, and symlinks included when Tribios captures a Base state after applying Capture exclusions, regardless of Git tracking or ignore rules.
Tribios metadata, special files, external symlink targets, and nested mounts lie outside the Workspace contents.
_Avoid_: Tracked files, source files

**Capture exclusion**:
A Project-relative rule that prevents matching content from entering future Base states.
It is independent of Git ignore rules and never changes existing Base states or Workspaces.
_Avoid_: Git ignore, Workspace ignore

**Workspace lifecycle**:
The period from explicit Workspace creation through explicit removal, matching the lifecycle of a Git worktree.
A Workspace remains available across command exits, Tribios restarts, and machine reboots.
_Avoid_: Command lifecycle, agent session

**Workspace isolation**:
A correctness boundary that prevents changes in one Workspace from changing the Project source or a sibling Workspace.
It is not a security boundary for running untrusted processes.
_Avoid_: Sandbox, containment
