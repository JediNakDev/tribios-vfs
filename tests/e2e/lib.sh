# Shared harness for the end-to-end tests.
#
# Tests observe behavior through the external seam: the CLI, native Workspace
# paths, and ordinary filesystem tools.

set -Eeuo pipefail

report_unexpected_command_failure() {
  local status="$?"
  echo "FAIL: ${BASH_SOURCE[1]}:${BASH_LINENO[0]}: [$BASH_COMMAND] exited $status" >&2
  exit "$status"
}
trap report_unexpected_command_failure ERR

TRIBIOS_BIN="${TRIBIOS_BIN:?TRIBIOS_BIN must point at the tribios CLI}"
export TRIBIOS_DAEMON="${TRIBIOS_DAEMON:?TRIBIOS_DAEMON must point at the daemon}"

SKIP_EXIT_CODE=77
WORK=""
PROJECT=""
MOUNT=""
MOUNTED=1

fail() { echo "FAIL: $*" >&2; exit 1; }

# A skip reports success on a host without the required native storage
# capability and becomes a failure when CI requires that backend.
skip() {
  if [ "${TRIBIOS_REQUIRE_MOUNT:-}" = "1" ]; then
    fail "TRIBIOS_REQUIRE_MOUNT is set, so skipping is not acceptable: $*"
  fi
  echo "SKIP: $*" >&2
  exit "$SKIP_EXIT_CODE"
}

assert_eq() {
  local expected="$1" actual="$2" what="${3:-value}"
  [ "$expected" = "$actual" ] || fail "$what: expected [$expected], got [$actual]"
}

assert_contains() {
  case "$2" in
    *"$1"*) ;;
    *) fail "expected [$2] to contain [$1]" ;;
  esac
}

tribios() { "$TRIBIOS_BIN" --project "$PROJECT" "$@"; }

cleanup() {
  if [ -n "$PROJECT" ] && [ -d "$PROJECT" ]; then
    "$TRIBIOS_BIN" --project "$PROJECT" daemon start >/dev/null 2>&1 || true
    while IFS=$'\t' read -r name state _; do
      [ "$state" = active ] || continue
      "$TRIBIOS_BIN" --project "$PROJECT" workspace remove "$name" >/dev/null 2>&1 || true
    done < <("$TRIBIOS_BIN" --project "$PROJECT" workspace list 2>/dev/null | tail -n +2)
    "$TRIBIOS_BIN" --project "$PROJECT" workspace wait-reclaim >/dev/null 2>&1 || true
    "$TRIBIOS_BIN" --project "$PROJECT" daemon stop >/dev/null 2>&1 || true
  fi
  [ -n "$WORK" ] && rm -rf "$WORK" 2>/dev/null || true
}
trap cleanup EXIT

# Creates a Git Project that builds with CMake and Ninja and contains ignored
# and untracked content that a Workspace needs in order to build.
make_project() {
  WORK="$(mktemp -d)"
  PROJECT="$WORK/project"
  mkdir -p "$PROJECT/src" "$PROJECT/vendor/dep" "$PROJECT/docs"

  cat > "$PROJECT/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.20)
project(sample CXX)
set(CMAKE_CXX_STANDARD 17)
include_directories(vendor/dep)
add_executable(sample src/main.cpp)
enable_testing()
add_test(NAME sample_runs COMMAND sample)
EOF
  cat > "$PROJECT/src/main.cpp" <<'EOF'
#include <iostream>
#include "answer.h"
int main() {
  std::cout << "answer=" << tribios_answer() << "\n";
  return tribios_answer() == 42 ? 0 : 1;
}
EOF
  # Ignored dependency content: a Workspace must be able to build without
  # reinstalling or reconstructing it.
  cat > "$PROJECT/vendor/dep/answer.h" <<'EOF'
#pragma once
inline int tribios_answer() { return 42; }
EOF
  printf 'vendor/\nlocal.env\nbuild/\n' > "$PROJECT/.gitignore"
  printf 'SECRET_TOKEN=untracked-and-ignored\n' > "$PROJECT/local.env"
  printf 'shared base content\n' > "$PROJECT/docs/notes.txt"
  printf 'one\ntwo\nthree\n' > "$PROJECT/docs/list.txt"
  ln -s ../src/main.cpp "$PROJECT/docs/main.link"

  git -C "$PROJECT" init --quiet --initial-branch=main
  git -C "$PROJECT" config user.email tribios@example.invalid
  git -C "$PROJECT" config user.name "Tribios Prototype"
  git -C "$PROJECT" add -A
  git -C "$PROJECT" commit --quiet -m "initial commit"
}

