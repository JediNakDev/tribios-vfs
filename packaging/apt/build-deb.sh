#!/usr/bin/env bash
# Builds one binary .deb for one suite. Runs inside that suite's container:
# a Debian package is only valid for the release it was built against, which is
# why there is one build per suite rather than one universal .deb.
#
#   packaging/apt/build-deb.sh <suite> <source-root> <output-dir>
set -Eeuo pipefail

suite="${1:?usage: build-deb.sh <suite> <source-root> <output-dir>}"
source_root="$(cd "${2:?missing source root}" && pwd)"
output_dir="$(mkdir -p "${3:?missing output dir}" && cd "$3" && pwd)"

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  build-essential debhelper devscripts dpkg-dev \
  cmake pkg-config libsqlite3-dev libfuse-dev git ca-certificates

upstream="$(sed -n 's/^project(tribios_vfs VERSION \([^ )]*\).*/\1/p' "$source_root/CMakeLists.txt")"
version="$("$source_root/packaging/apt/deb_version.sh" "$suite" "$source_root")"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
tree="$work/tribios-vfs-${version%%-*}"

# The build tree is a copy of the release source plus the debian/ directory. It
# is copied rather than built in place so a local run cannot dirty the checkout.
mkdir -p "$tree"
tar -C "$source_root" --exclude=./.git --exclude=./build -cf - . | tar -C "$tree" -xf -
rm -rf "$tree/debian"
cp -R "$source_root/packaging/debian" "$tree/debian"

maintainer="$(sed -n 's/^Maintainer: //p' "$tree/debian/control")"
changelog_date="$(date -R ${SOURCE_DATE_EPOCH:+--date="@$SOURCE_DATE_EPOCH"})"
cat > "$tree/debian/changelog" <<EOF
tribios-vfs ($version) $suite; urgency=medium

  * Preview build of upstream $upstream for $suite.

 -- $maintainer  $changelog_date
EOF

( cd "$tree" && dpkg-buildpackage -b -uc -us )

deb="$work/tribios-vfs_${version}_$(dpkg --print-architecture).deb"
[ -f "$deb" ] || { echo "expected $deb, got: $(ls "$work")" >&2; exit 1; }
cp "$deb" "$output_dir/"
echo "built $(basename "$deb")"
