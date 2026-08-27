#!/usr/bin/env bash
# A Workspace's Git state survives Git maintenance run in the Project while the
# Workspace is detached, and a Workspace opens without re-reading every file.
source "$(dirname "$0")/lib.sh"

make_project
configure_project
start_daemon

tribios workspace create durable >/dev/null
workspace="$(ws_path durable)"
admin_dir="$(sed 's/^gitdir: //' "$workspace/.git")"
[ -d "$admin_dir" ] || fail "the Workspace has no linked-worktree administrative directory"

# The index is seeded from the Project's, whose cached size and modification
# time still describe the copied files, so Git reports a clean tree without
# hashing anything. An index built by `read-tree` has zeroed stat data and
# would report every tracked file as modified until it re-read the whole tree.
assert_eq "" "$(git -C "$workspace" status --porcelain)" "Workspace opens clean"

# Stopping the daemon detaches every Workspace, which leaves each linked
# worktree pointing at a path that no longer exists. `git worktree prune` then
# deletes the branch, HEAD and index of every Workspace unless it is locked,
# and a manual prune ignores the grace period that protects a fresh worktree.
stop_daemon
git -C "$PROJECT" worktree prune --verbose
[ -d "$admin_dir" ] || fail "git worktree prune deleted the detached Workspace's Git state"

start_daemon
assert_contains "durable" "$(git -C "$PROJECT" worktree list)"
assert_eq "" "$(git -C "$workspace" status --porcelain)" "Workspace still clean after prune"

# Removal has to unlock first: `git worktree remove` refuses a locked worktree
# even with --force.
tribios workspace remove durable >/dev/null
tribios workspace wait-reclaim
[ ! -d "$admin_dir" ] || fail "removal left the linked-worktree administrative directory behind"

stop_daemon
echo "PASS Git state durability"
