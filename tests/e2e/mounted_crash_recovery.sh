#!/usr/bin/env bash
# Mounted macFUSE and Linux FUSE calls use the engine recovery contract.
source "$(dirname "$0")/lib.sh"

crash_mounted() {
  local failpoint="$1"
  shift
  stop_daemon
  export TRIBIOS_FAILPOINT="$failpoint"
  start_daemon
  "$@" >/dev/null 2>&1 || true
  tribios daemon status >/dev/null 2>&1 && fail "$failpoint did not crash the mounted daemon"
  unset TRIBIOS_FAILPOINT
  start_daemon
}

mounted_write_without_truncation() {
  printf '%s' "$3" | dd of="$MOUNT/$1/$2" conv=notrunc 2>/dev/null
}

make_project
configure_project
start_daemon
require_mount
tribios workspace create mounted-recovery >/dev/null

for boundary in after_journal after_stage_flush before_publish after_publish after_metadata_commit; do
  path="mounted-$boundary.txt"
  ws_write mounted-recovery "$path" old
  crash_mounted "write.$boundary" mounted_write_without_truncation \
    mounted-recovery "$path" new
  case "$boundary" in
    before_publish|after_publish|after_metadata_commit) expected=new ;;
    *) expected=old ;;
  esac
  assert_eq "$expected" "$(ws_read mounted-recovery "$path")" "mounted write.$boundary"
done

crash_mounted mkdir.after_publish ws_mkdir mounted-recovery recovered-directory
assert_eq dir "$(ws_stat mounted-recovery recovered-directory | awk '{print $1}')" \
  "mounted mkdir recovery"
ws_write mounted-recovery recovered-directory/file content
crash_mounted rename.after_publish ws_mv mounted-recovery recovered-directory recovered-name
assert_eq content "$(ws_read mounted-recovery recovered-name/file)" "mounted rename recovery"
crash_mounted unlink.after_upper_remove ws_rm mounted-recovery recovered-name/file
ws_exists mounted-recovery recovered-name/file && fail "mounted unlink recovery"
crash_mounted rmdir.after_upper_remove ws_rmdir mounted-recovery recovered-name
ws_exists mounted-recovery recovered-name && fail "mounted rmdir recovery"

assert_contains "pending operations: 0" "$(tribios recovery inspect)"
stop_daemon
echo "PASS mounted crash recovery"
