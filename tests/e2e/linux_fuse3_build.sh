#!/usr/bin/env bash
# Designed decision: production Linux builds use libfuse3 at the operating-system
# seam. macOS continues to use the API supplied by macFUSE.
source "$(dirname "$0")/lib.sh"

[ "$(uname -s)" = "Linux" ] || skip "the libfuse3 build contract applies only to Linux"

BUILD_DIR="${TRIBIOS_BUILD_DIR:?TRIBIOS_BUILD_DIR must point at the build tree}"
COMPILE_COMMANDS="$BUILD_DIR/compile_commands.json"
[ -f "$COMPILE_COMMANDS" ] || fail "compile_commands.json is missing from $BUILD_DIR"

grep -q "TRIBIOS_HAVE_FUSE" "$COMPILE_COMMANDS" || fail "the Linux build selected the unmounted stub"
grep -q "TRIBIOS_FUSE_API_VERSION=31" "$COMPILE_COMMANDS" ||
  fail "the Linux mount backend did not select libfuse3 API 31"
command -v fusermount3 >/dev/null 2>&1 || fail "fusermount3 is required to unmount Linux Workspaces"

echo "PASS Linux libfuse3 build contract"
