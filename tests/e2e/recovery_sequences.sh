#!/usr/bin/env bash
# Seeded operation sequences compare recovered behavior with a small model.
source "$(dirname "$0")/lib.sh"

make_project
configure_project
start_daemon
tribios workspace create generated >/dev/null

for seed in 100 201 302 403 500 601 702 803; do
  export TRIBIOS_FAULT_SEED="$seed"
  path="sequence-$seed.txt"
  model="seed-$seed"
  ws_write generated "$path" "$model"

  case $((seed % 4)) in
    0) boundary=write.after_journal; expected="$model"; expected_mode=644 ;;
    1) boundary=write.after_publish; expected="next-$seed"; expected_mode=644 ;;
    2) boundary=truncate.after_stage_flush; expected="$model"; expected_mode=644 ;;
    3) boundary=chmod.before_publish; expected="$model"; expected_mode=600 ;;
  esac

  stop_daemon
  export TRIBIOS_FAILPOINT="$boundary"
  start_daemon
  case "$boundary" in
    write.*) tribios fs write generated "$path" "next-$seed" 0 >/dev/null 2>&1 || true ;;
    truncate.*) tribios fs truncate generated "$path" 0 >/dev/null 2>&1 || true ;;
    chmod.*) tribios fs chmod generated "$path" 600 >/dev/null 2>&1 || true ;;
  esac
  unset TRIBIOS_FAILPOINT
  start_daemon
  assert_eq "$expected" "$(ws_read generated "$path")" "seed $seed contents"
  assert_eq "$expected_mode" "$(ws_stat generated "$path" | awk '{print $2}')" \
    "seed $seed mode"
  assert_contains "seed=$seed" "$(cat "$PROJECT/.tribios/recovery.trace")"
done
unset TRIBIOS_FAULT_SEED

assert_contains "pending operations: 0" "$(tribios recovery inspect)"
stop_daemon
echo "PASS recovery sequences"
