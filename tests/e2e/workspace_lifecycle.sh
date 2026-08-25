#!/usr/bin/env bash
# A Workspace can be created, listed, read as a full Base-state copy and removed.
source "$(dirname "$0")/lib.sh"

make_project
configure_project
start_daemon
tribios workspace create alpha >/dev/null
tribios workspace create beta --branch feature/beta >/dev/null

listing="$(tribios workspace list)"
assert_contains "alpha" "$listing"
assert_contains "feature/beta" "$listing"

# A Workspace starts from the Base state, including ignored and untracked
# content needed to build.
entries="$(ws_ls alpha)"
assert_contains "vendor" "$entries"
assert_contains "local.env" "$entries"
assert_contains ".git" "$entries"
assert_eq "SECRET_TOKEN=untracked-and-ignored" "$(ws_read alpha local.env)" "ignored file contents"

# An untouched Workspace holds no copies of the Base state.
untouched_bytes="$(tribios upper-bytes beta)"
base_bytes="$(grep -o 'base state: [0-9]* entries, [0-9]*' "$WORK/configure.out" | awk '{print $5}')"
[ "$untouched_bytes" -lt $(( base_bytes / 100 + 4096 )) ] ||
  fail "an untouched Workspace must not consume Base-state sized storage (got $untouched_bytes)"

# Logical removal makes the Workspace disappear and is reported separately from
# physical reclamation.
python3 - "$PROJECT/.tribios/meta.db" <<'PY'
import sqlite3
import sys

with sqlite3.connect(sys.argv[1]) as database:
    database.execute("""
        CREATE TRIGGER reject_workspace_save
        BEFORE INSERT ON workspace
        BEGIN
          SELECT RAISE(FAIL, 'test metadata write failure');
        END
    """)
PY
if removal_error="$(tribios workspace remove alpha 2>&1)"; then
  fail "logical removal must fail when its metadata cannot be committed"
fi
assert_contains "test metadata write failure" "$removal_error"
assert_eq "shared base content" "$(ws_read alpha docs/notes.txt)" \
  "a failed logical removal must leave the Workspace accessible"
assert_contains $'alpha\tactive' "$(tribios workspace list)"
python3 - "$PROJECT/.tribios/meta.db" <<'PY'
import sqlite3
import sys

with sqlite3.connect(sys.argv[1]) as database:
    database.execute("DROP TRIGGER reject_workspace_save")
PY

removal="$(tribios workspace remove alpha)"
assert_contains "logically in" "$removal"
assert_gone alpha docs/notes.txt
tribios workspace wait-reclaim
after="$(tribios workspace list | grep '^alpha')"
assert_contains "reclaimed" "$after"
reclaim_us="$(echo "$after" | awk -F'\t' '{print $6}')"
[ "$reclaim_us" -ge 0 ] || fail "physical reclamation time must be recorded separately"

stop_daemon
echo "PASS workspace lifecycle"
