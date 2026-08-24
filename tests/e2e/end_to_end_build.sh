#!/usr/bin/env bash
# The primary end-to-end test: two Workspaces from one Base state, independent
# mutation, build and test with unmodified tools, restart, commit, remove.
source "$(dirname "$0")/lib.sh"

command -v cmake >/dev/null || skip "cmake is not installed"
command -v ninja >/dev/null || skip "ninja is not installed"

make_project
configure_project
start_daemon
require_mount
tribios workspace create build-a >/dev/null
tribios workspace create build-b >/dev/null
a="$(ws_path build-a)"
b="$(ws_path build-b)"

# Independent mutation of the same Base-state file.
sed -i.bak 's/return 42;/return 7;/' "$b/vendor/dep/answer.h" && rm -f "$b/vendor/dep/answer.h.bak"
assert_contains "42" "$(cat "$a/vendor/dep/answer.h")"

# Unmodified CMake, Ninja, compiler and test runner in each Workspace.
cmake -S "$a" -B "$a/build" -G Ninja >/dev/null
ninja -C "$a/build" >/dev/null
assert_contains "answer=42" "$("$a/build/sample")"
ctest --test-dir "$a/build" >/dev/null || fail "tests must pass in the first Workspace"

cmake -S "$b" -B "$b/build" -G Ninja >/dev/null
ninja -C "$b/build" >/dev/null
assert_contains "answer=7" "$("$b/build/sample")"

# Restart, then verify persistence and isolation.
stop_daemon
start_daemon
assert_contains "42" "$(cat "$a/vendor/dep/answer.h")"
assert_contains "7" "$(cat "$b/vendor/dep/answer.h")"
assert_contains "42" "$(cat "$PROJECT/vendor/dep/answer.h")"
[ -x "$a/build/sample" ] || fail "build output must survive a restart"

# Independent commits. The dependency change is ignored by Git, so the commit
# carries a tracked change made in this Workspace only.
printf '// changed in build-b\n' >> "$b/src/main.cpp"
[ "$(git -C "$a" status --porcelain)" = "" ] || fail "the sibling Workspace must stay clean"
git -C "$b" add -A
git -C "$b" -c user.email=a@b.invalid -c user.name=tribios commit --quiet -m "answer is seven"
[ "$(git -C "$a" rev-parse HEAD)" != "$(git -C "$b" rev-parse HEAD)" ] ||
  fail "commits must be independent per Workspace"

# Removal.
tribios workspace remove build-a >/dev/null
tribios workspace remove build-b >/dev/null
tribios workspace wait-reclaim
[ -z "$(ls "$MOUNT")" ] || fail "the Project view must list no Workspaces after removal"
assert_gone build-a
assert_gone build-b

stop_daemon
echo "PASS end-to-end build"
