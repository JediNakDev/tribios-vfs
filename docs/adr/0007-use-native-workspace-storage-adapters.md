# Use native Workspace storage adapters

Status: accepted.
Date: 2026-08-27.
Tracking issue: #28.
Supersedes the production assumptions of ADR 0002 and ADR 0003.

Tribios uses one lifecycle-oriented Workspace storage module with platform-specific adapters.
macOS uses one immutable case-sensitive APFS sparse Base-state image and one sparse shadow per Workspace.
Linux prefers writable Btrfs snapshots and uses kernel OverlayFS on other supported local filesystems.
The selected backend and storage format version are persisted when the Project is configured.
An existing Project is never switched automatically.

The module interface covers capability probing, Base-state capture, Workspace creation, attachment, detachment, reclamation, and status inspection.
It deliberately has no read, write, lookup, listing, rename, deletion, tombstone, or copy-up operation.
After attachment, Git and developer tools reach the native kernel filesystem directly.
This small interface keeps platform mechanics local and prevents a future adapter from putting Tribios back on the per-file path.

The APFS adapter sizes the sparse Base image for the captured data plus a writable growth allowance.
The default allowance is the larger of four times the Base-state size and 16 GiB.
Unused volume capacity does not allocate equivalent physical storage, while each Workspace receives a clear capacity limit that status can report.

The Btrfs adapter creates a read-only Base subvolume and writable snapshots.
Logical removal renames a snapshot out of the public Workspace path before asynchronous deletion.
The OverlayFS adapter keeps a read-only lower tree and private user-owned upper and work directories.
It mounts in the host mount namespace, so every process sees the same path.

Linux mounting, unmounting, and privileged Btrfs deletion go through a narrowly scoped installed system service.
The service authenticates callers through Unix peer credentials and accepts private paths only below the caller's owned Project `.tribios` directory.
It accepts public Workspace targets only below a caller-owned configured Workspace root.
It is enabled once with `tribios install-privileges`; normal Workspace commands never prompt for a password.
Capability probing exercises the destructive half of each adapter as well as creation.

Workspace metadata stores only backend-neutral lifecycle state and the opaque locator needed by the selected adapter.
Lifecycle operations are idempotent so startup can repeat recovery after another interruption.
File-level atomicity, rename behavior, writeback, and sync semantics belong to the selected native filesystem.

Direct recursive reflink cloning was rejected because Workspace creation still traverses every Base-state entry.
The FUSE prototype was rejected for production because metadata-heavy Git operations remained 2.86 times native with one Workspace and about 4 times native with eight.
There is no FUSE fallback when native capability probing fails.
