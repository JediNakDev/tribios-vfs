# Replacing `git worktree` with Tribios Workspaces

Research for [issue #30](https://github.com/JediNakDev/tribios-vfs/issues/30), which blocks the `tribios configure` work in [issue #29](https://github.com/JediNakDev/tribios-vfs/issues/29), current as of 2026-10-18.
Repository paths refer to commit `b47905a`.

## Short answer

Transparently replacing `git worktree` for every caller is not achievable today.
Git ignores aliases that shadow built-in commands, no Git hook can skip or redirect the checkout, and the two open-source harnesses checked (Codex and T3 Code) create worktrees with plain `git worktree add` and no substitution seam.
A PATH shim could intercept some of them, but Tribios cannot honour what those commands ask for: an arbitrary path, an arbitrary base commit, a detached HEAD, or an existing branch.

Replacement is possible where a harness offers a supported seam.
Claude Code's `WorktreeCreate` and `WorktreeRemove` hooks replace its git worktree logic entirely and only need a directory path back, which `tribios workspace create` can supply.
That is a small, opt-in change that leaves `git worktree` untouched.

Recommendation: offer the Claude Code hook as the "replace git worktree" choice in `tribios configure`, back it with AGENTS.md instructions and an optional shell-command guard for other agents, do not ship a PATH shim, and first fix three gaps that already affect `tribios workspace create`: Base state drift from HEAD, unprotected Git admin entries, and relative-path worktree config.

## How a Tribios Workspace is registered with Git

`tribios workspace create <name>` runs `git worktree add --no-checkout -b <branch> .tribios/staging/<name>`, reads the admin directory from the staging `.git` file, rewrites `$GIT_DIR/worktrees/<id>/gitdir` to `<mount>/<name>/.git`, deletes the staging directory, and fills the index with `git read-tree HEAD`. [src/core/git_worktree.cpp:36-67](../../src/core/git_worktree.cpp)
The Workspace's `.git` file lives in its upper tree and the Workspace is served at `<mount>/<name>`, by default `.tribios/mnt/<name>`. [src/core/project_manager.cpp:245-247, 297-315](../../src/core/project_manager.cpp) [src/core/project_manager.cpp:122](../../src/core/project_manager.cpp)
The branch defaults to the Workspace name, and only a new branch can be created. [src/core/project_manager.cpp:245](../../src/core/project_manager.cpp) [src/cli/main.cpp:44](../../src/cli/main.cpp)
Removal marks the Workspace removed and later runs `git worktree remove --force` on the mount path, tolerating "is not a working tree". [src/core/project_manager.cpp:359-420](../../src/core/project_manager.cpp) [src/core/git_worktree.cpp:87-95](../../src/core/git_worktree.cpp)
On every daemon start, Tribios validates each active Workspace's `.git` pointer, its admin directory, the admin `gitdir` file, and its entry in `git worktree list --porcelain`, and refuses to open the Project if any check fails. [src/core/recovery.cpp:84-127](../../src/core/recovery.cpp) [src/core/project_manager.cpp:210-218](../../src/core/project_manager.cpp)

One semantic fact dominates the rest of this document.
A Workspace's files come from the Base state captured once at `tribios configure`, including untracked and ignored files, while its index comes from `HEAD` at Workspace creation. [CONTEXT.md:24-27](../../CONTEXT.md) [src/core/base_capture.cpp:13-19](../../src/core/base_capture.cpp) [src/core/git_worktree.cpp:58-67](../../src/core/git_worktree.cpp)
Tribios does not record which commit the Base state matched.
Once the main checkout's `HEAD` moves past the captured state, every new Workspace starts with `git status` showing the difference as local modifications.
`git worktree add <path> <commit-ish>` promises a checkout of `<commit-ish>`, and Tribios can only deliver the Base state.

## 1. Git aliases cannot override `git worktree`

The `alias.*` documentation says aliases that hide existing Git commands are ignored, except for deprecated commands. [git-config alias.*](https://git-scm.com/docs/git-config#Documentation/git-config.txt-alias) [Documentation/config/alias.adoc](https://github.com/git/git/blob/master/Documentation/config/alias.adoc)
The source agrees: `run_argv` calls `handle_builtin` before any alias lookup, `handle_builtin` exits when the command is a built-in, and `handle_alias` is only reached after the external `git-<cmd>` lookup fails. [git.c `run_argv`](https://github.com/git/git/blob/master/git.c)
`worktree` is a built-in, so neither `alias.worktree` nor a `git-worktree` executable on PATH is ever consulted.
No Git config key replaces a built-in subcommand.

## 2. Git hooks cannot make worktree creation cheap or redirect it

`post-checkout` is the only hook documented for `git worktree add`, and it runs after the worktree has been checked out, with the null ref, the new HEAD, and flag 1. [githooks post-checkout](https://git-scm.com/docs/githooks#_post_checkout)
It does not run when `--no-checkout` is used. [githooks post-checkout](https://git-scm.com/docs/githooks#_post_checkout)
In the source, `add_worktree` runs `post-checkout` only when the checkout succeeded and was not suppressed, and a hook failure does not remove the worktree. [builtin/worktree.c `add_worktree`](https://github.com/git/git/blob/master/builtin/worktree.c)
By the time it runs, the full checkout cost has already been paid, so it can only tidy up afterwards.

`reference-transaction` fires for any ref update, receives `<old> <new> <ref>` lines on stdin, and can abort only in the `preparing` and `prepared` states. [githooks reference-transaction](https://git-scm.com/docs/githooks#_reference_transaction)
`git worktree add -b` creates the branch through a separate `git branch` subprocess, and the worktree's HEAD is written through ref updates. [builtin/worktree.c `add`, `add_worktree`](https://github.com/git/git/blob/master/builtin/worktree.c)
This hook can veto those ref updates but cannot change the target path or skip the checkout.
No other hook is invoked by `builtin/worktree.c`. [builtin/worktree.c](https://github.com/git/git/blob/master/builtin/worktree.c)

Hooks are also not reliably run by harnesses.
Codex runs every worktree Git command with `-c core.hooksPath=/dev/null`. [codex-rs/worktree/src/git.rs:142-171](https://github.com/openai/codex/blob/ce0323f2f2d3b25e82806cd555237da3db4e1897/codex-rs/worktree/src/git.rs#L142-L171)

## 3. Harness seams

| Harness | How it creates worktrees | Default location | Supported way to substitute a command |
| --- | --- | --- | --- |
| Claude Code | `git worktree` by default; invocation method not documented | `.claude/worktrees/<name>`, branch `worktree-<name>` | Yes: `WorktreeCreate` and `WorktreeRemove` hooks |
| Codex (CLI TUI and desktop app) | `git` resolved from PATH: `worktree add --detach --no-checkout`, then `reset --hard` | `$CODEX_HOME/worktrees/<4 hex>/<repo>`, configurable root | No; setup scripts run after creation |
| Cursor | Not documented | Not documented | No; `.cursor/worktrees.json` only adds setup commands |
| T3 Code | `git` resolved from PATH: `worktree add [-b <branch>] -- <path> <ref>` | `<T3 base dir>/worktrees/<repo>/<branch>`, configurable | No; setup script runs after creation |

### Claude Code

`WorktreeCreate` fires for `claude --worktree`, the `EnterWorktree` tool, subagents with `isolation: "worktree"`, workflow agents, background sessions, and `claude remote-control` in worktree mode. [Claude Code hooks: WorktreeCreate](https://code.claude.com/docs/en/hooks#worktreecreate)
Configuring it replaces the default git behavior entirely, and `.worktreeinclude` is then not processed. [Claude Code hooks: WorktreeCreate](https://code.claude.com/docs/en/hooks#worktreecreate)
The input adds a `name` slug such as `bold-oak-a3f2` to the common fields, which include `cwd`. [Claude Code hooks: WorktreeCreate input](https://code.claude.com/docs/en/hooks#worktreecreate-input)
A command hook prints the worktree directory as the last non-empty line of stdout, and failure or a missing path fails creation. [Claude Code hooks: WorktreeCreate output](https://code.claude.com/docs/en/hooks#worktreecreate-output)
Claude Code refuses an absolute path containing `.` or `..` and any path that passes through a symlink below the repository root. [Claude Code hooks: WorktreeCreate output](https://code.claude.com/docs/en/hooks#worktreecreate-output)
A directory inside the repository is refused if git resolves it to the main checkout. [Claude Code worktrees: refuses to use a worktree](https://code.claude.com/docs/en/worktrees#claude-code-refuses-to-use-a-worktree)
A Tribios Workspace is a linked worktree whose `.git` file points at its own admin directory, so git should resolve it to the Workspace, not the main checkout, but this has not been tested.

`WorktreeRemove` receives `worktree_path`, exit 0 counts as removed, and a non-zero exit fails the removal if the directory still exists. [Claude Code hooks: WorktreeRemove](https://code.claude.com/docs/en/hooks#worktreeremove)
It does not fire when a subagent or workflow agent finishes; those hook-created worktrees are kept and must be removed by the user. [Claude Code hooks: WorktreeRemove](https://code.claude.com/docs/en/hooks#worktreeremove)
Claude Code never deletes the branch of a hook-created worktree. [Claude Code hooks: WorktreeRemove](https://code.claude.com/docs/en/hooks#worktreeremove)
When Claude runs `git worktree add` through its Bash tool, `WorktreeCreate` does not run, and the docs point to a `PreToolUse` hook with the `Bash` matcher to inspect or block that command. [Claude Code hooks: WorktreeCreate](https://code.claude.com/docs/en/hooks#worktreecreate)
The hook input carries no base ref, so `worktree.baseRef` cannot be honoured by a Tribios hook. [Claude Code worktrees: base branch](https://code.claude.com/docs/en/worktrees#choose-the-base-branch)
Whether Claude Code's built-in path resolves `git` from PATH or elsewhere is not documented and could not be confirmed.

### Codex

The docs say Codex creates worktrees in `$CODEX_HOME/worktrees`, in detached HEAD, with a configurable worktree root, and keeps the 15 most recent by default. [Codex git worktrees](https://learn.chatgpt.com/docs/environments/git-worktrees)
The docs describe no hook or command to replace creation; local-environment setup scripts run after Codex creates a worktree. [Codex local environments](https://learn.chatgpt.com/docs/environments/local-environment)
The documented hook events are `SessionStart`, `SessionEnd`, `PreToolUse`, `PermissionRequest`, `PostToolUse`, `PreCompact`, `PostCompact`, `UserPromptSubmit`, `SubagentStart`, `SubagentStop`, `Stop`, and `Interrupt`, with no worktree event. [Codex hooks](https://learn.chatgpt.com/docs/hooks)
The open-source `codex-rs/worktree` crate, described as using the Codex Desktop contract, builds its commands with `Command::new("git")`, which resolves `git` through PATH. [codex-rs/worktree/src/git.rs:143](https://github.com/openai/codex/blob/ce0323f2f2d3b25e82806cd555237da3db4e1897/codex-rs/worktree/src/git.rs#L143)
It runs `worktree add --detach --no-checkout <root> <sha>`, writes `core.worktree` into the new worktree's `config.worktree`, then runs `reset --hard <sha>`. [codex-rs/worktree/src/lib.rs:62-140](https://github.com/openai/codex/blob/ce0323f2f2d3b25e82806cd555237da3db4e1897/codex-rs/worktree/src/lib.rs#L62-L140)
It recognises a worktree as managed only if its canonical path sits under the managed root. [codex-rs/worktree/src/lib.rs:331-345, 383](https://github.com/openai/codex/blob/ce0323f2f2d3b25e82806cd555237da3db4e1897/codex-rs/worktree/src/lib.rs#L331-L345)
The managed root comes from `desktop.git-worktree-root` and must be absolute. [codex-rs/worktree/src/settings.rs:39-55](https://github.com/openai/codex/blob/ce0323f2f2d3b25e82806cd555237da3db4e1897/codex-rs/worktree/src/settings.rs#L39-L55)
Whether the closed-source desktop app runs this crate or its own implementation could not be confirmed.

### Cursor

Worktrees are a feature of the Agents Window, and `.cursor/worktrees.json` holds `setup-worktree`, `setup-worktree-unix`, and `setup-worktree-windows` commands run in the new worktree. [Cursor worktrees](https://cursor.com/docs/configuration/worktrees)
The page documents no location setting and no way to replace the creation command. [Cursor worktrees](https://cursor.com/docs/configuration/worktrees)
Cleanup keeps the newest worktrees up to a machine-wide cap of 25, and worktrees created outside Cursor under its worktree root, for example with `git worktree add`, are eligible for deletion. [Cursor worktrees](https://cursor.com/docs/configuration/worktrees)
How Cursor invokes git and where its worktree root is are not documented and could not be confirmed.

### T3 Code

T3 Code resolves `git` through `resolveSpawnCommand("git", ...)` with the spawn environment's PATH. [apps/server/src/vcs/GitVcsDriverCore.ts:965](https://github.com/pingdotgg/t3code/blob/13aa882c22d584de5388dfce3b3ca413387ba059/apps/server/src/vcs/GitVcsDriverCore.ts#L965)
`createWorktree` runs `git worktree add -b <new> -- <path> <ref>`, or `git worktree add -- <path> <ref>` for an existing ref, with the path defaulting to `<worktrees dir>/<repo>/<branch>`. [GitVcsDriverCore.ts:3423-3452](https://github.com/pingdotgg/t3code/blob/13aa882c22d584de5388dfce3b3ca413387ba059/apps/server/src/vcs/GitVcsDriverCore.ts#L3423-L3452)
The default worktrees dir is `<base dir>/worktrees`, with a user override in Settings, Storage. [apps/server/src/config.ts:136](https://github.com/pingdotgg/t3code/blob/13aa882c22d584de5388dfce3b3ca413387ba059/apps/server/src/config.ts#L136) [apps/server/src/worktreesDirectory.ts](https://github.com/pingdotgg/t3code/blob/13aa882c22d584de5388dfce3b3ca413387ba059/apps/server/src/worktreesDirectory.ts)
Managed worktree paths are compared by real path. [apps/server/src/worktreesDirectory.ts:54](https://github.com/pingdotgg/t3code/blob/13aa882c22d584de5388dfce3b3ca413387ba059/apps/server/src/worktreesDirectory.ts#L54)
Removal runs `git -c status.showUntrackedFiles=normal worktree remove`. [GitVcsDriverCore.ts:3825](https://github.com/pingdotgg/t3code/blob/13aa882c22d584de5388dfce3b3ca413387ba059/apps/server/src/vcs/GitVcsDriverCore.ts#L3825)
The worktree setup contract has a `setup-script` stage after `checkout`, and no provider hook for creation was found. [packages/contracts/src/worktreeSetup.ts](https://github.com/pingdotgg/t3code/blob/13aa882c22d584de5388dfce3b3ca413387ba059/packages/contracts/src/worktreeSetup.ts)
T3 Code is open source, so adding a creation hook upstream is a realistic path.

## 4. A PATH `git` shim

A shim would be an executable named `git` earlier on PATH that handles `worktree add` and `worktree remove` inside a Tribios Project and execs the real git for everything else.

What works:

- Codex and T3 Code resolve `git` from PATH, so a shim on their PATH would see their worktree commands. [codex-rs/worktree/src/git.rs:143](https://github.com/openai/codex/blob/ce0323f2f2d3b25e82806cd555237da3db4e1897/codex-rs/worktree/src/git.rs#L143) [GitVcsDriverCore.ts:965](https://github.com/pingdotgg/t3code/blob/13aa882c22d584de5388dfce3b3ca413387ba059/apps/server/src/vcs/GitVcsDriverCore.ts#L965)
- Recursion is avoidable: the shim must exec the real git by absolute path, never `git` by name, and Tribios must call the real git the same way.

What breaks:

- **Path mismatch.**
  Harnesses pass their own path and expect the worktree there; Codex and T3 Code check it by canonical or real path. [codex-rs/worktree/src/lib.rs:331-345](https://github.com/openai/codex/blob/ce0323f2f2d3b25e82806cd555237da3db4e1897/codex-rs/worktree/src/lib.rs#L331-L345) [worktreesDirectory.ts:54](https://github.com/pingdotgg/t3code/blob/13aa882c22d584de5388dfce3b3ca413387ba059/apps/server/src/worktreesDirectory.ts#L54)
  A symlink from the harness path to `.tribios/mnt/<name>` canonicalises to the mount and fails those checks, and Claude Code rejects symlinked worktree paths below the repository root. [Claude Code hooks: WorktreeCreate output](https://code.claude.com/docs/en/hooks#worktreecreate-output)
  A Linux bind mount needs `CAP_SYS_ADMIN`. [mount(2)](https://man7.org/linux/man-pages/man2/mount.2.html)
  Tribios serves one FUSE view per Project with Workspaces as its children, and new top-level directories cannot be created in it. [src/fuse/fuse_adapter.cpp:29-52, 215-221](../../src/fuse/fuse_adapter.cpp)
  Serving a Workspace at an arbitrary path would mean one mount per Workspace, a daemon redesign.
- **Base mismatch.**
  Codex asks for a detached HEAD at a specific SHA and T3 Code for a specific ref; Tribios can only offer the Base state on a new branch.
- **Follow-up commands.**
  Codex runs `reset --hard <sha>` after `worktree add`. [codex-rs/worktree/src/lib.rs:130-140](https://github.com/openai/codex/blob/ce0323f2f2d3b25e82806cd555237da3db4e1897/codex-rs/worktree/src/lib.rs#L130-L140)
  On a Tribios Workspace this would likely rewrite every tracked file whose index stat data does not match, copying it into the upper tree and erasing the copy-on-write benefit; this is inferred and untested.
- **Removal semantics.**
  Codex refuses to remove a worktree containing ignored files, and a Tribios Workspace contains the ignored files of the Base state. [codex-rs/worktree/src/lib.rs:304-317](https://github.com/openai/codex/blob/ce0323f2f2d3b25e82806cd555237da3db4e1897/codex-rs/worktree/src/lib.rs#L304-L317) [src/core/base_capture.cpp:13-19](../../src/core/base_capture.cpp)
- **Reach.**
  Callers that run git by absolute path or embed a Git library bypass the shim.
  GUI apps get their environment from the OS session, not the user's shell profile, so a PATH change in a shell rc file may not reach them; this has to be verified per app.
  Claude Code's and Cursor's git resolution could not be confirmed.
- **Cost and blast radius.**
  Every git call made on that PATH pays an extra process start and a Project lookup, including the frequent status polling that harnesses run.
  T3 Code's own comment notes its status polling runs beside the user's git commands. [GitVcsDriverCore.ts:955-959](https://github.com/pingdotgg/t3code/blob/13aa882c22d584de5388dfce3b3ca413387ba059/apps/server/src/vcs/GitVcsDriverCore.ts#L955-L959)
  The overhead has not been measured.
  A shim bug breaks git for every repository on the machine, not just Tribios Projects.
- **Nested git.**
  Git prepends its exec path to PATH for its own children, so subcommands git spawns internally do not necessarily pass through the shim. [git.c `setup_path` comment](https://github.com/git/git/blob/master/git.c)
  This is harmless for interception of top-level `worktree add`, but the shim cannot rely on seeing every git process.

To be correct, a shim would effectively have to reimplement `git worktree add` argument parsing and refuse every form Tribios cannot honour, which is most forms harnesses actually use.

## 5. What other `git worktree` commands do to a Tribios Workspace today

The admin entry is `.git/worktrees/<id>` with an absolute `gitdir` pointing at `<mount>/<name>/.git`. [src/core/git_worktree.cpp:49-55](../../src/core/git_worktree.cpp)
Tribios does not lock it.

- **`git worktree list`** shows `<mount>/<name>` on the Workspace branch.
  While the daemon is stopped the path is missing, so the entry is reported as prunable. [git-worktree list](https://git-scm.com/docs/git-worktree#Documentation/git-worktree.txt-list)
- **`git worktree prune`** removes entries whose `gitdir` points at a missing location and whose admin `index` is not newer than the expiry, and skips locked entries. [worktree.c `should_prune_worktree`](https://github.com/git/git/blob/master/worktree.c)
  The `prune` command starts with `expire = TIME_MAX`, so without `--expire` every such entry qualifies immediately. [builtin/worktree.c `prune`](https://github.com/git/git/blob/master/builtin/worktree.c)
  `git gc` runs `git worktree prune --expire 3.months.ago` unless `gc.worktreePruneExpire` says otherwise. [git-config gc.worktreePruneExpire](https://git-scm.com/docs/git-config#Documentation/git-config.txt-gcworktreePruneExpire)
  So `git worktree prune` with the daemon stopped, or `git gc` after three idle months, deletes the admin directory, and the next daemon start fails validation with "active Workspace Git administrative directory is missing". [src/core/recovery.cpp:95-99](../../src/core/recovery.cpp)
- **`git worktree remove <mount>/<name>`** first runs `git status --porcelain --ignore-submodules=none` unless forced, and refuses on any output. [builtin/worktree.c `check_clean_worktree`](https://github.com/git/git/blob/master/builtin/worktree.c)
  A Workspace usually fails this because the Base state contains untracked files or has drifted from HEAD.
  With `--force`, or on a clean Workspace, git deletes the working tree recursively, then deletes the admin directory even if the tree deletion failed. [builtin/worktree.c `remove_worktree`](https://github.com/git/git/blob/master/builtin/worktree.c)
  Through FUSE every file and the `.git` file are unlinked, and the final `rmdir` of the Workspace root fails with `EBUSY`. [src/fuse/fuse_adapter.cpp:231-237](../../src/fuse/fuse_adapter.cpp) [src/core/workspace_engine.cpp:634-636](../../src/core/workspace_engine.cpp)
  Tribios is left with an active but emptied Workspace and no admin entry.
  `tribios workspace remove` still cleans it up while the daemon runs, because removal tolerates a missing worktree, but a daemon restart first fails validation with "active Workspace has no valid .git pointer". [src/core/git_worktree.cpp:92-95](../../src/core/git_worktree.cpp) [src/core/recovery.cpp:87-93](../../src/core/recovery.cpp)
- **`git worktree move`** renames the directory before updating the admin files. [builtin/worktree.c `move_worktree`](https://github.com/git/git/blob/master/builtin/worktree.c)
  Out of the mount the rename crosses filesystems, inside it Tribios returns `EXDEV` across Workspaces and `EBUSY` for a Workspace root, so the move fails before anything changes. [src/fuse/fuse_adapter.cpp:251-253](../../src/fuse/fuse_adapter.cpp) [src/core/workspace_engine.cpp:647-650](../../src/core/workspace_engine.cpp)
- **`git worktree repair`** leaves a missing worktree alone and rewrites linking files only when they are broken, incorrect, or of the wrong absolute/relative kind. [worktree.c `repair_gitfile`](https://github.com/git/git/blob/master/worktree.c) [git-worktree --relative-paths](https://git-scm.com/docs/git-worktree#Documentation/git-worktree.txt---relative-paths)
  With default settings it is a no-op on a mounted Workspace.
  With `--relative-paths` or `worktree.useRelativePaths=true` it rewrites both files to relative paths, after which the `git worktree list` comparison in Tribios validation would no longer match; this is inferred and untested. [src/core/recovery.cpp:117-125](../../src/core/recovery.cpp)
- **`git worktree lock`** writes a `locked` file in the admin directory. [git-worktree lock](https://git-scm.com/docs/git-worktree#Documentation/git-worktree.txt-lock)
  Locked entries are never pruned, cannot be moved, and need `--force` twice to remove. [git-worktree remove](https://git-scm.com/docs/git-worktree#Documentation/git-worktree.txt-remove) [worktree.c `should_prune_worktree`](https://github.com/git/git/blob/master/worktree.c)
  Git documents locking exactly for worktrees on a device that is not always mounted. [git-worktree lock](https://git-scm.com/docs/git-worktree#Documentation/git-worktree.txt-lock)

A related latent issue affects `tribios workspace create` itself.
With `worktree.useRelativePaths=true`, git writes a relative `gitdir:` into the staging `.git` file. [worktree.c `write_worktree_linking_files`](https://github.com/git/git/blob/master/worktree.c)
Tribios uses that value as a path without resolving it against the staging directory, so registration would read or write the wrong location. [src/core/git_worktree.cpp:43-55](../../src/core/git_worktree.cpp)

## Approaches compared

Complexity is a rough estimate of new or changed code and of ongoing maintenance.

| Approach | Feasible | Coverage | Main risks | Complexity |
| --- | --- | --- | --- | --- |
| Git alias for `worktree` | No | None; built-ins win | None, it never runs | None |
| Git hook (`post-checkout`, `reference-transaction`) | No | Runs after the checkout or only vetoes refs; Codex disables hooks | Would only add cost | None |
| Harness setup scripts (Codex, Cursor, T3 Code) | No | Run after the harness has already made a full checkout | Would only add cost | None |
| Claude Code `WorktreeCreate`/`WorktreeRemove` hook | Yes | Every Claude Code worktree path except Claude typing `git worktree add` in Bash | Base state instead of `baseRef`; subagent Workspaces are not auto-removed; isolation check untested | Small: a hook script of a few dozen lines, `configure` writing it into `.claude/settings.local.json`, and a machine-readable `workspace create` output |
| `PreToolUse` guard on shell commands (Claude Code, Codex) | Yes | Agent-typed `git worktree add` in a Tribios Project | Text matching misses indirect invocations | Small |
| AGENTS.md section | Yes | Agents that read and follow it | Not enforced | Trivial; already item 3 of #29 |
| PATH `git` shim | Partly | Codex and T3 Code if PATH reaches them; unknown for Claude Code and Cursor | Path and base mismatch, Codex `reset --hard`, global breakage, per-call overhead | Large: worktree argument parsing, per-harness compatibility, and per-Workspace mounts and per-commit Base states in Tribios |
| Upstream hooks in Codex, Cursor, T3 Code | Depends on vendors | Each harness that adopts one | Not under project control | Small per harness once a hook exists; T3 Code accepts contributions |

## Recommendation

Keep `git worktree` exactly as it is and make every Tribios path opt-in, as #30 requires.
Never install a git alias, a global hook, or a PATH shim.

For the "replace git worktree" option in `tribios configure` (#29 item 1):

1. Offer it as "use Tribios Workspaces for Claude Code worktrees".
   When chosen, write `WorktreeCreate` and `WorktreeRemove` command hooks into `.claude/settings.local.json`, matching #29's preference for local configuration.
   `WorktreeCreate` runs `tribios workspace create <name> --branch worktree-<name>` (starting the daemon if needed) and prints the absolute Workspace path; `WorktreeRemove` maps `worktree_path` back to the Workspace name and runs `tribios workspace remove`.
   Turning the option off deletes the two hooks and Claude Code returns to plain `git worktree`.
2. Optionally add a `PreToolUse` guard for Claude Code and Codex that, inside a Tribios Project, tells the agent to use `tribios workspace create` instead of `git worktree add`.
   Make it advisory rather than blocking, so `git worktree` stays usable.
3. For every harness, write the short AGENTS.md section from #29 pointing at a Tribios usage doc.
4. Tell users that Codex, Cursor, and T3 Code managed worktrees keep using git until those products offer a creation hook, and file upstream requests, starting with T3 Code.

Before shipping the hook, fix these gaps, which already affect manual `tribios workspace create`:

- Record the commit the Base state matched and report drift between it and `HEAD` when creating a Workspace.
  Without this, a hook-created Workspace silently starts with unexplained changes, which is worse than a manual one because the user never chose Tribios for that session.
- Lock each Workspace's admin entry with `git worktree lock --reason "managed by Tribios"` so `git worktree prune`, `git gc`, `remove`, and `move` cannot break it, and unlock or double-force during Tribios removal.
- Resolve the staging `gitdir:` value relative to the staging directory, and decide how Tribios behaves when `worktree.useRelativePaths` is set.
- Add `--print-path` or porcelain output to `tribios workspace create` so hooks do not parse human-readable text.

Revisit a PATH shim only if Tribios later supports Workspaces at caller-chosen paths and from caller-chosen commits, and an important harness still lacks a hook.
Until then a shim would fail most real harness invocations and put every git call on the machine at risk.
