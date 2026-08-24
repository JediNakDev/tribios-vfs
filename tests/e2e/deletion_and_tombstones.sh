#!/usr/bin/env bash
# A removed Base-state path stays absent, including across a daemon restart.
source "$(dirname "$0")/lib.sh"

make_project
configure_project
start_daemon
tribios workspace create del >/dev/null

ws_rm del docs/notes.txt
ws_rm_rf del vendor

stop_daemon
start_daemon

ws_exists del docs/notes.txt && fail "a tombstoned file must stay absent after restart"
ws_exists del vendor && fail "a tombstoned directory must stay absent after restart"
assert_contains "docs" "$(ws_ls del)"

# Re-creating a removed directory must not resurrect its Base-state children.
ws_mkdir del vendor
assert_eq "" "$(ws_ls del vendor)" "a re-created directory must start empty"
ws_write del vendor/fresh.txt "fresh"
assert_eq "fresh.txt" "$(ws_ls del vendor)" "only the new child is visible"

stop_daemon
start_daemon
assert_eq "fresh.txt" "$(ws_ls del vendor)" "the re-created directory survives restart"

stop_daemon
echo "PASS deletion and tombstones"
