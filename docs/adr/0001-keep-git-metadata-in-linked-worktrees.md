# Keep Git metadata in linked worktrees

Git Projects will use Git's linked-worktree metadata instead of treating `.git` as Workspace contents.
Workspaces share Git objects and refs while keeping their own HEAD and index, and Tribios manages the visible working-tree files.
This preserves normal Git behavior without copying repository storage into every Workspace.
