#!/usr/bin/env bash
# Workspace isolation is a correctness guarantee.
source "$(dirname "$0")/lib.sh"

make_project
configure_project
start_daemon
tribios workspace create one >/dev/null
tribios workspace create two >/dev/null

# Both Workspaces read the same unchanged Base-state content.
assert_eq "shared base content" "$(ws_read one docs/notes.txt)" "workspace one initial read"
assert_eq "shared base content" "$(ws_read two docs/notes.txt)" "workspace two initial read"
assert_eq "base" "$(ws_layer one docs/notes.txt)" "unchanged content must resolve to the Base state"

# The same Base-state file is mutated differently in each Workspace.
ws_write one docs/notes.txt "written by one"
ws_write two docs/notes.txt "written by two"
assert_eq "written by one" "$(ws_read one docs/notes.txt)" "workspace one after mutation"
assert_eq "written by two" "$(ws_read two docs/notes.txt)" "workspace two after mutation"
assert_eq "upper" "$(ws_layer one docs/notes.txt)" "a mutated file must be a private copy"

# The Project source is unchanged.
assert_eq "shared base content" "$(cat "$PROJECT/docs/notes.txt")" "project source"

# New files belong only to their creator.
ws_write one only-in-one.txt "private"
ws_exists two only-in-one.txt && fail "a new file must not leak into a sibling Workspace"

# Deletions stay private too.
ws_rm one docs/list.txt
ws_exists two docs/list.txt || fail "a deletion must not leak into a sibling Workspace"
[ -f "$PROJECT/docs/list.txt" ] || fail "a deletion must not reach the Project source"

# Later changes to the Project source stay out of existing Workspaces.
printf 'changed after capture\n' > "$PROJECT/docs/afterwards.txt"
ws_exists two docs/afterwards.txt && fail "the Base state must not follow later Project changes"

stop_daemon
echo "PASS isolation"
