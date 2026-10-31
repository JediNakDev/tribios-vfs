# Troubleshooting Tribios VFS

## Command reference

```text
tribios configure <project> [--mount <path>] [--force]
tribios info [--project <path>]
tribios daemon start [--project <path>] [--no-mount]
tribios daemon stop|status [--project <path>]
tribios workspace create <name> [--branch <branch>] [--project <path>]
tribios workspace list [--project <path>]
tribios workspace remove <name> [--project <path>]
tribios workspace wait-reclaim [--project <path>]
tribios recovery inspect [--project <path>]
tribios version
```

Every command except `configure` and `version` accepts `--project <path>`, so it can run from outside the Project root.

## `tribios info` does not report `mount backend: mounted`

- Run `tribios daemon status`.
  If the daemon is stopped, run `tribios daemon start`.
- On macOS, check that macFUSE is installed (`brew list --cask macfuse`) and approved in System Settings > Privacy & Security.
  The approval takes effect only after a restart.
  On Apple Silicon, macFUSE also needs Reduced Security with user-managed kernel extensions, set from Recovery.
- On Linux, check that `fuse3` is installed and `/dev/fuse` exists.
- `tribios daemon start --no-mount` starts without a mount on purpose.
  Stop it and start it again without the flag.

## An operation fails with "Operation not supported"

Tribios fails these explicitly instead of faking them:

- hard links
- extended attributes
- record locking
- special files such as FIFOs and device nodes
- renames from one Workspace into another

Work around them inside the task, for example by copying instead of hard-linking.
Tell the user when a tool depends on one of them.

## The Workspace does not show a file the Project root has

The Base state is captured once, by `tribios configure`.
Files added to the Project source afterwards never appear in existing Workspaces.
Bring them in through Git, or copy them into the Workspace.
Run `tribios configure . --force` only if the user agrees to recapture the Base state.

## `workspace create` fails because the branch exists

A removed Workspace keeps its branch.
Choose a new name, pass `--branch <unused-branch>`, or ask the user before deleting the old branch.

## A removed Workspace still appears in a directory listing

The kernel can cache the directory entry until the mount's entry timeout expires.
The Workspace no longer answers reads or writes.
Wait a few seconds and list again.

## After a crash or reboot

Workspaces persist across daemon restarts and reboots.
Run `tribios daemon start`, then `tribios workspace list`.
Run `tribios recovery inspect` if a Workspace is missing or the daemon reports an interrupted operation, and show its output to the user.
