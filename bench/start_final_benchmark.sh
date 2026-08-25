#!/usr/bin/env bash
set -euo pipefail

repository_root="$(git rev-parse --show-toplevel)"
if [[ "$PWD" != "$repository_root" ]]; then
  echo "run this command from the repository root: $repository_root" >&2
  exit 2
fi

benchmark_root="/Volumes/PortableSSD/tribios-vfs-benchmark"
mkdir -p "$benchmark_root"
launcher_log="$benchmark_root/launcher.log"

nohup "$repository_root/bench/run_final_benchmark.sh" "$@" \
  >> "$launcher_log" 2>&1 < /dev/null &
runner_pid=$!
printf '%s\n' "$runner_pid" > "$benchmark_root/runner.pid"

echo "Benchmark runner started as PID $runner_pid"
echo "Launcher log: $launcher_log"
echo "Run status: $benchmark_root/latest/status.txt"
echo "Follow progress: tail -f $benchmark_root/latest/run.log"
