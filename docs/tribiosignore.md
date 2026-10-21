# `.tribiosignore`

`.tribiosignore` keeps chosen paths out of the Base state that `tribios configure` captures, and so out of every Workspace created from it.
It is a convenience for keeping secrets, caches, and machine-specific files out of Workspaces.
It is not a security boundary.
ADR 0007 records why it works the way it does.

## When it applies

Tribios reads the file once, at the Project root, when it captures a new Base state with `tribios configure` or `tribios configure --force`.
Editing it later changes nothing until the next capture.
Existing Workspaces never change.

Without a `.tribiosignore`, Tribios captures every regular file, directory, and symlink, including Git-ignored and untracked content.

`.gitignore` is never consulted.
A path can be ignored by Git and captured, excluded by Tribios and tracked by Git, or both.
Excluding a tracked file makes it appear as deleted in `git status` inside the Workspace.

## Checking a policy

```sh
tribios configure . --dry-run      # what a capture would include and exclude, writing nothing
tribios check-ignore cache/a.bin   # the decision for one path and the pattern that made it
```

`check-ignore` takes a Project-relative path, or an absolute path inside the Project.
It prints `included` or `excluded`, then the reason, such as `.tribiosignore:4: cache/, matched parent directory cache`.

## Syntax

Each line holds one pattern.

- Blank lines are ignored.
- A line starting with `#` is a comment. Write `\#` for a name that starts with `#`.
- Trailing spaces are ignored unless escaped with `\`.
- A leading `!` re-includes what an earlier pattern excluded. Write `\!` for a name that starts with `!`.
- A trailing `/` matches directories only. A symlink to a directory is not a directory.
- A pattern with no `/` except a trailing one matches a name at any depth: `secret.env` matches `secret.env` and `config/secret.env`.
- A pattern with a leading or inner `/` matches from the Project root: `/build` and `docs/drafts` match only there.
- `*` matches anything except `/`, and `?` matches one character except `/`.
- `[abc]`, `[a-z]`, and `[!a-z]` (or `[^a-z]`) match one character from, or not from, the set.
- `**` as a whole segment matches across directories: `**/fixtures`, `logs/**`, and `a/**/b`.
  Elsewhere it behaves like `*`.
- `\` makes the next character literal.

The last matching pattern decides.
Tribios never descends into an excluded directory, so a negation cannot re-include something inside it.
To keep one file from an excluded directory, exclude the directory's contents instead:

```gitignore
build/*
!build/keep.txt
```

## Always applied

These rules come before the policy, and no pattern overrides them:

- `.git` and `.tribios` at the Project root are never captured.
- Special files such as sockets and FIFOs, and nested mounts, are never captured.
- Symlinks are matched by their own path and are never followed.
- `.tribiosignore` itself is always captured, as the exact bytes that shaped the capture.

## Errors

Capture stops before writing anything, reporting the line, when the file:

- is unreadable or is not a regular file;
- contains a NUL byte or invalid UTF-8;
- has an unterminated `[` class, ends a pattern with a lone `\`, or has a pattern that names no path or contains an empty segment such as `a//b`.

## Differences from `.gitignore`

- Only one file, at the Project root. Nested `.tribiosignore` files, `.git/info/exclude`, and global excludes are not read.
- Matching is case-sensitive on every host, whatever the filesystem or `core.ignorecase` says.
- POSIX classes such as `[[:alpha:]]` are not supported.
- Malformed patterns are errors rather than patterns that silently match nothing.
