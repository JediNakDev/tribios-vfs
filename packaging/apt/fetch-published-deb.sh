#!/usr/bin/env bash
# Prints the path of the newest already-published .deb for one suite, or
# nothing when the repository has no package for it yet.
#
#   packaging/apt/fetch-published-deb.sh <base-url> <suite> <arch> <dest-dir>
#
# The upgrade half of the install test needs a real previous preview to upgrade
# from. Before the first publication there is none, and the caller runs the
# clean-install half only.
set -Eeuo pipefail

base_url="${1:?usage: fetch-published-deb.sh <base-url> <suite> <arch> <dest-dir>}"
suite="${2:?missing suite}"
architecture="${3:?missing architecture}"
dest="${4:?missing destination directory}"

packages="$(curl -fsSL "$base_url/dists/$suite/main/binary-$architecture/Packages" 2>/dev/null)" || exit 0

# The last Filename in the index is the newest, because apt-ftparchive walks the
# pool in name order and the version is in the file name.
filename="$(printf '%s\n' "$packages" | sed -n 's/^Filename: //p' | sort -V | tail -1)"
[ -n "$filename" ] || exit 0

mkdir -p "$dest"
out="$dest/$(basename "$filename")"
curl -fsSL "$base_url/$filename" -o "$out"
echo "$out"
