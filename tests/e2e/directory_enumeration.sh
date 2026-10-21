#!/usr/bin/env bash
source "$(dirname "$0")/lib.sh"

DIRECTORY_PROBE="${TRIBIOS_DIRECTORY_PROBE:?TRIBIOS_DIRECTORY_PROBE must point at the directory probe}"
ENTRY_COUNT=4096

make_project
mkdir -p "$PROJECT/bulk/subdirectory"
for ((index = 0; index < ENTRY_COUNT; ++index)); do
  printf -v name 'entry-%04d.txt' "$index"
  printf 'x' > "$PROJECT/bulk/$name"
done
ln -s subdirectory "$PROJECT/bulk/link-to-subdirectory"

configure_project
start_daemon
tribios workspace create enumeration >/dev/null
require_mount
"$DIRECTORY_PROBE" "$(ws_path enumeration)/bulk" "$ENTRY_COUNT"

# Copy-up changes the backing inode of both a file and a Base-only parent.
ws_write enumeration bulk/entry-0000.txt upper
ws_write enumeration bulk/subdirectory/nested.txt upper
# Let pre-copy-up getattr replies expire before comparing stat and readdir.
sleep 1.1
"$DIRECTORY_PROBE" "$(ws_path enumeration)/bulk" "$ENTRY_COUNT"
stop_daemon
