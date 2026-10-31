#!/usr/bin/env bash
# Linux has no unprivileged forced unmount, so `daemon stop` unmounts before it
# agrees to stop. A mount that something briefly holds still stops, and one that
# stays busy refuses the stop and keeps serving, so the person can close what
# holds it and retry. docs/adr/0011-stop-only-after-unmounting.md records the
# decision.
source "$(dirname "$0")/lib.sh"

[ "$(uname -s)" = Linux ] || skip "only Linux refuses to stop a busy mount; macOS forces the unmount"

make_project
configure_project
start_daemon
tribios workspace create busy >/dev/null
require_mount
ws="$(ws_path busy)"

# A holder that leaves within the retry window does not stop the daemon from stopping.
( cd "$ws" && sleep 1 ) &
holder=$!
sleep 0.2
stop_daemon || fail "a briefly held mount must still stop"
wait "$holder"
[ ! -e "$ws" ] || fail "the mount is still present after the daemon stopped"

# A holder that stays makes the stop fail with the reason, and the daemon keeps serving.
start_daemon
( cd "$ws" && exec sleep 60 ) &
holder=$!
sleep 0.2
if out="$(tribios daemon stop 2>&1)"; then fail "stopping a busy mount must fail"; fi
assert_contains "busy" "$out"
assert_eq running "$(tribios daemon status)" "daemon state after a refused stop"
assert_eq "shared base content" "$(cat "$ws/docs/notes.txt")" "a Workspace read after a refused stop"

kill "$holder"
wait "$holder" 2>/dev/null || true
stop_daemon || fail "the stop must succeed once the mount is idle"
[ ! -e "$ws" ] || fail "the mount is still present after the daemon stopped"
echo "PASS daemon stop busy mount"
