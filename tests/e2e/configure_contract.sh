#!/usr/bin/env bash
# Designed `configure` reporting: Git-only Projects, one immutable capture, and two-phase removal.
source "$(dirname "$0")/lib.sh"

plain_project="$(mktemp -d)"
if plain_error="$("$TRIBIOS_BIN" configure "$plain_project" 2>&1)"; then
  rm -rf "$plain_project"
  fail "configuring a non-Git Project must fail"
fi
rm -rf "$plain_project"
assert_contains "is not a Git Project" "$plain_error"

make_project
configure_project

grep -q "may include secrets" "$WORK/configure.err" ||
  fail "configure must warn that captured ignored files may include secrets"
grep -q "^base state:" "$WORK/configure.out" || fail "configure must report the Base state size"
grep -q "^storage backend:" "$WORK/configure.out" || fail "configure must report the backend"

# The Base state is captured once and is immutable.
if "$TRIBIOS_BIN" configure "$PROJECT" >/dev/null 2>&1; then
  fail "configuring an already configured Project must not recapture the Base state"
fi

start_daemon
tribios workspace create untouched >/dev/null
tribios workspace create doomed >/dev/null

assert_contains "writable remaining bytes:" "$(tribios workspace status untouched)"

# Logical removal makes the Workspace disappear and is reported separately from
# physical reclamation.
removal="$(tribios workspace remove doomed)"
assert_contains "logically in" "$removal"
assert_gone doomed docs/notes.txt
tribios workspace wait-reclaim
after="$(tribios workspace list | grep '^doomed')"
assert_contains "reclaimed" "$after"
reclaim_us="$(echo "$after" | awk -F'\t' '{print $6}')"
[ "$reclaim_us" -ge 0 ] || fail "physical reclamation time must be recorded separately"

stop_daemon
echo "PASS configure contract"
