#!/usr/bin/env bash
# Two-phase harness for a disposable host that an external controller reboots.
set -euo pipefail

TRIBIOS_BIN="${TRIBIOS_BIN:?TRIBIOS_BIN must point at the tribios CLI}"
export TRIBIOS_DAEMON="${TRIBIOS_DAEMON:?TRIBIOS_DAEMON must point at the daemon}"
phase="${1:?usage: machine_restart.sh prepare|verify PROJECT}"
project="${2:?usage: machine_restart.sh prepare|verify PROJECT}"

tribios() { "$TRIBIOS_BIN" --project "$project" "$@"; }

if [ "$phase" = prepare ]; then
  "$TRIBIOS_BIN" configure "$project" >/dev/null
  tribios daemon start >/dev/null
  tribios workspace create restart-durable >/dev/null
  printf 'machine-restart' > "$PROJECT/.tribios/mnt/restart-durable/durable.txt"
  python3 - "$PROJECT/.tribios/mnt/restart-durable/durable.txt" <<'PY'
import os
import sys
descriptor = os.open(sys.argv[1], os.O_RDONLY)
os.fsync(descriptor)
os.close(descriptor)
PY
  printf 'READY_TO_REBOOT %s\n' "$project"
  exit 0
fi

if [ "$phase" = verify ]; then
  tribios daemon start >/dev/null
  actual="$(cat "$PROJECT/.tribios/mnt/restart-durable/durable.txt")"
  [ "$actual" = machine-restart ] || {
    echo "expected durable machine-restart data, got [$actual]" >&2
    exit 1
  }
  tribios recovery inspect | grep -q 'pending operations: 0'
  tribios daemon stop >/dev/null
  echo "PASS machine restart"
  exit 0
fi

echo "unknown phase: $phase" >&2
exit 2
