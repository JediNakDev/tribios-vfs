#!/usr/bin/env bash
# I/O failures, short writes and repeated recovery crashes preserve invariants.
source "$(dirname "$0")/lib.sh"

restart_without_faults() {
  unset TRIBIOS_IO_FAULT TRIBIOS_SHORT_WRITE_BYTES
  stop_daemon
  start_daemon
}

make_project
configure_project
start_daemon
tribios workspace create faulted >/dev/null
tribios workspace create independent >/dev/null

# Every injected storage failure before publication reports ENOSPC, retains the
# old file after restart, and leaves no journal or temporary artifact.
for boundary in journal.write stage.flush journal.phase publish.rename publish.directory_flush journal.finish; do
  ws_write faulted durable.txt old
  stop_daemon
  export TRIBIOS_IO_FAULT="$boundary"
  start_daemon
  if output="$(tribios fs write faulted durable.txt new 0 2>&1)"; then
    fail "$boundary unexpectedly succeeded"
  fi
  assert_contains "errno 28" "$output"
  restart_without_faults
  assert_eq old "$(ws_read faulted durable.txt)" "$boundary old-state preservation"
  assert_contains "pending operations: 0" "$(tribios recovery inspect)"
  [ ! -d "$PROJECT/.tribios/workspaces/faulted/recovery" ] ||
    fail "$boundary leaked recovery artifacts"
done

# Lifecycle journal and persistence failures do not create a partially active
# Workspace or remove an existing one.
for boundary in lifecycle.journal.write lifecycle.state.write lifecycle.storage.create lifecycle.git_pointer.write lifecycle.active.write; do
  name="fault-${boundary//./-}"
  stop_daemon
  export TRIBIOS_IO_FAULT="$boundary"
  start_daemon
  tribios workspace create "$name" >/dev/null 2>&1 && fail "$boundary unexpectedly succeeded"
  restart_without_faults
  tribios fs stat "$name" docs/notes.txt >/dev/null 2>&1 &&
    fail "$boundary exposed a partial Workspace"
  git -C "$PROJECT" branch --list "$name" | grep -q . && fail "$boundary leaked a Git branch"
done

tribios workspace create removal-fault >/dev/null
for boundary in lifecycle.journal.write lifecycle.removed.write; do
  stop_daemon
  export TRIBIOS_IO_FAULT="$boundary"
  start_daemon
  tribios workspace remove removal-fault >/dev/null 2>&1 &&
    fail "$boundary unexpectedly removed the Workspace"
  restart_without_faults
  assert_contains $'removal-fault\tactive' "$(tribios workspace list)"
done

# A short write reports exactly the accepted prefix and publishes no bytes
# beyond that count.
ws_write faulted short.txt old
stop_daemon
export TRIBIOS_SHORT_WRITE_BYTES=3
start_daemon
assert_eq 3 "$(tribios fs write faulted short.txt abcdef 0)" "short-write count"
restart_without_faults
assert_eq abc "$(ws_read faulted short.txt)" "short-write prefix"

# Recovery itself can be killed before or after applying an operation and
# remains idempotent on the following startup.
for recovery_boundary in recovery.before_apply recovery.after_apply recovery.before_metadata_commit recovery.after_metadata_commit; do
  ws_write faulted repeated.txt old
  stop_daemon
  export TRIBIOS_FAILPOINT=write.before_publish
  start_daemon
  tribios fs write faulted repeated.txt new 0 >/dev/null 2>&1 || true
  tribios daemon status >/dev/null 2>&1 && fail "write.before_publish did not crash"
  export TRIBIOS_FAILPOINT="$recovery_boundary"
  if "$TRIBIOS_DAEMON" --project "$PROJECT" --no-mount >/dev/null 2>&1; then
    fail "$recovery_boundary did not crash recovery"
  fi
  unset TRIBIOS_FAILPOINT
  start_daemon
  assert_eq new "$(ws_read faulted repeated.txt)" "$recovery_boundary idempotence"
  assert_contains "pending operations: 0" "$(tribios recovery inspect)"
done

for recovery_boundary in recovery.before_prepared_rollback recovery.after_prepared_rollback; do
  ws_write faulted prepared-recovery.txt old
  stop_daemon
  export TRIBIOS_FAILPOINT=write.after_journal
  start_daemon
  tribios fs write faulted prepared-recovery.txt new 0 >/dev/null 2>&1 || true
  export TRIBIOS_FAILPOINT="$recovery_boundary"
  if "$TRIBIOS_DAEMON" --project "$PROJECT" --no-mount >/dev/null 2>&1; then
    fail "$recovery_boundary did not crash recovery"
  fi
  unset TRIBIOS_FAILPOINT
  start_daemon
  assert_eq old "$(ws_read faulted prepared-recovery.txt)" "$recovery_boundary rollback"
  assert_contains "pending operations: 0" "$(tribios recovery inspect)"
done

# An acknowledged mutation in another Workspace remains independent of the
# operation selected for interruption.
ws_write independent acknowledged.txt independent
stop_daemon
export TRIBIOS_FAILPOINT=write.after_publish
start_daemon
tribios fs write faulted durable.txt interrupted 0 >/dev/null 2>&1 || true
unset TRIBIOS_FAILPOINT
start_daemon
assert_eq independent "$(ws_read independent acknowledged.txt)" "independent Workspace"
assert_eq "shared base content" "$(cat "$PROJECT/docs/notes.txt")" "Project source isolation"

stop_daemon
echo "PASS recovery faults"
