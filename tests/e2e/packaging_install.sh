#!/usr/bin/env bash
# Designed install contract: the staged layout, the core workflow run from an
# installed tree, and the rule that upgrade and uninstall never touch Project
# data. docs/release-and-install-contract.md and docs/adr/0006-install-only-the-public-artifacts.md record the decisions.
source "$(dirname "$0")/lib.sh"

command -v cmake >/dev/null 2>&1 || skip "cmake is needed to stage an install"
BUILD_DIR="${TRIBIOS_BUILD_DIR:?TRIBIOS_BUILD_DIR must point at the build tree}"

STAGE="$(mktemp -d)"
cleanup_stage() { rm -rf "$STAGE" 2>/dev/null || true; }
trap 'cleanup_stage; cleanup' EXIT

stage_install() {
  DESTDIR="$STAGE" cmake --install "$BUILD_DIR" --prefix /usr/local >/dev/null
}

# --- clean install ----------------------------------------------------------

stage_install
staged="$(cd "$STAGE" && find . -type f | sed 's|^\./||' | sort)"
expected="$(printf '%s\n' \
  usr/local/bin/tribios \
  usr/local/libexec/tribios/tribios_daemon \
  usr/local/share/doc/tribios-vfs/LICENSE \
  usr/local/share/doc/tribios-vfs/README.md | sort)"
assert_eq "$expected" "$staged" "staged install layout"

# Internal headers and static libraries are not an SDK and must stay unstaged.
[ -z "$(cd "$STAGE" && find . -name '*.a' -o -name '*.h' -o -name '*.hpp')" ] ||
  fail "the install staged internal headers or static libraries"

INSTALLED_CLI="$STAGE/usr/local/bin/tribios"
[ -x "$INSTALLED_CLI" ] || fail "the installed CLI is not executable"

# --- the installed CLI finds its daemon without help ------------------------

# TRIBIOS_DAEMON is how the other tests point at the build tree. Unsetting it is
# the point of this test: an installed CLI must resolve libexec on its own.
unset TRIBIOS_DAEMON
TRIBIOS_BIN="$INSTALLED_CLI"

version_line="$("$INSTALLED_CLI" version)"
assert_contains "tribios " "$version_line"

# --- core workflow against the installed tree -------------------------------

make_project
configure_project
start_daemon
tribios workspace create packaged >/dev/null
ws_write packaged docs/installed.txt "staged install works"
assert_eq "staged install works" "$(ws_read packaged docs/installed.txt)" "installed workflow read"
stop_daemon

# --- upgrade preserves Project data -----------------------------------------

# Reinstalling over an existing prefix is what a package upgrade does to the
# filesystem. It does not exercise a version-to-version migration; that arrives
# with the first release that changes the on-disk metadata format.
stage_install
start_daemon
assert_eq "staged install works" "$(ws_read packaged docs/installed.txt)" "workspace data after upgrade"
assert_contains "packaged" "$(tribios workspace list)"
stop_daemon

# --- uninstall preserves Project data ---------------------------------------

[ -f "$BUILD_DIR/install_manifest.txt" ] || fail "cmake --install wrote no install manifest"
# CMake writes the manifest without a trailing newline, so the last entry only
# arrives through the read's failure branch.
while IFS= read -r installed_file || [ -n "$installed_file" ]; do
  rm -f "$STAGE$installed_file"
done < "$BUILD_DIR/install_manifest.txt"
[ -z "$(cd "$STAGE" && find . -type f)" ] || fail "uninstall left files behind in the staging root"

[ -d "$PROJECT/.tribios" ] || fail "uninstall removed the Project data directory"
[ -f "$PROJECT/.tribios/meta.db" ] || fail "uninstall removed the Project metadata database"

echo "PASS packaging install contract"
