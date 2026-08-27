#!/usr/bin/env bash
# Independent Workspace lifecycle operations and native I/O can overlap.
source "$(dirname "$0")/lib.sh"

make_project
configure_project
start_daemon

pids=()
for index in $(seq 1 8); do
  tribios workspace create "parallel-$index" >/dev/null &
  pids+=("$!")
done
for pid in "${pids[@]}"; do wait "$pid"; done

pids=()
for index in $(seq 1 8); do
  ws_write "parallel-$index" docs/notes.txt "workspace-$index" &
  pids+=("$!")
done
for pid in "${pids[@]}"; do wait "$pid"; done

pids=()
for index in 1 2 3 4; do
  tribios workspace remove "parallel-$index" >/dev/null &
  pids+=("$!")
done
for index in 5 6 7 8; do
  assert_eq "workspace-$index" "$(ws_read "parallel-$index" docs/notes.txt)" \
    "Workspace $index while siblings are removed"
done
for pid in "${pids[@]}"; do wait "$pid"; done
tribios workspace wait-reclaim

stop_daemon
echo "PASS concurrent Workspaces"
