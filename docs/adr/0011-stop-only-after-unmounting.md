# Stop the daemon only after its mount is gone

Status: accepted.
Date: 2026-10-18.

`tribios daemon stop` used to tell the daemon to shut down, and the daemon stopped serving before it tried to unmount.
On Linux it ran `fusermount3 -u` once.
When something held the mount at that moment, the unmount failed with `EBUSY`, and the daemon waited forever for a FUSE loop that would never end.
The CLI reported that the mount did not stop, and a second `daemon stop` could not reach a daemon that had already closed its socket.
The documented remedy, closing what holds the mount and retrying, could not work.

The holder is often brief.
After eight Workspaces ran `git status` on the 100,000-file fixture, the mount stayed busy for 100 to 300 milliseconds after every visible tool had left it, with no process holding a working directory, an open file, or a blocked call inside it.
Indexers, file watchers and virtual-machine file sharing hold mounts the same way.

The daemon now unmounts before it agrees to a shutdown request.
If the mount is already absent, including after a failed mount attempt, shutdown proceeds without invoking an unmount helper.
On Linux it retries a busy unmount every 100 milliseconds for up to 5 seconds.
If the mount is still busy, it refuses the shutdown, keeps serving, and replies with the reason, which the CLI reports.
Retrying `daemon stop` after closing what holds the mount then works.

A signal has no caller to refuse, so after a signal the daemon keeps trying to unmount until it succeeds.
Unmounting runs a helper process, so it is no longer done inside the signal handler.

macOS keeps forcing the unmount with `diskutil unmount force` when a plain `umount` fails.

## Considered options

A lazy unmount, `fusermount3 -u -z`, always detaches the mount at once.
It was rejected because the old daemon would keep serving the detached tree to whatever still holds it.
Nothing stops `daemon start` from launching a second daemon on the same Project in the meantime, and two daemons would then write the same Workspace storage.

## Consequences

`tests/e2e/daemon_stop_busy_mount.sh` and `tests/e2e/daemon_stop_failed_mount.sh` hold the behavior in place.
