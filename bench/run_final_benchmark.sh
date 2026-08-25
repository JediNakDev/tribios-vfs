#!/usr/bin/env bash
set -euo pipefail

repository_root="$(git rev-parse --show-toplevel)"
if [[ "$PWD" != "$repository_root" ]]; then
  echo "run this command from the repository root: $repository_root" >&2
  exit 2
fi

portable_ssd="/Volumes/PortableSSD"
if [[ ! -d "$portable_ssd" ]] || ! mount | grep "on $portable_ssd " >/dev/null; then
  echo "PortableSSD is not mounted at $portable_ssd" >&2
  exit 2
fi

unexpected_changes="$({
  git status --porcelain --untracked-files=all |
    sed 's/^...//' |
    grep -Ev '^(AGENTS\.md|AGENTS\.local\.md)$'
} || true)"
if [[ -n "$unexpected_changes" ]]; then
  echo "refusing to benchmark a source tree with uncommitted implementation changes:" >&2
  echo "$unexpected_changes" >&2
  exit 2
fi

run_id="$(date -u '+%Y%m%dT%H%M%SZ')-$$"
benchmark_root="$portable_ssd/tribios-vfs-benchmark"
run_directory="$benchmark_root/runs/$run_id"
build_directory="$run_directory/build"
fixture_path="$run_directory/fixture"
scratch_path="$run_directory/scratch"
temporary_path="$run_directory/tmp"
result_path="$run_directory/results.json"
ctest_output_path="$run_directory/ctest-output.txt"
status_path="$run_directory/status.txt"

mkdir -p "$build_directory" "$scratch_path" "$temporary_path"
ln -sfn "$run_directory" "$benchmark_root/latest"
exec > >(tee -a "$run_directory/run.log") 2>&1
export TMPDIR="$temporary_path"

daemon_started=0
run_completed=0
benchmark_exit_code=1

# shellcheck disable=SC2329  # Invoked through the EXIT trap.
finish_run() {
  exit_code=$?
  trap - EXIT
  set +e
  if [[ "$daemon_started" -eq 1 ]]; then
    "$build_directory/tribios" --project "$fixture_path" daemon stop
  fi
  if [[ "$run_completed" -eq 1 ]]; then
    printf 'COMPLETE\nbenchmark_exit_code=%s\n' "$benchmark_exit_code" > "$status_path"
  else
    printf 'FAILED\nscript_exit_code=%s\n' "$exit_code" > "$status_path"
  fi
  echo "Benchmark artifacts: $run_directory"
  exit "$exit_code"
}
trap finish_run EXIT

printf 'RUNNING\n' > "$status_path"
echo "Benchmark run: $run_id"
echo "Repository: $repository_root"
echo "Commit: $(git rev-parse HEAD)"
echo "Artifacts: $run_directory"
git status --short
sw_vers
system_profiler SPHardwareDataType
df -h "$portable_ssd"

llvm_prefix="$(brew --prefix llvm)"
sqlite_prefix="$(brew --prefix sqlite)"
export PKG_CONFIG_PATH="/usr/local/lib/pkgconfig:$sqlite_prefix/lib/pkgconfig"

cmake -S "$repository_root" -B "$build_directory" -G Ninja \
  -DCMAKE_CXX_COMPILER="$llvm_prefix/bin/clang++" \
  -DCMAKE_EXE_LINKER_FLAGS="-L$llvm_prefix/lib/c++ -Wl,-rpath,$llvm_prefix/lib/c++" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja -C "$build_directory"
ctest --test-dir "$build_directory" -L unit --output-on-failure --no-tests=error
grep -q TRIBIOS_HAVE_FUSE "$build_directory/compile_commands.json"

python3 "$repository_root/bench/generate_fixture.py" "$fixture_path" \
  --files 100000 --bytes 2147483648
"$build_directory/tribios" configure "$fixture_path"
"$build_directory/tribios" --project "$fixture_path" daemon start
daemon_started=1

set +e
TRIBIOS_REQUIRE_MOUNT=1 TMPDIR="$temporary_path" \
  python3 "$repository_root/bench/benchmark.py" \
    --cli "$build_directory/tribios" \
    --project "$fixture_path" \
    --scratch "$scratch_path" \
    --build-directory "$build_directory" \
    --output "$result_path"
benchmark_exit_code=$?
set -e

if [[ ! -f "$result_path" ]]; then
  echo "benchmark exited without writing $result_path" >&2
  exit "$benchmark_exit_code"
fi

python3 - "$result_path" "$ctest_output_path" <<'PY'
import json
import sys
from pathlib import Path

result_path = Path(sys.argv[1])
ctest_output_path = Path(sys.argv[2])
report = json.loads(result_path.read_text())
ctest_output_path.write_text(report["results"]["correctness"]["output"])
print(f"Final verdict eligible: {report['final_verdict_eligible']}")
print(f"Verdict: {'PASS' if report['passed'] else 'FAIL'}")
PY

run_completed=1
exit "$benchmark_exit_code"
