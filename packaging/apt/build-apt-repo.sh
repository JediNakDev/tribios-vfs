#!/usr/bin/env bash
# Regenerates the signed APT repository in place.
#
#   packaging/apt/build-apt-repo.sh <repo-dir> <incoming-dir> <gpg-key-id>
#
# <repo-dir> is the published tree and already holds every previously released
# .deb; this script adds the new ones and rewrites the indexes over the whole
# pool, so older versions stay installable. <incoming-dir> holds one
# subdirectory per suite, each containing that suite's freshly built .deb files.
#
# Debian's third-party repository guidance is the source of the shape here: one
# suite per distribution release, and a key the user scopes with Signed-By
# rather than trusting globally.
# https://wiki.debian.org/DebianRepository/UseThirdParty
set -Eeuo pipefail

repo="$(mkdir -p "${1:?usage: build-apt-repo.sh <repo-dir> <incoming-dir> <gpg-key-id>}" && cd "$1" && pwd)"
incoming="$(cd "${2:?missing incoming dir}" && pwd)"
key_id="${3:?missing gpg key id}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

read_suites() { grep -v '^[[:space:]]*#' "$here/suites.txt" | grep -v '^[[:space:]]*$'; }

gpg --batch --yes --armor --export "$key_id" > "$repo/tribios-vfs.asc"

while read -r suite image architecture; do
  pool="$repo/pool/$suite/main/t/tribios-vfs"
  dist="$repo/dists/$suite"
  mkdir -p "$pool" "$dist/main/binary-$architecture"

  if [ -d "$incoming/$suite" ]; then
    find "$incoming/$suite" -name '*.deb' -exec cp {} "$pool/" \;
  fi
  [ -n "$(find "$pool" -name '*.deb' -print -quit)" ] ||
    { echo "no package in the $suite pool" >&2; exit 1; }

  # Paths inside Packages must be relative to the repository root, so
  # apt-ftparchive runs from there.
  ( cd "$repo" && apt-ftparchive packages "pool/$suite" ) \
    > "$dist/main/binary-$architecture/Packages"
  gzip -9 -c "$dist/main/binary-$architecture/Packages" \
    > "$dist/main/binary-$architecture/Packages.gz"

  ( cd "$repo" && apt-ftparchive \
      -o "APT::FTPArchive::Release::Origin=Tribios VFS" \
      -o "APT::FTPArchive::Release::Label=Tribios VFS preview" \
      -o "APT::FTPArchive::Release::Suite=$suite" \
      -o "APT::FTPArchive::Release::Codename=$suite" \
      -o "APT::FTPArchive::Release::Architectures=$architecture" \
      -o "APT::FTPArchive::Release::Components=main" \
      -o "APT::FTPArchive::Release::Description=Tribios VFS preview packages for $suite ($image)" \
      release "dists/$suite" ) > "$dist/Release"

  # InRelease is what modern APT fetches; Release.gpg stays for older clients.
  gpg --batch --yes --local-user "$key_id" --clearsign -o "$dist/InRelease.new" "$dist/Release"
  mv "$dist/InRelease.new" "$dist/InRelease"
  gpg --batch --yes --local-user "$key_id" --detach-sign --armor -o "$dist/Release.gpg.new" "$dist/Release"
  mv "$dist/Release.gpg.new" "$dist/Release.gpg"

  echo "published $suite: $(find "$pool" -name '*.deb' | wc -l | tr -d ' ') package(s)"
done < <(read_suites)

# GitHub Pages would otherwise run the tree through Jekyll and drop the
# directories APT needs.
touch "$repo/.nojekyll"
