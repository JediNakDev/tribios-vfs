#!/usr/bin/env bash
# The Linux mount lets the kernel cache entries and attributes for a minute, so
# the daemon drops them itself whenever a Workspace changes without going
# through the mount. Every check reads right after the change, well inside that
# minute. docs/adr/0010-linux-kernel-cache.md records the decision.
source "$(dirname "$0")/lib.sh"

[ "$(uname -s)" = Linux ] || skip "only the Linux mount caches entries for longer than a second"

make_project
configure_project
start_daemon
tribios workspace create cached >/dev/null
require_mount
ws="$(ws_path cached)"

# Put every path the checks touch into the kernel's cache first.
stat "$ws" "$ws/docs" "$ws/docs/notes.txt" "$ws/docs/list.txt" "$ws/CMakeLists.txt" \
  "$ws/src/main.cpp" >/dev/null
cat "$ws/docs/notes.txt" >/dev/null

# Run from inside the Workspace: dropping cache entries must never detach a
# shell's working directory.
cd "$ws"
tribios fs write cached docs/notes.txt "rewritten outside the mount" 0 >/dev/null
assert_eq 27 "$(stat -c %s docs/notes.txt)" "size after fs write"
assert_eq "rewritten outside the mount" "$(cat docs/notes.txt)" "content after fs write"
assert_eq "$ws" "$(pwd -P)" "working directory after fs write"
cd "$WORK"

tribios fs chmod cached docs/list.txt 600 >/dev/null
assert_eq 600 "$(file_mode "$ws/docs/list.txt")" "mode after fs chmod"

tribios fs rm cached docs/list.txt >/dev/null
[ ! -e "$ws/docs/list.txt" ] || fail "a file removed outside the mount is still visible"
printf 'recreated' > "$ws/docs/list.txt" || fail "cannot recreate a file removed outside the mount"
assert_eq recreated "$(cat "$ws/docs/list.txt")" "recreated content"

tribios fs mv cached CMakeLists.txt src/main.cpp >/dev/null
[ ! -e "$ws/CMakeLists.txt" ] || fail "a file renamed away outside the mount is still visible"
assert_eq "cmake_minimum_required(VERSION 3.20)" "$(head -1 "$ws/src/main.cpp")" \
  "a rename target replaced outside the mount"
mkdir "$ws/CMakeLists.txt" || fail "cannot reuse a name renamed away outside the mount"

mkdir "$ws/scratch"
stat "$ws/scratch" >/dev/null
tribios fs rmdir cached scratch >/dev/null
[ ! -e "$ws/scratch" ] || fail "a directory removed outside the mount is still visible"

# Removing a Workspace and reusing its name shows the new Workspace at once.
tribios workspace remove cached >/dev/null
[ ! -e "$ws" ] || fail "a removed Workspace is still visible"
tribios workspace wait-reclaim
tribios workspace create cached --branch cached-again >/dev/null
assert_eq "shared base content" "$(cat "$ws/docs/notes.txt")" "a recreated Workspace's content"

stop_daemon
echo "PASS kernel cache coherence"
