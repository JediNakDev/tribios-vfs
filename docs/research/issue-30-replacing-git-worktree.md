# Replacing `git worktree` with a Tribios command

Research for [issue #30](https://github.com/JediNakDev/tribios-vfs/issues/30), current as of 2026-08-27.

Local observations in this note were made on this machine with `git version 2.52.0` (`git --version`), in scratch repositories under `/tmp`.
Git source citations are against `git/git` on `master` as fetched on 2026-08-27, so line numbers are a snapshot, not a stable reference.

The two defects this note identifies, the prune hazard in section 6 and the stat-cache cost in section 3, were fixed in commit `a49ca30` shortly after it was written.
The analysis below is left as it was found, so it still describes them in the present tense.

## Recommendation

Tribios should not build a new design.
It already implements the only design worth shipping.
`register_linked_worktree` runs `git worktree add --no-checkout` into a staging directory, rewrites `worktrees/<id>/gitdir` to point at the mounted Workspace, and writes the matching `.git` file into the Workspace ([`src/core/git_worktree.cpp:28`](../../src/core/git_worktree.cpp)).
That is a real, registered linked worktree whose working tree happens to live on a copy-on-write mount, and I confirmed locally that Git does not care where the working tree physically lives.
So the honest answer to "is it sufficient and possible to replace `git worktree`" is: yes, and the replacement already exists as `tribios workspace create`.
Issue #30 is really a question about *interception and discoverability*, not about storage design.

For interception, ship exactly one mechanism and document the rest:

1. Ship a Claude Code `WorktreeCreate` and `WorktreeRemove` hook writer.
   This is a first-party, documented replacement point that "replace[s] the default `git worktree` logic entirely, including placing worktrees somewhere other than `.claude/worktrees/`" ([Claude Code worktrees](https://code.claude.com/docs/en/worktrees)).
   It is the only mechanism in this whole space that catches an agent harness reliably, because the harness asks for it.
2. Do not ship a `git` shim, a `git` alias, or a `git-worktree` on `PATH`.
   Two of the three are impossible, and the third is a footgun. See the interception section.
3. Ship the AGENTS.md section (issue #29 option 3) unconditionally rather than as a fallback.
   It is the only thing that works for Codex, Cursor, aider, and every harness Tribios has never heard of.
4. Add `git worktree lock` to Workspace creation. That one is a correctness bug today, not a feature. See failure cases.

Complexity verdict: low for the parts worth doing.
The hook writer, the AGENTS.md section, the `.gitignore` line and a `lock` call come to a few hundred lines of code and one config schema.
Intercepting `git worktree` generically is not achievable at any complexity, so the cost question never arises.

## What Tribios already does, and why that reframes the issue

ADR 0001 already decided this: "Git Projects will use Git's linked-worktree metadata instead of treating `.git` as Workspace contents" ([`docs/adr/0001-keep-git-metadata-in-linked-worktrees.md`](../adr/0001-keep-git-metadata-in-linked-worktrees.md)).
The Base capture skips `.git` and `.tribios` at the Project root ([`src/core/base_capture.cpp:31`](../../src/core/base_capture.cpp)), so a Workspace never contains a repository of its own.
After creation, the Workspace root holds a `.git` *file* pointing back at `<project>/.git/worktrees/<id>` ([`src/core/project_manager.cpp:303`](../../src/core/project_manager.cpp)).

So the real comparison is between one `git worktree` whose checkout Git materialized file by file and one whose checkout the kernel materialized as a copy-on-write clone.
The Git-visible object is identical in both cases.
That is a stronger pitch than a replacement would be.

## 1. `git worktree` on-disk mechanics

### The pointer pair

Observed locally with `git init` plus `git worktree add ../wt1 -b feat1`:

- The linked working tree root holds a `.git` file whose only content is `gitdir: /private/tmp/wt-exp/main/.git/worktrees/wt1`.
- The admin directory `<common>/worktrees/wt1/` contained exactly `HEAD`, `ORIG_HEAD`, `commondir`, `gitdir`, `index`, `logs/`, `refs/`.
- `commondir` contained the relative string `../..`.
- `gitdir` contained the absolute path `/private/tmp/wt-exp/wt1/.git`, that is, the path of the `.git` *file*, not of the working tree root.

`gitrepository-layout(5)` documents `worktrees/<id>/gitdir` as "a text file containing the absolute path back to the .git file that points to here", used "to check if the linked repository has been manually removed", and says "the mtime of this file should be updated every time the linked repository is accessed" (`man 5 gitrepository-layout`).
It documents `worktrees/<id>/locked` and `worktrees/<id>/config.worktree` in the same section.
`prune` is not a file; it is a command.

`git-worktree(1)` states the environment split directly: "Within a linked worktree, $GIT_DIR is set to point to this private directory ... and $GIT_COMMON_DIR is set to point back to the main worktree's $GIT_DIR. These settings are made in a .git file located at the top directory of the linked worktree" (`man git-worktree`, DETAILS).

### Per-worktree versus shared

`git rev-parse --git-path` is the documented way to resolve this, and `git-worktree(1)` says so: "The rule of thumb is do not make any assumption about whether a path belongs to $GIT_DIR or $GIT_COMMON_DIR when you need to directly access something inside $GIT_DIR."

Observed locally by running `git -C /tmp/wt-exp/wt1 rev-parse --git-path <p>` for each path:

| Path | Resolves to |
| --- | --- |
| `HEAD`, `index`, `logs/HEAD`, `ORIG_HEAD`, `MERGE_HEAD`, `FETCH_HEAD`, `COMMIT_EDITMSG`, `rebase-merge` | `worktrees/wt1/...` (per-worktree) |
| `refs/bisect`, `refs/worktree`, `info/sparse-checkout`, `config.worktree` | `worktrees/wt1/...` (per-worktree) |
| `refs/heads/main`, `objects`, `hooks`, `config`, `packed-refs`, `shallow`, `info/exclude` | `<main>/.git/...` (shared) |

This matches the documented exception list: refs are shared "except refs/bisect, refs/worktree and refs/rewritten" (`man git-worktree`, DETAILS).

### `extensions.worktreeConfig` and `core.bare`

By default the config file is shared across all worktrees, and `core.bare` / `core.worktree` in the common config apply to the main worktree only (`man git-worktree`, CONFIGURATION FILE).
Enabling `git config extensions.worktreeConfig true` moves per-worktree settings into `config.worktree` and removes that exception, and the manual warns "Older Git versions will refuse to access repositories with this extension."
That warning is backed by the format rule: a version-1 repository specifying an `extensions.*` key the running Git has not implemented means "the operation MUST NOT proceed" ([`Documentation/technical/repository-version.adoc`](https://github.com/git/git/blob/master/Documentation/technical/repository-version.adoc)).
The manual also calls out that `core.sparseCheckout` "should not be shared, unless you are sure you always use sparse checkout for all worktrees."

Relevance to Tribios: any per-Workspace Git config Tribios might want (see the stat-cache section) needs `extensions.worktreeConfig`, which is a repository-format change that a user's other tools may reject.
Tribios should treat that as opt-in and loud, never automatic.

### Moving, repairing, and pruning

Moving a linked worktree by hand requires updating the `gitdir` file, and `git worktree repair` does it automatically (`man git-worktree`, DETAILS).
I verified the manual path locally: I moved `wt1` to `/tmp/wt-exp/mnt/wt1`, rewrote one line of `worktrees/wt1/gitdir`, and `git -C mnt/wt1 status` and `git -C main worktree list` both worked immediately with no `repair` call.
The `.git` file inside the moved tree never changed, because it points at the admin directory, which never moved.
This is exactly the operation `register_linked_worktree` performs, so Tribios is on a documented path, not a hack.

`git worktree repair` also infers the backlink from the working tree's `.git` file when the admin `gitdir` is missing or wrong (`worktree.c`, `repair_worktree_at_path`, near line 790 of `worktree.c` on master).
That makes `repair` a viable recovery tool for a Workspace whose mount moved, provided the mount is present when `repair` runs.

Now the "missing but not prunable" cases. `should_prune_worktree` in [`worktree.c:941`](https://github.com/git/git/blob/master/worktree.c) returns "prune" when the admin directory is not a directory, when `gitdir` is missing, unreadable, short-read, or empty, or when the path in `gitdir` does not exist *and* the admin `index` mtime is at or below the expiry.
It returns "do not prune" as the very first check after the directory test if a `locked` file exists.
So there are exactly two ways to keep a missing worktree alive: the `locked` file, or a recent `index` mtime combined with a non-`TIME_MAX` expiry.

The `index` mtime escape hatch does not apply to the manual command.
`git worktree prune` sets `expire = TIME_MAX` before parsing options ([`builtin/worktree.c:259`](https://github.com/git/git/blob/master/builtin/worktree.c)), so `st_mtime <= expire` is always true and a missing path is always pruned.
Only `git gc`, which calls `git worktree prune --expire 3.months.ago` by default (`gc.worktreePruneExpire`, `man git-config`), gets the grace period.
I confirmed both halves locally: with the Workspace path moved away, `git worktree prune -v --dry-run` printed `Removing worktrees/wt1: gitdir file points to non-existent location`, and after `git worktree lock --reason "tribios workspace detached" <path>` the same dry run printed nothing.

## 2. What Git actually requires of a working tree

I hand-built an admin directory in `/tmp/handwritten` without ever calling `git worktree add`, writing only four things:

```
.git/worktrees/ws/commondir   -> "../..\n"
.git/worktrees/ws/gitdir      -> "/tmp/handwritten/ws/.git\n"
.git/worktrees/ws/HEAD        -> "ref: refs/heads/ws\n"
.git/worktrees/ws/index       -> git read-tree ws with GIT_INDEX_FILE set
ws/.git                       -> "gitdir: /tmp/handwritten/src/.git/worktrees/ws\n"
```

Result: `git -C ws status --short --branch` printed `## ws`, `git -C src worktree list` listed the Workspace, `git commit` inside it succeeded, and the new commit was immediately visible from the main checkout via `git -C src log ws`.
`git worktree add /tmp/handwritten/ws2 ws` from the main checkout then failed with `fatal: 'ws' is already used by worktree at '/tmp/handwritten/ws'`, so the hand-written entry participates fully in branch exclusivity.

That answers the third design option: hand-writing the admin directory works today with a four-file minimum.

But do not do it.
There is no stability guarantee for this layout.
`repository-version.adoc` guarantees the opposite of what a hand-writer wants: it guarantees that Git *refuses* to operate on formats it does not understand, and explicitly says format bumps "should be kept to an absolute minimum" in favour of per-file versioning.
Nothing in `gitrepository-layout(5)` or `git-worktree(1)` promises the worktree admin layout is a public write interface, and `worktree.c` carries a `NEEDSWORK` comment right above `get_worktrees_internal` acknowledging the metadata-access path is not in the shape Git wants ([`worktree.c:177`](https://github.com/git/git/blob/master/worktree.c)).
Meanwhile `git worktree add --no-checkout` gets the same result through a supported command, which is what Tribios already does.
The only cost of the supported route is the staging directory dance and the one-line `gitdir` rewrite, both of which are documented operations.

### The design Tribios should not switch to: a full `.git` copy per Workspace

If the Base state included `.git`, each Workspace would be an independent clone-like fork.
Enumerated consequences:

- Refs diverge silently. Each Workspace has its own `refs/heads`, so `main` in Workspace A and `main` in Workspace B are different commits with no error and no warning. Git's `already used by worktree` guard, which I verified above, would be gone entirely, and that guard is the single most useful safety property `git worktree` provides for parallel agents.
- No shared object store. Every Workspace pays full object storage. Worse, a commit made in a Workspace is invisible from the main checkout until an explicit `git fetch` from a path remote, so the "parallel agents, then merge" workflow gains a synchronization step per Workspace per round trip. This is the workflow Tribios exists to serve, so this alone disqualifies the design.
- Config, remotes and credentials duplicate. `remote.origin.url`, `credential.helper`, and any `git config --local` value is forked at capture time and drifts.
- Hooks duplicate. `hooks` resolves to the common dir today (verified above), so a hook edit in the main checkout reaches every Workspace. With a full copy it would not.
- The index is stale on arrival. See the next section; a copied `.git/index` carries the source volume's `dev`/`ino`/`ctime`.
- `core.fsmonitor` and `core.untrackedCache` are per-repository state that would need re-priming per Workspace, and the untracked cache in particular is keyed on directory mtimes that a copy does not preserve. I did not test either against a copy-on-write clone, so treat the exact failure mode as unverified. The design is disqualified on the ref-divergence point regardless.

### Does Git care where the working tree lives?

No, on the evidence above.
`validate_worktree` in [`worktree.c:362`](https://github.com/git/git/blob/master/worktree.c) checks only that the recorded worktree path is absolute, that `<path>/.git` exists, that it parses as a gitfile, and that it points back at `worktrees/<id>`.
There is no check on filesystem type, device number, or mount status.
`WT_VALIDATE_WORKTREE_MISSING_OK` explicitly allows a missing path, which is how `git worktree remove` and friends tolerate an unmounted tree.

`git worktree add --no-checkout` plus manual population is therefore a legitimate pattern: Git records the registration and leaves the tree empty, and whoever fills the tree is Git's business only through the index.

A nested Workspace path inside the main working tree is also fine.
I verified that `git worktree add .tribios/mnt/ws -b ws` inside a repository leaves the main `git status` reporting only `?? .tribios/`, that `.tribios/` in `.gitignore` silences it, and that the main checkout's `git ls-files --others` does not descend into the nested tree, because the nested `.git` file stops the walk.
That is the direct justification for issue #29's auto-`.gitignore` item.

## 3. The index and stat-cache problem

This is where the most is to be gained, and nothing addresses it today.

`register_linked_worktree` populates the Workspace index with `git read-tree HEAD` ([`src/core/git_worktree.cpp:65`](../../src/core/git_worktree.cpp)).
I verified locally what that produces: `git ls-files --debug` on a `read-tree` index shows every entry with `ctime: 0:0`, `mtime: 0:0`, `dev: 0 ino: 0`, `size: 0`, against a normal index that carries real values.

A zeroed stat cache means the first `git status` in a fresh Workspace must open and hash every tracked file.
For the "metadata-heavy Git operations" workload ADR 0007 measures against, that is the worst possible first impression, and it happens on every Workspace create.
A plain `git worktree add` does not have this problem, because the checkout that writes the files also writes their stat data into the index.
So on this one axis Tribios is currently *slower* than the thing it is replacing.

Git's stat comparison fields are documented in `core.checkStat`: the default checks many fields, and `minimal` excludes "sub-second part of mtime and ctime, the uid and gid of the owner of the file, the inode number (and the device number, if Git was compiled to use it) ... leaving only the whole-second part of mtime (and ctime, if core.trustCtime is set) and the filesize" (`man git-config`).
`core.trustctime` is "True by default" and turning it off ignores ctime differences (`man git-config`).

I tested whether that combination is enough to make a copied index valid across a fresh set of inodes.
Setup: a 20-file repository, files copied with `cp -p` so mtimes survive but inodes do not, the main checkout's `.git/index` copied into the linked worktree's admin directory.

- With default settings, `git status` was clean but rewrote the index stat data (`ino` changed from the source's `177684891` to the copy's `177685026`), which means it re-hashed.
- With `-c core.checkStat=minimal -c core.trustctime=false`, `git status` was clean and left the index untouched (`ino` still the source's `177684891`), which means it did not re-hash.

So the fast path exists, and it needs three things Tribios does not do today:

1. Preserve mtimes in the Base capture. `copy_workspace_contents` uses `std::filesystem::copy_file` and `chmod` only ([`src/core/base_capture.cpp:53`](../../src/core/base_capture.cpp)); it never sets times, so captured files carry capture-time mtimes. A `utimensat` per file fixes this.
2. Seed the Workspace index from the Project's index rather than from `read-tree`, when the Project's index is clean with respect to HEAD.
3. Set `core.checkStat=minimal` and `core.trustctime=false` for the Workspace.

Item 3 is the awkward one.
`git config --worktree` needs `extensions.worktreeConfig`, which as quoted above makes older Git refuse the repository.
Setting it in the shared `.git/config` instead changes behaviour for the user's main checkout too.
The honest options are: make it an explicit opt-in in `tribios configure`, or skip item 3 and accept one re-hash pass per Workspace.

One thing I did not measure, and it matters before anyone promises a number: what APFS `clonefile`, an APFS sparse-image copy, a Btrfs snapshot, and an OverlayFS lower-to-upper copy-up each do to `st_ino` and `st_ctime`.
I did not test any of them here.
The general shape is that a Btrfs snapshot preserves inode numbers within the snapshot's own inode namespace while `st_dev` changes, an APFS clone allocates a new inode, and OverlayFS copy-up allocates a new upper inode on first write.
Under `core.checkStat=minimal` all of `dev`, `ino`, and `ctime` drop out of the comparison, so the design above should survive all three, but that reasoning comes from the documented field list rather than from measurement.
Measure it before it goes in a benchmark claim.

## 4. Interception: can `git worktree` be replaced?

Short answer: not through Git, and only reliably through a harness that offers a hook.

### Aliases cannot shadow a built-in

`git-config(1)` says: "To avoid confusion and troubles with script usage, aliases that hide existing Git commands are ignored except for deprecated commands."

The source confirms the mechanism and the exception.
In [`git.c`](https://github.com/git/git/blob/master/git.c), `run_argv` (around line 842) consults `handle_alias` up front only when `is_deprecated_command(args->v[0])` is true, then calls `handle_builtin` (line 869), and `handle_builtin` (line 754) `exit()`s inside `run_builtin` whenever `get_builtin(cmd)` returns non-NULL.
The general alias lookup at the bottom of the loop is reached only after `execv_dashed_external` fails, which never happens for a built-in.

Verified locally: `git config alias.worktree '!echo TRIBIOS'` followed by `git worktree list` printed the normal worktree list.
A control alias for a name Git does not know, `git config alias.wt '!echo YES-TRIBIOS'`, printed `YES-TRIBIOS`.

### `git-worktree` on `PATH` or in `GIT_EXEC_PATH` cannot shadow a built-in either

`execv_dashed_external` ([`git.c:793`](https://github.com/git/git/blob/master/git.c)) is only reached after `handle_builtin` has already exited for a built-in name.

Verified locally with an executable `/tmp/wt-exp/bin/git-worktree` that prints `INTERCEPTED-PATH`:

- `PATH=/tmp/wt-exp/bin:$PATH git worktree list` printed the normal list.
- `GIT_EXEC_PATH=/tmp/wt-exp/bin git worktree list` printed the normal list.
- The control, `git foo` with `/tmp/wt-exp/bin/git-foo` on `PATH`, printed `EXTERNAL-FOO`.

So both of the "clean" interception ideas are dead.

### A `git` shim earlier on `PATH`

This is the only mechanism that technically works, and it should not ship.

What it breaks, in rough order of how often it will bite:

- Anything that execs `git` by absolute path. `/usr/bin/git`, `/opt/homebrew/bin/git`, and a bundled Git inside an IDE all bypass `PATH` entirely.
- Anything that does not use a shell. A shell *function* named `git` is invisible to `posix_spawn`, `execvp`, Node's `child_process.spawn` without `shell: true`, and Rust's `std::process::Command`. Only a shim *file* on `PATH` catches those, and only for processes that inherit the modified `PATH`.
- Non-interactive and non-login shells. A function or alias defined in `.zshrc` does not exist in `sh -c` from a daemon, a `launchd` job, or a CI runner.
- libgit2 and JGit based tools. They never exec `git` at all, so nothing on `PATH` matters. VS Code's built-in Git uses the `git` binary, but many extensions and GUI clients do not.
- Argument fidelity. A shim must forward every global option (`-C`, `-c`, `--git-dir`, `--work-tree`, `--exec-path`, `-p`) correctly to be transparent, and any bug in that forwarding becomes "Git is broken on my machine" from the user's point of view.
- Debuggability. When a shim misbehaves, the error surfaces as a Git error with no mention of Tribios.

A shim also cannot be scoped to a repository, because `PATH` is per process, not per directory.
Tribios would be changing the meaning of `git` for the whole machine to change it for one project.
That is a bad trade for a tool whose value proposition is "isolation".

### The mechanism that actually works: harness hooks

In Claude Code, the `WorktreeCreate` hook "replace[s] the default `git worktree` logic entirely, including placing worktrees somewhere other than `.claude/worktrees/`", the hook reads the requested name from JSON on stdin, and it "print[s] the directory path so Claude Code can use it as the session's working directory" ([Claude Code worktrees](https://code.claude.com/docs/en/worktrees)).
`WorktreeRemove` is the matching cleanup event.
The hooks reference states that for `WorktreeCreate`, "Any non-zero exit code causes worktree creation to fail", and that `WorktreeRemove` cannot block ([Claude Code hooks](https://code.claude.com/docs/en/hooks)).
This fires for `--worktree`, for `isolation: "worktree"` subagents, and for background sessions.

Two Claude Code details Tribios must design against:

- With a `WorktreeCreate` hook, "`.worktreeinclude` is not processed when you use `--worktree`. Copy any local configuration files inside your hook script instead." Tribios's Base capture already includes untracked and ignored files, so this is a non-issue and arguably an advantage.
- Claude Code validates a worktree's git identity before adopting it, and refuses when "its `.git` file points at the main repository's own `.git` directory, or git resolves its working tree to the main checkout through a `core.worktree` redirect."
  A Tribios Workspace's `.git` file points at `<main>/.git/worktrees/<id>`, not at `<main>/.git`, so it should pass, and the docs say a hook-created worktree "can pass the check."
  I could not test this end to end, so treat "Claude Code accepts a Tribios Workspace as an isolation worktree" as unverified until someone runs it. It is the single highest-value thing to check before building the hook writer.

Codex "creates worktrees in `$CODEX_HOME/worktrees`" and "uses Git worktrees under the hood", with the worktree left in detached HEAD ([Codex git worktrees](https://learn.chatgpt.com/docs/environments/git-worktrees)).
It supports `.worktreeinclude` but the documentation describes no hook or config that replaces the creation logic.
Whether Codex execs the `git` binary or uses a library is not documented, so I cannot say whether a `PATH` shim would even reach it.

Cursor "automatically creates and manages git worktrees for parallel agents" and reads `.cursor/worktrees.json` with `setup-worktree`, `setup-worktree-unix`, and `setup-worktree-windows` keys ([Cursor worktrees](https://cursor.com/docs/configuration/worktrees)).
Those keys run commands *inside a worktree Cursor has already created*; nothing documented replaces creation.
So Cursor is a documentation target, not an interception target.

I did not investigate aider or anything else against primary sources, so I have nothing to report there either way.

### The honest alternative

Issue #29's option 3 deserves better than consolation-prize treatment.
It has universal reach precisely because the agent reads it and complies regardless of which harness it is running under.
A short AGENTS.md section pointing at `docs/api.md` costs almost nothing and covers Codex, Cursor, aider, and everything unreleased.
The `WorktreeCreate` hook is a strict upgrade *for Claude Code specifically*, and should be presented that way rather than as a general "replace git worktree" feature that it cannot be.

## 5. Semantic gaps between a Tribios Workspace and a plain `git worktree`

Because Tribios creates a real linked worktree, most of the expected gaps do not exist.

All of the following work today, verified locally against a linked worktree with a relocated `gitdir`:

- Branch exclusivity. `fatal: '<branch>' is already used by worktree at '<path>'`, with `--force` as the documented escape hatch.
- `git worktree list` discoverability, including the `locked` and `prunable` annotations.
- Shared object store, so no per-Workspace object cost and `git log <other-branch>` works from any Workspace.
- Immediate ref visibility. A commit in a Workspace updates `<main>/.git/refs/heads/<branch>` directly, so the main checkout sees it with no fetch. This is the property the "parallel agents" workflow depends on and it is preserved.
- Shared `hooks`, shared `config`, shared `info/exclude`, shared credentials.

The genuine gaps and rough edges:

- The stat cache, covered above. That is the real one.
- Submodules. `git-worktree(1)` BUGS says plainly: "Multiple checkout in general is still experimental, and the support for submodules is incomplete. It is NOT recommended to make multiple checkouts of a superproject." That limitation is Git's, not Tribios's, but a Tribios Workspace inherits it, and a Base capture that copies `.git` out of submodule directories may behave differently from a `git worktree add` on a superproject. I have not tested it, so test before claiming submodule support either way.
- Git LFS, which I did not investigate. LFS keeps its object cache under `.git/lfs`, which resolves to the common dir, so it is probably shared and fine, but the smudge filter runs at checkout time and Tribios does no checkout, and I did not verify that.
- Sparse-checkout. `info/sparse-checkout` is per-worktree (verified), so a Workspace starts with none while the Project may have one. A Workspace materializes the whole tree from the Base state regardless of the Project's sparse settings. `core.sparseCheckout` in the shared config would then be inconsistent with the Workspace's contents. Worth an explicit unsupported note.
- `git config --local` from inside a Workspace writes to the shared config and affects the main checkout and every sibling Workspace. That is standard worktree behaviour, but it is surprising to a user who thinks of a Workspace as isolated, and it means Workspace isolation is a *filesystem* boundary and not a *Git configuration* boundary. Worth one sentence in `docs/api.md`.
- Detached-HEAD parity. Codex worktrees are detached HEAD by default; Tribios always creates a named branch. Not a defect, just a difference to document if Tribios ever backs a Codex flow.

## 6. Failure and recovery cases

- `git worktree prune` deletes a live Workspace's admin directory. This is the sharpest hazard and it is live today. When a Workspace is detached, its path stops resolving, `should_prune_worktree` reports `gitdir file points to non-existent location`, and manual `git worktree prune` uses `expire = TIME_MAX` so the grace period does not save it. Any `git gc` older than the three-month default is safe, but the manual command is not, and harness cleanup sweeps run it. The fix is to call `git worktree lock --reason "tribios workspace <name> is detached"` at creation and `git worktree unlock` only immediately before `git worktree remove`. I verified locally that `locked` suppresses the prune. Note that Claude Code already does exactly this for its own worktrees: "Claude runs `git worktree lock` on its worktree so that concurrent cleanup cannot remove it."
- Mount missing at git-invocation time. With the mount gone, the Workspace path does not resolve and `git status` in the main checkout is unaffected. `git worktree list` marks the entry `prunable` unless it is locked. Recovery is the existing `attach_workspace` path; `git worktree repair` is the fallback if the admin `gitdir` ever drifts.
- Crash during workspace create. Already handled: `rollback_linked_worktree_creation` unregisters both the Workspace path and the staging directory and deletes the branch ([`src/core/git_worktree.cpp:107`](../../src/core/git_worktree.cpp)), and `recovery.cpp` re-runs it. The staging directory exists precisely because `git worktree add` will not target a path that is not yet a mount, and that is the right shape.
- `.tribios` accidentally committed. Verified locally that without a `.gitignore` entry the main checkout reports `?? .tribios/`, and with `.tribios/` ignored it disappears. Committing it would commit a Workspace's `.git` pointer file and, on macOS, potentially the sparse image. Issue #29's auto-`.gitignore` item is worth doing, and should append rather than rewrite, and should be idempotent.
- A hook returning a path Tribios cannot mount. Claude Code fails worktree creation on any non-zero hook exit and prints an error naming the path if it cannot enter the directory. A `WorktreeCreate` hook must therefore create the Workspace, wait for the mount to be attached, and print the mount path on stdout and nothing else. Any diagnostic output has to go to stderr.

## 7. Complexity assessment

Rated against what Tribios already has.

| Work item | What it costs | Complexity |
| --- | --- | --- |
| Nothing, for the storage design itself | The registered-linked-worktree design already ships | none |
| `git worktree lock` at create, `unlock` before remove | Two command invocations plus their idempotent recovery paths | trivial |
| `.gitignore` append for `.tribios/` | Idempotent append, no rewrite | trivial |
| AGENTS.md section plus `docs/api.md` | Prose, plus a stable command contract to point at | low |
| Config file schema and `tribios configure` interactivity (issue #29) | One JSON schema, local versus committed decision, a TTY prompt path plus a non-interactive flag path for CI | low to moderate |
| Claude Code `WorktreeCreate` / `WorktreeRemove` hook writer | Merge into an existing `settings.json` without clobbering it, a small hook script, name-to-Workspace mapping, wait-for-mount, stdout discipline, and a removal path that tolerates an already-removed Workspace | moderate |
| Index seeding plus mtime-preserving capture | `utimensat` in the capture walk, index seeding with a clean-index precondition, and a fallback to `read-tree` | moderate, and the highest value per unit of work |
| `core.checkStat=minimal` per Workspace | Needs `extensions.worktreeConfig`, which is a repository-format change; must be explicit opt-in | moderate, mostly in explaining it |
| Generic `git worktree` interception via a `git` shim | Argument forwarding fidelity, absolute-path bypass, non-shell bypass, machine-wide blast radius, undebuggable failures | high, and should not be built |
| Hand-written worktree admin directories | Works today, no stability guarantee, replaces a supported command with an unsupported one for no benefit | moderate, and should not be built |

The overall verdict for issue #30 is that the complexity is low, because the hard part is already done and the expensive part is not achievable.
What remains is a configuration surface, one harness integration, one correctness fix, and one performance fix.

## 8. What I could not verify

- Whether Claude Code accepts a Tribios Workspace as a valid isolation worktree in practice. Highest-priority thing to test.
- The exact `WorktreeCreate` / `WorktreeRemove` input JSON field names. The hooks page was truncated on fetch; only the lifecycle table and the exit-code semantics came through.
- What APFS clone, APFS sparse-image copy, Btrfs snapshot, and OverlayFS copy-up do to `st_ino` and `st_ctime`. Reasoned from the `core.checkStat` field list, not measured.
- Whether Codex, Cursor, or aider exec the `git` binary or use a Git library. Not documented in first-party sources I could reach.
- Submodule and Git LFS behaviour in a Tribios Workspace.
