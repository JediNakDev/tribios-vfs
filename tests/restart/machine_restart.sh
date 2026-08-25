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
  tribios daemon start --no-mount >/dev/null
  tribios workspace create restart-durable >/dev/null
  tribios fs write restart-durable durable.txt machine-restart 0 >/dev/null
  tribios fs fsync restart-durable durable.txt >/dev/null
  tribios fs fsyncdir restart-durable "" >/dev/null
  printf 'READY_TO_REBOOT %s\n' "$project"
  exit 0
fi

if [ "$phase" = verify ]; then
  tribios daemon start --no-mount >/dev/null
  actual="$(tribios fs read restart-durable durable.txt)"
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
