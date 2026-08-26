#!/usr/bin/env bash
# Designed decision: the tested-suite matrix is written once, in
# packaging/apt/suites.txt, and every consumer agrees with it. A suite that is
# built but undocumented would be an unsupported install claim, which is the
# specific failure docs/install/debian-ubuntu.md exists to prevent.
#
# This test reads packaging metadata only. Building a .deb and installing it
# needs a Debian container, which is what .github/workflows/apt-preview.yml does
# on every release tag.
source "$(dirname "$0")/lib.sh"

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SUITES="$ROOT/packaging/apt/suites.txt"
DOC="$ROOT/docs/install/debian-ubuntu.md"

for required in \
  packaging/debian/control \
  packaging/debian/rules \
  packaging/debian/copyright \
  packaging/debian/source/format \
  packaging/apt/suites.txt \
  packaging/apt/deb_version.sh \
  packaging/apt/build-deb.sh \
  packaging/apt/build-apt-repo.sh \
  packaging/apt/verify-install.sh \
  docs/install/debian-ubuntu.md \
  .github/workflows/apt-preview.yml
do
  [ -f "$ROOT/$required" ] || fail "$required is missing"
done
[ -x "$ROOT/packaging/debian/rules" ] || fail "debian/rules is not executable"

# --- the control file declares what a package needs to be installable --------

control="$(cat "$ROOT/packaging/debian/control")"
for field in "Source: tribios-vfs" "Package: tribios-vfs" "Architecture: amd64" \
             "Standards-Version:" "Maintainer:" "Homepage:"; do
  assert_contains "$field" "$control"
done
# Dependencies are resolved from the binaries rather than listed by hand, which
# is what keeps one suite's package from claiming another suite's libraries.
assert_contains '${shlibs:Depends}' "$control"
assert_contains '${misc:Depends}' "$control"
assert_contains "libfuse-dev" "$control"
assert_contains "libsqlite3-dev" "$control"
assert_contains "cmake (>= 3.24)" "$control"

# The package build must not reach the network for Catch2 or run the tier ladder.
assert_contains "TRIBIOS_BUILD_TESTS=OFF" "$(cat "$ROOT/packaging/debian/rules")"
grep -q "TRIBIOS_BUILD_TESTS" "$ROOT/CMakeLists.txt" ||
  fail "CMakeLists.txt has no TRIBIOS_BUILD_TESTS option for packagers to turn off"

# --- the matrix agrees with the documentation and the workflow ---------------

suites="$(grep -v '^[[:space:]]*#' "$SUITES" | grep -v '^[[:space:]]*$' | awk '{print $1}')"
[ -n "$suites" ] || fail "packaging/apt/suites.txt lists no suite"

for suite in $suites; do
  grep -q "| \`$suite\` |" "$DOC" || fail "suite $suite is built but not in the documented matrix"
done

documented="$(sed -n 's/^| `\([a-z0-9.]*\)` | .* |$/\1/p' "$DOC")"
for suite in $documented; do
  printf '%s\n' $suites | grep -qx "$suite" ||
    fail "suite $suite is documented as tested but is not built"
done

# --- version spelling --------------------------------------------------------

upstream="$(sed -n 's/^project(tribios_vfs VERSION \([^ )]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
for suite in $suites; do
  version="$("$ROOT/packaging/apt/deb_version.sh" "$suite")"
  assert_eq "${upstream/-/\~}-1~${suite}1" "$version" "Debian version for $suite"
done

# An upstream prerelease must sort below the release it precedes, which is the
# whole reason the Debian spelling differs from the upstream one.
if command -v dpkg >/dev/null 2>&1; then
  dpkg --compare-versions "0.2.0~beta.1-1" lt "0.2.0-1" ||
    fail "the prerelease spelling does not sort before the release"
  dpkg --compare-versions "0.2.0-1~bookworm1" lt "0.2.0-1" ||
    fail "the suite suffix does not sort below a plain Debian revision"
fi

echo "PASS Debian packaging metadata"
