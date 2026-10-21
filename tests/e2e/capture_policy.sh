#!/usr/bin/env bash
# Designed `.tribiosignore` behavior from ADR 0007: validation before capture,
# dry run, explanation, and exclusions that only shape a new Base state.
source "$(dirname "$0")/lib.sh"

make_project
mkdir -p "$PROJECT/cache/nested"
printf 'disposable\n' > "$PROJECT/cache/nested/blob.bin"

# A malformed policy stops capture before anything is written.
printf 'ok.txt\n[unterminated\n' > "$PROJECT/.tribiosignore"
if bad_error="$("$TRIBIOS_BIN" configure "$PROJECT" 2>&1)"; then
  fail "configure must reject a malformed .tribiosignore"
fi
assert_contains ".tribiosignore:2: unterminated character class" "$bad_error"
[ ! -e "$PROJECT/.tribios/meta.db" ] || fail "a rejected policy must not leave a committed Base state"

# Excludes an untracked secret, a Git-tracked file and a whole directory, and
# re-includes one Git-ignored dependency file under the documented rules.
printf '# secrets\nlocal.env\ndocs/list.txt\ncache/\nvendor/*\n!vendor/dep/\n' > "$PROJECT/.tribiosignore"

dry_run="$("$TRIBIOS_BIN" configure "$PROJECT" --dry-run 2>/dev/null)"
assert_contains "dry run: nothing was captured" "$dry_run"
assert_contains "excluded local.env (.tribiosignore:2: local.env)" "$dry_run"
assert_contains "excluded cache/ (.tribiosignore:4: cache/)" "$dry_run"
[ ! -e "$PROJECT/.tribios" ] || fail "a dry run must not write Tribios storage"

explained="$("$TRIBIOS_BIN" check-ignore --project "$PROJECT" cache/nested/blob.bin)"
assert_eq "excluded cache/nested/blob.bin (.tribiosignore:4: cache/, matched parent directory cache)" \
  "$explained" "explanation through an excluded parent"
explained="$("$TRIBIOS_BIN" check-ignore --project "$PROJECT" vendor/dep/answer.h)"
assert_eq "included vendor/dep/answer.h (no pattern matches)" "$explained" "re-included dependency"
explained="$("$TRIBIOS_BIN" check-ignore --project "$PROJECT" .tribiosignore)"
assert_contains "included .tribiosignore" "$explained"

configure_project
grep -q "^excluded by .tribiosignore: " "$WORK/configure.out" ||
  fail "configure must report excluded entries and bytes"
grep -q "not a security boundary" "$WORK/configure.err" ||
  fail "configure must warn that .tribiosignore is not a security boundary"

start_daemon
tribios workspace create filtered >/dev/null

! ws_exists filtered local.env || fail "an excluded untracked file reached the Workspace"
! ws_exists filtered docs/list.txt || fail "an excluded tracked file reached the Workspace"
! ws_exists filtered cache || fail "an excluded directory reached the Workspace"
ws_exists filtered vendor/dep/answer.h || fail "a re-included dependency is missing"
ws_exists filtered src/main.cpp || fail "unmatched content is missing"
assert_eq "$(cat "$PROJECT/.tribiosignore")" "$(ws_read filtered .tribiosignore)" \
  "the Workspace's copy of the policy that shaped it"

# Editing the policy never changes the existing Base state or its Workspaces.
printf 'src/\n' > "$PROJECT/.tribiosignore"
tribios workspace create after-edit >/dev/null
ws_exists filtered src/main.cpp || fail "a policy edit changed an existing Workspace"
ws_exists after-edit src/main.cpp || fail "a policy edit changed the existing Base state"
! ws_exists after-edit local.env || fail "a policy edit changed the existing Base state"

stop_daemon
echo "PASS capture policy"
