#!/usr/bin/env bash
# Reads, creates, overwrites, truncation, directories, rename, unlink,
# recursive removal, metadata, permissions and symlinks.
source "$(dirname "$0")/lib.sh"

make_project
configure_project
start_daemon
tribios workspace create fs >/dev/null

# Read of a Base-state file.
assert_eq "shared base content" "$(ws_read fs docs/notes.txt)" "base file read"

# Create, overwrite and truncate.
ws_write fs new.txt "created"
assert_eq "created" "$(ws_read fs new.txt)" "new file"
ws_write fs docs/notes.txt "overwritten by the workspace"
assert_eq "overwritten by the workspace" "$(ws_read fs docs/notes.txt)" "overwritten base file"
ws_truncate_to_empty fs new.txt
assert_eq "" "$(ws_read fs new.txt)" "truncated file"

# Directories.
ws_mkdir fs newdir
ws_write fs newdir/inner.txt "inner"
assert_contains "inner.txt" "$(ws_ls fs newdir)"

# File and directory sync use the native filesystem contract.
python3 - "$(ws_path fs)/newdir/inner.txt" "$(ws_path fs)/newdir" <<'PY'
import os
import sys

for path in sys.argv[1:]:
    descriptor = os.open(path, os.O_RDONLY)
    os.fsync(descriptor)
    os.close(descriptor)
PY

# Rename stays private to the Workspace.
ws_mv fs docs/list.txt docs/renamed.txt
ws_exists fs docs/list.txt && fail "the rename source must be gone"
assert_eq "one" "$(ws_read fs docs/renamed.txt | head -1)" "renamed file contents"

# Unlink and recursive removal driven by an ordinary tool.
ws_rm fs new.txt
ws_exists fs new.txt && fail "an unlinked file must be gone"
ws_rm_rf fs newdir
ws_exists fs newdir && fail "a recursively removed directory must be gone"

# Metadata and permissions.
assert_eq "dir" "$(ws_stat fs docs | awk '{print $1}')" "directory kind"
ws_chmod fs docs/renamed.txt 640
assert_eq "640" "$(ws_stat fs docs/renamed.txt | awk '{print $2}')" "permission change"

# Symlinks: the Base-state symlink is preserved, and new ones work.
assert_eq "../src/main.cpp" "$(ws_readlink fs docs/main.link)" "captured symlink target"
ws_symlink fs ./renamed.txt docs/alias.link
assert_eq "./renamed.txt" "$(ws_readlink fs docs/alias.link)" "new symlink target"
assert_eq "symlink" "$(ws_stat fs docs/alias.link | awk '{print $1}')" "symlink kind"

stop_daemon
echo "PASS filesystem semantics"
