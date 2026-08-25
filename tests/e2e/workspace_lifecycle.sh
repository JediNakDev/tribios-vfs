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

# Removal makes the Workspace disappear.
tribios workspace remove alpha >/dev/null
assert_gone alpha docs/notes.txt

stop_daemon
echo "PASS workspace lifecycle"
