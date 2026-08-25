# Shared harness for the end-to-end tests.
#
# Tests observe behavior through the external seam. With a FUSE backend that
# seam is the mounted Workspace path driven by ordinary shell tools; without one
# it is `tribios fs`, which reaches the same engine the callbacks call. The
# ws_* helpers below pick whichever exists, so one test body covers both.

set -euo pipefail

TRIBIOS_BIN="${TRIBIOS_BIN:?TRIBIOS_BIN must point at the tribios CLI}"
export TRIBIOS_DAEMON="${TRIBIOS_DAEMON:?TRIBIOS_DAEMON must point at the daemon}"

SKIP_EXIT_CODE=77
WORK=""
PROJECT=""
MOUNT=""
MOUNTED=0

fail() { echo "FAIL: $*" >&2; exit 1; }

# A skip reports success, which is right on a laptop without a FUSE backend and
# wrong as a gate. CI sets TRIBIOS_REQUIRE_MOUNT=1 to turn every skip into a
# failure, including a missing build tool: on a runner that is a broken runner.
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
  "$TRIBIOS_BIN" configure "$PROJECT" "$@" > "$WORK/configure.out" 2> "$WORK/configure.err"
  MOUNT="$PROJECT/.tribios/mnt"
}

start_daemon() {
  tribios daemon start >/dev/null
  local info
  info="$(tribios info)"
  case "$info" in
    *"mount backend: mounted"*) MOUNTED=1 ;;
    *) MOUNTED=0 ;;
  esac
}

stop_daemon() { tribios daemon stop >/dev/null; }

require_mount() {
  [ "$MOUNTED" = "1" ] || skip "this build has no FUSE backend, so mounted-path tools cannot run"
}

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
  if [ "$MOUNTED" = "1" ]; then
    mkdir -p "$(dirname "$MOUNT/$1/$2")"
    printf '%s' "$3" > "$MOUNT/$1/$2"
  else
    tribios fs write "$1" "$2" "$3" 0 >/dev/null
  fi
}

ws_read() {
  if [ "$MOUNTED" = "1" ]; then cat "$MOUNT/$1/$2"; else tribios fs read "$1" "$2"; fi
}

ws_ls() {
  if [ "$MOUNTED" = "1" ]; then ls -A "$MOUNT/$1/${2:-}" | sort; else tribios fs ls "$1" "${2:-}" | sort; fi
}

ws_exists() {
  if [ "$MOUNTED" = "1" ]; then
    [ -e "$MOUNT/$1/$2" ] || [ -L "$MOUNT/$1/$2" ]
  else
    tribios fs stat "$1" "$2" >/dev/null 2>&1
  fi
}

file_mode() { stat -f '%Lp' "$1"; }

ws_stat() { # workspace path -> "<kind> <octal mode>"
  if [ "$MOUNTED" = "1" ]; then
    local kind
    if [ -L "$MOUNT/$1/$2" ]; then kind=symlink
    elif [ -d "$MOUNT/$1/$2" ]; then kind=dir
    else kind=file; fi
    echo "$kind $(file_mode "$MOUNT/$1/$2")"
  else
    tribios fs stat "$1" "$2" | head -1
  fi
}

ws_layer() { # answers upper or base, so tests can assert what was copied up
  tribios fs stat "$1" "$2" | sed -n 3p
}

ws_mkdir() { if [ "$MOUNTED" = "1" ]; then mkdir "$MOUNT/$1/$2"; else tribios fs mkdir "$1" "$2" >/dev/null; fi; }
ws_rm() { if [ "$MOUNTED" = "1" ]; then rm "$MOUNT/$1/$2"; else tribios fs rm "$1" "$2" >/dev/null; fi; }
ws_rmdir() { if [ "$MOUNTED" = "1" ]; then rmdir "$MOUNT/$1/$2"; else tribios fs rmdir "$1" "$2" >/dev/null; fi; }
ws_mv() { if [ "$MOUNTED" = "1" ]; then mv "$MOUNT/$1/$2" "$MOUNT/$1/$3"; else tribios fs mv "$1" "$2" "$3" >/dev/null; fi; }
ws_symlink() { if [ "$MOUNTED" = "1" ]; then ln -s "$2" "$MOUNT/$1/$3"; else tribios fs symlink "$1" "$2" "$3" >/dev/null; fi; }
ws_readlink() { if [ "$MOUNTED" = "1" ]; then readlink "$MOUNT/$1/$2"; else tribios fs readlink "$1" "$2"; fi; }
ws_chmod() { # workspace path mode
  if [ "$MOUNTED" = "1" ]; then chmod "$3" "$MOUNT/$1/$2"; else tribios fs chmod "$1" "$2" "$3" >/dev/null; fi
}
ws_truncate_to_empty() { # workspace path
  if [ "$MOUNTED" = "1" ]; then : > "$MOUNT/$1/$2"; else tribios fs truncate "$1" "$2" 0 >/dev/null; fi
}
ws_rm_rf() { if [ "$MOUNTED" = "1" ]; then rm -rf "$MOUNT/$1/$2"; else ws_remove_tree "$1" "$2"; fi; }

# What `rm -rf` drives through the mounted path, one call at a time.
ws_remove_tree() {
  local ws="$1" path="$2" entry
  if tribios fs stat "$ws" "$path" 2>/dev/null | head -1 | grep -q '^dir'; then
    for entry in $(tribios fs ls "$ws" "$path"); do
      ws_remove_tree "$ws" "$path/$entry"
    done
    tribios fs rmdir "$ws" "$path" >/dev/null
  else
    tribios fs rm "$ws" "$path" >/dev/null
  fi
}
