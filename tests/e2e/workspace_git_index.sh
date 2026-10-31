#!/usr/bin/env bash
# A new Workspace's Git index already describes its unchanged Base-state files
# the way the mount reports them, so Git's first status trusts them without
# rereading every file. It never vouches for a file that differs from HEAD.
# docs/adr/0009-seed-workspace-git-index.md records the decision.
source "$(dirname "$0")/lib.sh"

make_project
# Uncommitted when configure runs: the Base state holds content HEAD lacks.
printf 'uncommitted edit\n' >> "$PROJECT/docs/list.txt"
configure_project
# Committed after configure: HEAD moves past the commit the Base state was
# hashed against, so this file's Base content is stale relative to HEAD.
printf 'committed after configure\n' > "$PROJECT/docs/notes.txt"
git -C "$PROJECT" commit --quiet -m "after configure" -- docs/notes.txt

start_daemon
tribios workspace create seeded >/dev/null
require_mount
ws="$(ws_path seeded)"

index_line() { git -C "$ws" ls-files --debug -- "$1" | sed -n "/$2:/p"; }
assert_contains "mtime: 0:0" "$(index_line docs/notes.txt mtime)"
assert_contains "mtime: 0:0" "$(index_line docs/list.txt mtime)"
case "$(index_line src/main.cpp mtime)" in
  *"mtime: 0:0"*) fail "an unchanged Base-state file must start with stat data" ;;
esac

seeded_index="$(git -C "$ws" ls-files --debug)"
assert_eq " M docs/list.txt
 M docs/notes.txt" "$(git -C "$ws" status --porcelain)" \
  "only the files that differ from HEAD are modified"
# Git writes back every entry it had to reread and found unchanged, so an
# untouched index means the seeded stat data matched what the mount reports.
assert_eq "$seeded_index" "$(git -C "$ws" ls-files --debug)" \
  "the first status must not rewrite the seeded index"

# Workspace inode numbers are stable, so the index stays valid across a
# daemon restart.
stop_daemon
start_daemon
assert_eq " M docs/list.txt
 M docs/notes.txt" "$(git -C "$ws" status --porcelain)" "status after a restart"
assert_eq "$seeded_index" "$(git -C "$ws" ls-files --debug)" \
  "a restart must not invalidate the seeded index"

stop_daemon
echo "PASS workspace git index"