configure_project() {
  if ! "$TRIBIOS_BIN" configure "$PROJECT" "$@" > "$WORK/configure.out" 2> "$WORK/configure.err"; then
    if grep -q "no supported Workspace storage backend" "$WORK/configure.err"; then
      skip "this host cannot provide a native Workspace storage backend"
    fi
    cat "$WORK/configure.err" >&2
    fail "Project configuration failed"
  fi
  MOUNT="$PROJECT/.tribios/mnt"
}

start_daemon() {
  local start_output
  if ! start_output="$(tribios daemon start 2>&1)"; then
    echo "$start_output" >&2
    if [ -f "$PROJECT/.tribios/daemon.log" ]; then
      echo "daemon log:" >&2
      sed -n '1,240p' "$PROJECT/.tribios/daemon.log" >&2
    fi
    fail "the daemon did not start"
  fi
  assert_contains "storage backend:" "$(tribios info)"
}

stop_daemon() { tribios daemon stop >/dev/null; }

require_mount() { :; }

ws_path() { echo "$MOUNT/$1"; }

# A removed Workspace stops answering at once, but the kernel may hold its
# cached directory entry until the mount's entry timeout expires.
assert_gone() { # workspace [path]
  local deadline=$((SECONDS + 5))
  while ws_exists "$1" "${2:-}" && [ "$SECONDS" -lt "$deadline" ]; do sleep 0.2; done
  ! ws_exists "$1" "${2:-}" || fail "$1/${2:-} is still visible after removal"
}

# --- Workspace filesystem operations through the available seam -------------

ws_write() { # workspace path data
  mkdir -p "$(dirname "$MOUNT/$1/$2")"
  printf '%s' "$3" > "$MOUNT/$1/$2"
}

ws_read() { cat "$MOUNT/$1/$2"; }

ws_ls() { ls -A "$MOUNT/$1/${2:-}" | sort; }

ws_exists() {
  [ -e "$MOUNT/$1/$2" ] || [ -L "$MOUNT/$1/$2" ]
}

# GNU stat and BSD stat spell "the permission bits" differently.
if stat --version >/dev/null 2>&1; then
  file_mode() { stat -c '%a' "$1"; }
else
  file_mode() { stat -f '%Lp' "$1"; }
fi

ws_stat() { # workspace path -> "<kind> <octal mode>"
  local kind
  if [ -L "$MOUNT/$1/$2" ]; then kind=symlink
  elif [ -d "$MOUNT/$1/$2" ]; then kind=dir
  else kind=file; fi
  echo "$kind $(file_mode "$MOUNT/$1/$2")"
}

ws_mkdir() { mkdir "$MOUNT/$1/$2"; }
ws_rm() { rm "$MOUNT/$1/$2"; }
ws_rmdir() { rmdir "$MOUNT/$1/$2"; }
ws_mv() { mv "$MOUNT/$1/$2" "$MOUNT/$1/$3"; }
ws_symlink() { ln -s "$2" "$MOUNT/$1/$3"; }
ws_readlink() { readlink "$MOUNT/$1/$2"; }
ws_chmod() { chmod "$3" "$MOUNT/$1/$2"; }
ws_truncate_to_empty() { : > "$MOUNT/$1/$2"; }
ws_rm_rf() { rm -rf "$MOUNT/$1/$2"; }
