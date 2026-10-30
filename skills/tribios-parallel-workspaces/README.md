# tribios-parallel-workspaces

An agent skill that runs parallel coding agents in isolated [Tribios VFS](https://github.com/JediNakDev/tribios-vfs) Workspaces.

Each Workspace is a full Git working tree, including ignored files such as `node_modules` and build output.
It costs only the files the agent changes, so ten agents do not mean ten copies of the project.

## What the skill does

- Checks that `tribios` is installed and the mount is live.
- Configures the Project once and starts the daemon.
- Creates one Workspace per agent and reports each path and branch.
- Cleans up Workspaces and the daemon when the work is done.
- Keeps the agent away from `git worktree`, which would corrupt Tribios-owned state.

## Requirements

- Tribios VFS installed (macOS with macFUSE, or Linux with fuse3).
- A Git project.

## Install

Copy the `tribios-parallel-workspaces` folder into your agent's skills directory, for example `~/.claude/skills/` for Claude Code.

## Example prompts

- "Set up three Tribios workspaces so three agents can work on issues 12, 14 and 15."
- "Give each agent its own checkout of this repo."
- "Clean up the agent workspaces and stop Tribios."

## License

MIT, same as Tribios VFS.
