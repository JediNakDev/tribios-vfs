#!/usr/bin/env bash
# ADR 0011 permits shutdown when the attempted mount never became active.
source "$(dirname "$0")/lib.sh"
make_project
configure_project
rmdir "$MOUNT"
printf 'not a mount directory' > "$MOUNT"
start_daemon
if ! grep -q 'cannot prepare mount point' "$PROJECT/.tribios/daemon.log"; then
  skip "this regression requires a compiled FUSE backend"
fi
stop_daemon || fail "an already absent mount must permit shutdown"
if tribios daemon status >/dev/null 2>&1; then
  fail "the daemon is still serving after shutdown"
fi
rm "$MOUNT"
echo "PASS daemon stop after failed mount"
