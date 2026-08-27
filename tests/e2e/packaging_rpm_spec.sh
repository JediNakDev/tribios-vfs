#!/usr/bin/env bash
# Designed decision: the Copr spec is the install contract in RPM form. The Copr
# build runs in Fedora's infrastructure and cannot run here, so what this test
# guards is the part that drifts silently between releases: the version, the
# license, the release archive it fetches, and the set of files it packages.
# docs/packaging/copr.md and docs/release-and-install-contract.md record it.
source "$(dirname "$0")/lib.sh"

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SPEC="$REPO_ROOT/packaging/rpm/tribios-vfs.spec"
[ -f "$SPEC" ] || fail "the spec is missing: $SPEC"

spec_tag() { sed -n "s/^$1:[[:space:]]*//p" "$SPEC" | head -1; }

# --- the spec tracks the one place the version lives ------------------------

project_version="$(sed -n 's/^project(tribios_vfs VERSION \([0-9.]*\).*/\1/p' "$REPO_ROOT/CMakeLists.txt")"
[ -n "$project_version" ] || fail "could not read the project version from CMakeLists.txt"

# RPM spells a prerelease 0.2.0~beta1, the upstream version plus a tilde suffix
# that sorts before the release, so only the part before the tilde is compared.
spec_version="$(spec_tag Version)"
assert_eq "$project_version" "${spec_version%%~*}" "spec Version against the project version"

assert_eq "tribios-vfs" "$(spec_tag Name)" "spec Name"
assert_eq "MIT" "$(spec_tag License)" "spec License"
assert_contains "MIT" "$(head -1 "$REPO_ROOT/LICENSE")"

# The release workflow publishes exactly this archive name for tag v<version>.
assert_eq "%{url}/releases/download/v%{version}/%{name}-%{version}.tar.gz" \
  "$(spec_tag Source0)" "spec Source0 against the published archive name"

# --- the build stays offline and excludes FUSE ------------------------------

assert_contains "-DTRIBIOS_BUILD_TESTS=OFF" "$(cat "$SPEC")"
if grep -qi 'pkgconfig(fuse\|Requires:.*fuse\|TRIBIOS_HAVE_FUSE' "$SPEC"; then
  fail "the RPM spec retains a FUSE dependency"
fi
assert_contains "tribios_storage_service" "$(cat "$SPEC")"
assert_contains "tribios-storage.service" "$(cat "$SPEC")"

# --- %files matches what cmake --install actually stages --------------------

command -v cmake >/dev/null 2>&1 || skip "cmake is needed to stage an install"
BUILD_DIR="${TRIBIOS_BUILD_DIR:?TRIBIOS_BUILD_DIR must point at the build tree}"

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"; cleanup' EXIT
DESTDIR="$STAGE" cmake --install "$BUILD_DIR" --prefix /usr >/dev/null

files_section="$(sed -n '/^%files/,/^%changelog/p' "$SPEC")"
# %dir entries only claim directory ownership. The rest are the paths the
# package installs, once the %license and %doc markers and the RPM macros are
# resolved to the same prefix the staging root uses.
packaged_paths=""
while IFS= read -r line; do
  case "$line" in
    %files*|%dir*|"") continue ;;
    %license\ *|%doc\ *|%attr*\ *) line="${line#* }" ;;
    /*|%{*) ;;
    *) continue ;;
  esac
  line="${line/\%\{_bindir\}//usr/bin}"
  line="${line/\%\{_libexecdir\}//usr/libexec}"
  line="${line/\%\{_unitdir\}//usr/lib/systemd/system}"
  line="${line/\%\{_datadir\}//usr/share}"
  line="${line/\%\{name\}/tribios-vfs}"
  packaged_paths="$packaged_paths$line"$'\n'
done <<< "$files_section"
packaged_paths="$(printf '%s' "$packaged_paths" | sort)"

staged_paths="$(cd "$STAGE" && find . -type f | sed 's|^\.||' | sort)"
while IFS= read -r staged; do
  printf '%s\n' "$packaged_paths" | grep -qxF "$staged" ||
    fail "the spec does not package the staged file $staged"
done <<< "$staged_paths"

# Every packaged file must exist in the staging root too, so a stale %files
# entry fails the build in Copr rather than after the repository is published.
while IFS= read -r packaged; do
  case "$packaged" in
    */) continue ;;
  esac
  [ -e "$STAGE$packaged" ] ||
    fail "the spec packages $packaged, which cmake --install does not stage"
done <<< "$packaged_paths"

# --- let a Fedora machine run the real parser -------------------------------

# Absent everywhere but a Fedora or EPEL host, so this is extra coverage where
# it exists rather than a skip that would turn CI red on Ubuntu and macOS.
if command -v rpmspec >/dev/null 2>&1; then
  rpmspec -P "$SPEC" >/dev/null || fail "rpmspec could not parse the spec"
fi

echo "PASS packaging rpm spec contract"
