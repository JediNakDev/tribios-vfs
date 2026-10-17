#!/usr/bin/env bash
# Builds the binary RPM from a source tree inside a clean Amazon Linux 2023
# container, as root.
#
#   packaging/rpm/al2023/build-rpm.sh <source-tree> <output-dir> [release]
#
# The spec fetches the published release archive, so this script makes the
# archive the release workflow would publish from the checked-out tree instead.
# That lets a pull request build what it changes. `release` overrides the spec's
# Release number, which is how the upgrade check gets two installable builds.
set -Eeuo pipefail

source_tree="$(cd "$1" && pwd)"
output_dir="$2"
release="${3:-}"

dnf -y install rpm-build cmake ninja-build pkgconf-pkg-config sqlite-devel fuse3-devel \
  gcc14-c++ git-core tar gzip findutils
echo "toolchain: $(cmake --version | head -1); $(gcc14-g++ --version | head -1)"

version="$(sed -n 's/^project(tribios_vfs VERSION \([0-9.]*\).*/\1/p' "$source_tree/CMakeLists.txt")"
topdir="$(mktemp -d)"
mkdir -p "$topdir/SOURCES" "$topdir/SPECS"
git config --global --add safe.directory "$source_tree"
git -C "$source_tree" archive --format=tar.gz --prefix="tribios-vfs-$version/" \
  -o "$topdir/SOURCES/tribios-vfs-$version.tar.gz" HEAD

spec="$topdir/SPECS/tribios-vfs.spec"
cp "$source_tree/packaging/rpm/tribios-vfs.spec" "$spec"
if [ -n "$release" ]; then
  sed -i "s/^Release:.*/Release:        $release%{?dist}/" "$spec"
fi

rpmbuild -bb --define "_topdir $topdir" "$spec"
mkdir -p "$output_dir"
find "$topdir/RPMS" -name 'tribios-vfs-[0-9]*.rpm' -exec cp {} "$output_dir/" \;
ls -l "$output_dir"
