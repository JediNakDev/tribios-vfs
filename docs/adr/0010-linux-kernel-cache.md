# Let the Linux kernel cache entries and attributes, and drop them on outside changes

Status: accepted.
Date: 2026-10-18.
Tracking issue: #32.

Every `lstat` through a FUSE mount costs a round trip to the daemon once the kernel's cached directory entry or attributes for the path expire.
The high-level libfuse defaults both timeouts to one second, so a `git status` that lstats every tracked file asks the daemon about each of them again on every run.
With eight Workspaces running `git status -uno` at once on the 100,000-file fixture, those requests queued in the daemon and each warm round took 2.2 to 2.8 seconds on Linux, against 76 milliseconds for eight plain copies.

On Linux the mount now sets `entry_timeout` and `attr_timeout` to 60 seconds.
The same warm eight-way round then takes 118 to 176 milliseconds.
`negative_timeout` stays at zero, so a name that did not exist is always looked up again, and directory listings are never cached.

A change made through the mount keeps the kernel's cache current, because the kernel itself performs it.
A change made inside the daemon does not, and a stale entry is now visible for a minute rather than a second: reads return the old size and content, a removed file still answers `stat`, and creating a file at a removed name fails.
Those changes are the Workspace lifecycle requests and `tribios fs` mutations, so the control server reports each path such a request changed, and the FUSE adapter drops what the kernel cached for it.
It invalidates the inode of the path and of its parent through libfuse, which covers attributes and cached file content.
When the path no longer names what it named before, as after a removal, a rename, or removing a Workspace, it also drops the name's directory entry from its parent.

The high-level libfuse API addresses files by path and never exposes the node id that entry invalidation needs.
The adapter reads it from the kernel instead, with `name_to_handle_at` on the parent directory in the mounted view, because the kernel encodes a FUSE file handle as the node id followed by its generation.
That lookup goes through the mount, so it runs only on the control thread and never on a FUSE request thread.
Only the stale name is dropped.
Invalidating a whole Workspace directory would be simpler, but it detaches the directory from every process whose working directory is inside it, and `getcwd` then fails, which breaks Git.

macOS keeps the one-second defaults.
There the kernel evicts vnodes well before 60 seconds once eight Workspaces walk 100,000 files each, so a longer timeout measured no gain, and macFUSE offers no equivalent of `name_to_handle_at` for FUSE node ids.

## Considered options

Raising only `attr_timeout` leaves every path lookup going to the daemon once its entry expires, and a lookup costs the same round trip as `getattr`.

Moving the Linux adapter to the low-level libfuse API would hand Tribios the node ids directly, but it would also mean reimplementing libfuse's path-to-node table, including how a directory rename moves every cached descendant.

Rejecting `tribios fs` mutations while a Project is mounted would avoid stale caches without invalidation, but the seam exists so the harness can drive identical semantics with or without a mount, and Workspace removal needs invalidation anyway.

## Consequences

The Linux adapter depends on the kernel's FUSE file handle layout, which has not changed since FUSE gained export support.
If the kernel ever stops encoding it, the adapter skips entry invalidation and a removed name stays visible until its entry expires, which is the old behavior stretched from one second to sixty.
`tests/e2e/kernel_cache_coherence.sh` holds the behavior in place.
