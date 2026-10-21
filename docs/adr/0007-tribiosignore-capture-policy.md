# Exclude Base-state content with a root `.tribiosignore`

Status: accepted.
Date: 2026-10-18.
Tracking issue: #5.

A Base state captures ignored and untracked files by default, because a Workspace has to build and test without reconstructing its environment.
That default also captures secrets, large disposable caches, and machine-specific output that the user may not want in every Workspace.
`.gitignore` cannot make this decision, because Git tracking and Tribios capture answer different questions: a dependency directory is usually Git-ignored and still belongs in a Workspace.

Tribios reads one policy file, `.tribiosignore`, at the Project root.
It applies only while capturing a new Base state.
Existing Base states and Workspaces never change when the file changes, because Workspace contents are fixed by the Base state they were created from.
Nested `.tribiosignore` files, `.gitignore`, the repository exclude file, and global Git excludes are not read.

The grammar is gitignore-style, so it is already familiar, and `docs/tribiosignore.md` documents every intentional difference.
Patterns are evaluated in file order and the last matching pattern decides.
Capture never descends into an excluded directory, so a negated pattern cannot re-include content beneath it.
This matches Git's rule and keeps an excluded tree out of capture time as well as storage.

Matching is byte-exact and case-sensitive on every host.
Git follows `core.ignorecase`, which would make the same policy capture different content on a case-insensitive macOS volume and on Linux.
A policy whose result depends on the host filesystem would break the rule that macOS and Linux share one Workspace engine and one set of semantics.

Tribios' fixed rules come before the policy and cannot be overridden by it.
Git and Tribios metadata, special files, and nested mounts are never captured, and symlinks are matched by their own path without following them.
`.tribiosignore` itself is always captured.
Tribios writes the exact bytes it compiled, rather than copying the file again, so a Workspace always holds the policy that shaped it even if the file changed during capture.

The whole policy is read and validated before anything is written.
With `--force`, the existing Base state is removed only after validation succeeds.
An unreadable file, a file that is not regular, a NUL byte, invalid UTF-8, an unterminated character class, a trailing escape, or a pattern with an empty path segment stops capture with the file and line.
A typo therefore cannot produce a quietly wrong snapshot.

`tribios configure --dry-run` runs the same walk as a real capture without writing.
That is what keeps a preview and a capture from disagreeing.
`tribios check-ignore <path>` explains one path: the deciding pattern and its line, an excluded parent directory, or the fixed rule that applied.
It does not need a configured Project, so a policy can be checked before the first capture.
After capture, `configure` reports the excluded entry count, excluded logical bytes, and the time spent evaluating the policy.

`.tribiosignore` reduces what Workspaces receive.
It is not a security boundary, and the CLI says so whenever a policy excluded something.

A Git-tracked file can be excluded.
The Workspace's linked worktree still has the file in its index, so `git status` inside that Workspace reports it as deleted.
That is the honest view of a Workspace that lacks the file, and it is why excluding tracked files is a deliberate user choice rather than a default.

Pattern grammar stays in tier 0.
Filesystem capture, dry-run and explanation cases run in the design tier alongside the CLI acceptance test.
The capture integration cases pass an already-loaded policy to the capture boundary so deletion and replacement between loading and traversal can be tested deterministically without adding a pause hook to the CLI.
