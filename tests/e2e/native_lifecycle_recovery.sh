#!/usr/bin/env bash
# Interrupted native storage lifecycle operations recover idempotently.
source "$(dirname "$0")/lib.sh"

make_project
configure_project

start_with_failpoint() {
  local failpoint="$1"
  TRIBIOS_FAILPOINT="$failpoint" "$TRIBIOS_DAEMON" --project "$PROJECT" \
    >"$WORK/failpoint.log" 2>&1 &
  local daemon_pid=$!
  local deadline=$((SECONDS + 15))
  until tribios daemon status >/dev/null 2>&1; do
    [ "$SECONDS" -lt "$deadline" ] || fail "daemon did not start for $failpoint"
    sleep 0.05
  done
  echo "$daemon_pid"
}

wait_for_crash() {
  local daemon_pid="$1"
  wait "$daemon_pid" 2>/dev/null || true
  local deadline=$((SECONDS + 15))
  while tribios daemon status >/dev/null 2>&1 && [ "$SECONDS" -lt "$deadline" ]; do
    sleep 0.05
  done
  if tribios daemon status >/dev/null 2>&1; then
    fail "failpoint daemon stayed alive"
  fi
}

pid="$(start_with_failpoint workspace_create.after_storage)"
tribios workspace create rolled-back >/dev/null 2>&1 || true
wait_for_crash "$pid"
start_daemon
assert_contains $'rolled-back\treclaimed' "$(tribios workspace list)"
tribios workspace create rolled-back >/dev/null

stop_daemon
pid="$(start_with_failpoint workspace_create.after_active_commit)"
tribios workspace create durable >/dev/null 2>&1 || true
wait_for_crash "$pid"
start_daemon
assert_contains $'durable\tactive' "$(tribios workspace list)"
assert_eq "shared base content" "$(ws_read durable docs/notes.txt)" "active creation recovery"

stop_daemon
pid="$(start_with_failpoint workspace_remove.after_detach)"
tribios workspace remove durable >/dev/null 2>&1 || true
wait_for_crash "$pid"
start_daemon
assert_contains $'durable\tactive' "$(tribios workspace list)"
assert_eq "shared base content" "$(ws_read durable docs/notes.txt)" "detached active recovery"

stop_daemon
pid="$(start_with_failpoint workspace_remove.after_removed_commit)"
tribios workspace remove durable >/dev/null 2>&1 || true
wait_for_crash "$pid"
start_daemon
assert_contains $'durable\treclaimed' "$(tribios workspace list)"
assert_gone durable

stop_daemon
echo "PASS native lifecycle recovery"
