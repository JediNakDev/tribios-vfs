# Seed each Workspace's Git index with the stat data its mount reports

Status: accepted.
Date: 2026-10-18.
Tracking issue: #32.

ADR 0001 gives every Workspace its own Git index.
Workspace creation used to fill that index with `git read-tree HEAD`, which records object ids but no stat data.
Git trusts an index entry without reading the file only when the entry's stat data matches the file, so the first Git command that refreshes a new Workspace's index read and hashed every tracked file through the mount.
On the 100,000-file, 2 GiB fixture that first `git status` took 24 seconds on macOS, while every later one took about 30 milliseconds.
Agents create Workspaces often and run `git status` first, so this was the slowness users met on every new Workspace.

`tribios configure` now hashes the Base state against HEAD once, at native speed, into a private index under `.tribios`.
It runs `git read-tree` and `git update-index --refresh` with the Base-state directory as the work tree, so Git applies the Project's own attributes and filters.
Git records stat data only for the files whose content matches the committed blob, so that index is the record of which Base-state files a Workspace can trust.
Configure records the commit it hashed against beside the index.

Workspace creation copies that index and runs `git read-tree -m -i <configure commit> HEAD`.
The two-tree merge keeps an entry's stat data only when its blob is the same at both commits, so a file changed since configure is hashed on first use.
The daemon then rewrites each verified entry to what the Workspace reports for the same file.
The two differ only in identity and precision: the Workspace's salted inode, the mount's device, the daemon's owner, and whole-second times.
An entry is verified only when the Base-state file's current stat data still equals what configure recorded, and every other entry gets empty stat data.
The result has exactly the entries `git read-tree HEAD` would produce, so seeding changes what Git rereads and never what it reports.

Git distrusts an entry whose mtime is not older, in whole seconds, than the index it was read from.
Configure therefore writes the Base-state index in a later second than the last captured file, which costs it at most one second.

Seeding falls back to the old unseeded index when it cannot apply.
That covers Projects configured before this change, SHA-256 repositories, an unborn HEAD, and any failure while seeding.
The fallback is correct and only slower.

## Considered options

Setting `core.checkStat=minimal` in each Workspace would let a copied index match without rewriting inodes.
It was rejected because Git would then compare only whole-second mtime and size, so a same-size edit within the same second would go unnoticed.
It would also need the `extensions.worktreeConfig` repository extension.

Refreshing each new Workspace's index in the background after creation would hide the latency but still read every file through the mount once per Workspace.

## Consequences

Tribios now reads and writes Git's index file format, versions 2 to 4, and computes its SHA-1 checksum.
Split indexes are rejected and fall back.
Workspace creation lstats every verified Base-state file once, which is native filesystem work proportional to the tracked file count, as `git read-tree` already was.
`tests/e2e/workspace_git_index.sh` holds the behavior in place.
