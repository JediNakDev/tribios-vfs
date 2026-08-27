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

# One suite may be built for several architectures, so the rows are grouped by
# suite: the pool is split per architecture because apt-ftparchive has no
# architecture filter and would otherwise list every .deb in every index.
for suite in $(read_suites | awk '{print $1}' | awk '!seen[$0]++'); do
  image="$(read_suites | awk -v s="$suite" '$1 == s {print $2; exit}')"
  architectures="$(read_suites | awk -v s="$suite" '$1 == s {print $3}')"
  dist="$repo/dists/$suite"

  for architecture in $architectures; do
    pool="$repo/pool/$suite/binary-$architecture/main/t/tribios-vfs"
    mkdir -p "$pool" "$dist/main/binary-$architecture"

    if [ -d "$incoming/$suite" ]; then
      find "$incoming/$suite" -name "*_$architecture.deb" -exec cp {} "$pool/" \;
    fi
    [ -n "$(find "$pool" -name '*.deb' -print -quit)" ] ||
      { echo "no $architecture package in the $suite pool" >&2; exit 1; }

    # Paths inside Packages must be relative to the repository root, so
    # apt-ftparchive runs from there.
    ( cd "$repo" && apt-ftparchive packages "pool/$suite/binary-$architecture" ) \
      > "$dist/main/binary-$architecture/Packages"
    gzip -9 -c "$dist/main/binary-$architecture/Packages" \
      > "$dist/main/binary-$architecture/Packages.gz"
  done

  # apt-ftparchive checksums every file it finds under dists/<suite>, so the
  # previous run's Release, InRelease and Release.gpg are removed first and the
  # new Release is written outside the tree and moved in. Left in place they
  # would be checksummed into the file that is supposed to authenticate them.
  rm -f "$dist/Release" "$dist/InRelease" "$dist/Release.gpg"
  release_file="$(mktemp)"

  ( cd "$repo" && apt-ftparchive \
      -o "APT::FTPArchive::Release::Origin=Tribios VFS" \
      -o "APT::FTPArchive::Release::Label=Tribios VFS preview" \
      -o "APT::FTPArchive::Release::Suite=$suite" \
      -o "APT::FTPArchive::Release::Codename=$suite" \
      -o "APT::FTPArchive::Release::Architectures=$(echo $architectures)" \
      -o "APT::FTPArchive::Release::Components=main" \
      -o "APT::FTPArchive::Release::Description=Tribios VFS preview packages for $suite ($image)" \
      release "dists/$suite" ) > "$release_file"
  mv "$release_file" "$dist/Release"

  # InRelease is what modern APT fetches; Release.gpg stays for older clients.
  gpg --batch --yes --local-user "$key_id" --clearsign -o "$dist/InRelease.new" "$dist/Release"
  mv "$dist/InRelease.new" "$dist/InRelease"
  gpg --batch --yes --local-user "$key_id" --detach-sign --armor -o "$dist/Release.gpg.new" "$dist/Release"
  mv "$dist/Release.gpg.new" "$dist/Release.gpg"

  echo "published $suite [$(echo $architectures)]: $(find "$repo/pool/$suite" -name '*.deb' | wc -l | tr -d ' ') package(s)"
done

# GitHub Pages would otherwise run the tree through Jekyll and drop the
# directories APT needs.
touch "$repo/.nojekyll"
