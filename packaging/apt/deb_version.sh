#!/usr/bin/env bash
# Prints the Debian version for one suite: <upstream>-1~<suite>1.
#
# The upstream version is spelled the Debian way, so an upstream prerelease
# 0.2.0-beta.1 becomes 0.2.0~beta.1 and sorts before 0.2.0, which is the whole
# reason the two spellings differ. docs/release-and-install-contract.md records
# the per-ecosystem spellings.
set -Eeuo pipefail

suite="${1:?usage: deb_version.sh <suite> [source-root]}"
root="${2:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"

upstream="$(sed -n 's/^project(tribios_vfs VERSION \([^ )]*\).*/\1/p' "$root/CMakeLists.txt")"
[ -n "$upstream" ] || { echo "no project version in $root/CMakeLists.txt" >&2; exit 1; }

echo "${upstream/-/\~}-1~${suite}1"
