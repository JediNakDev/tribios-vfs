#!/usr/bin/env bash
# Workspace changes survive a clean daemon restart.
source "$(dirname "$0")/lib.sh"

make_project
configure_project
start_daemon
tribios workspace create keep >/dev/null
tribios workspace create other >/dev/null

ws_write keep docs/notes.txt "persisted content"
ws_write keep created.txt "created before restart"
ws_mkdir keep persisted-dir
ws_chmod keep created.txt 600

stop_daemon
start_daemon

assert_contains "keep" "$(tribios workspace list)"
assert_eq "persisted content" "$(ws_read keep docs/notes.txt)" "copied-up file after restart"
assert_eq "created before restart" "$(ws_read keep created.txt)" "new file after restart"
assert_contains "persisted-dir" "$(ws_ls keep)"
assert_eq "600" "$(ws_stat keep created.txt | awk '{print $2}')" "permissions after restart"
assert_eq "shared base content" "$(ws_read other docs/notes.txt)" "sibling isolation after restart"

stop_daemon
echo "PASS restart persistence"
