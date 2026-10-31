---
name: tribios-parallel-workspaces
description: Use when the user wants to run several coding agents in parallel on one Git project, asks to "spin up a workspace per agent", "give each agent its own checkout", "isolate parallel agents", or mentions Tribios VFS or the `tribios` command. Creates, lists and removes copy-on-write Workspaces with Tribios VFS instead of `git worktree`, so each agent gets a full working tree, including node_modules and build output, without copying the project.
---

# Parallel agent Workspaces with Tribios VFS

Tribios VFS is a FUSE filesystem that gives each agent its own persistent, isolated Workspace of one Project.
Every Workspace starts from the same captured Base state and stores only the files it changes.
A Workspace is a normal Git linked worktree on its own branch, so Git, build tools and editors work unchanged.

Read `references/troubleshooting.md` when a command fails or `tribios info` does not report a mount.

## Rules

- Create and remove Workspaces only with `tribios workspace create` and `tribios workspace remove`.
  Never use `git worktree add` or `git worktree remove` on a Tribios Project, because Tribios owns the mounted worktree state.
- Run `tribios configure` once per Project.
  It captures an immutable Base state, and later edits to the Project source do not reach existing Workspaces.
- Treat Workspace isolation as a correctness boundary, not a sandbox.
  Any local process can read any Workspace, including secrets captured in the Base state.
- Do not run untrusted code on the assumption that a Workspace contains it.

## Step 1: check the install

```sh
tribios version
```

If the command is missing, tell the user to install Tribios VFS and stop.
Install commands for macOS, Debian, Ubuntu, Fedora, Amazon Linux 2023 and Arch are in the project README: https://github.com/JediNakDev/tribios-vfs#install.
On macOS, macFUSE must be installed and approved in System Settings > Privacy & Security, followed by a restart.

## Step 2: configure the Project and start the daemon

Run this from the Project root.
Skip `configure` if `tribios info` already describes the Project.

```sh
tribios configure .
tribios daemon start
tribios info
```

`configure` captures every regular file, directory and symlink, including ignored and untracked files such as `.env`, `node_modules` and build output.
Show the user the warning it prints and confirm they accept it before going on.

Continue only when `tribios info` reports `mount backend: mounted`.

## Step 3: create one Workspace per agent

```sh
tribios workspace create agent-one
tribios workspace create agent-two --branch feature/login-form
tribios workspace list
```

The Workspace name is the branch name unless `--branch` is given.
Each Workspace appears at `.tribios/mnt/<name>` under the Project root.
Point each agent at its own path, for example `cd .tribios/mnt/agent-one`.

Pick names that are unique and stable for the length of the task.
A removed Workspace keeps its branch, so reusing a name needs that branch merged, renamed or deleted first.

## Step 4: work inside the Workspace

Inside `.tribios/mnt/<name>`, use ordinary commands: edit, build, test, `git commit`, `git push`.
Changes stay in that Workspace.
Sibling Workspaces and the Project source do not see them.

Merge work back through Git, by pushing the Workspace branch and opening a pull request, or by merging the branch in the Project root.

## Step 5: clean up

Run removal from the Project root, never from inside the Workspace being removed.

```sh
cd /path/to/project
tribios workspace remove agent-one
tribios workspace wait-reclaim   # optional: waits for background storage reclaim
tribios daemon stop              # only when no agent still needs a Workspace
```

`remove` returns once the Workspace is gone.
Storage is reclaimed in the background.

## Output

After setting up Workspaces, report a table like this to the user:

| Workspace | Branch | Path |
| --- | --- | --- |
| agent-one | agent-one | /path/to/project/.tribios/mnt/agent-one |
| agent-two | feature/login-form | /path/to/project/.tribios/mnt/agent-two |

After cleanup, report which Workspaces were removed and whether the daemon is still running.
