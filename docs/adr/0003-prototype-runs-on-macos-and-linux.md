# macOS and Linux use platform FUSE interfaces behind one adapter

Status: accepted.
Date: 2026-08-26.
Tracking issue: #3.

Tribios keeps filesystem policy in the shared Workspace engine and translates host requests in one thin FUSE adapter.
macOS builds that adapter against the API 26 interface supplied by macFUSE.
Linux builds it against libfuse3 API 31.
Linux FUSE 2 is no longer a supported build target.

The two libraries expose the same operations with a few different callback signatures.
The adapter isolates those type-level differences with compile-time guards around `getattr`, `readdir`, `rename`, `chmod`, `chown`, `truncate`, and `utimens`.
The libfuse3 initialization callback enables stable reported inode numbers, while macFUSE keeps its `use_ino` mount option.
macFUSE also keeps its volume name, AppleDouble suppression, and extended-attribute offset arguments.
Linux unmounts through `fusermount3`.

Both adapters call the same `ProjectManager` and `WorkspaceEngine` interfaces.
They share the metadata format, Base-state capture, Git linked-worktree behavior, journal, recovery protocol, and command output.
No Workspace rule is conditional on the host operating system.

Linux release checks run the mounted compatibility and recovery tests on x86-64 and arm64.
Each architecture runs with Projects backed by ext4 and XFS.
The mount uses `default_permissions`, does not request `allow_other`, and therefore stays private to the invoking user unless a future interface adds an explicit access choice.

We rejected keeping libfuse 2 on Linux because issue #3 requires libfuse3 and current distributions treat FUSE 3 as the normal development and runtime interface.
We rejected a copied Linux Workspace engine because fixes to copy-on-write, Git, or recovery behavior would then drift between platforms.
We also rejected two nearly identical callback files because duplicating the request translation would create the same drift risk without buying a smaller interface.
