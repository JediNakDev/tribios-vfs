#!/usr/bin/env bash
# Git works normally inside a Workspace through linked-worktree metadata.
source "$(dirname "$0")/lib.sh"

make_project
configure_project
start_daemon
tribios workspace create gitone >/dev/null
tribios workspace create gittwo >/dev/null
one="$(ws_path gitone)"
two="$(ws_path gittwo)"

# Linked-worktree registration is observable without a mounted path: each
# Workspace has its own branch and administrative state, and exposes only a
# `.git` file rather than a copy of the repository.
worktrees="$(git -C "$PROJECT" worktree list)"
assert_contains "$one" "$worktrees"
assert_contains "$two" "$worktrees"
assert_contains "gitone" "$(git -C "$PROJECT" branch --list gitone)"
assert_contains "gitdir:" "$(ws_read gitone .git)"
assert_eq "file" "$(ws_stat gitone .git | awk '{print $1}')" ".git must be a linked-worktree pointer file"

require_mount

assert_eq "gitone" "$(git -C "$one" rev-parse --abbrev-ref HEAD)" "branch identity"
assert_eq "gittwo" "$(git -C "$two" rev-parse --abbrev-ref HEAD)" "sibling branch identity"

# Shared object database and refs, private HEAD and index.
assert_eq "$(git -C "$PROJECT" rev-parse HEAD)" "$(git -C "$one" rev-parse HEAD)" "shared objects"

printf 'workspace one change\n' > "$one/docs/notes.txt"
status="$(git -C "$one" status --porcelain)"
assert_contains "docs/notes.txt" "$status"
assert_contains "workspace one change" "$(git -C "$one" diff)"
assert_eq "" "$(git -C "$two" status --porcelain)" "sibling working tree must be clean"

git -C "$one" add docs/notes.txt
assert_eq "" "$(git -C "$two" status --porcelain)" "the index must be private to a Workspace"
git -C "$one" -c user.email=a@b.invalid -c user.name=tribios commit --quiet -m "workspace one commit"

assert_eq "$(git -C "$PROJECT" rev-parse HEAD)" "$(git -C "$two" rev-parse HEAD)" "sibling HEAD is independent"
git -C "$PROJECT" cat-file -e "$(git -C "$one" rev-parse HEAD)" ||
  fail "the Workspace commit must land in the shared object database"

stop_daemon
echo "PASS git workflow"
