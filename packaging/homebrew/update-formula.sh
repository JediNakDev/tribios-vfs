#!/usr/bin/env bash
# Points the Homebrew formula at a published release.
#
# The checksum is never computed locally. It is read from the .sha256 file the
# release workflow published beside the archive, then verified against the
# archive that URL actually serves, so a formula can only ever pin a checksum
# that both the release and a fresh download agree on.
#
# Usage: packaging/homebrew/update-formula.sh <version> [formula-path]
set -Eeuo pipefail

version="${1:?usage: update-formula.sh <version> [formula-path]}"
formula="${2:-$(dirname "$0")/tap/Formula/tribios-vfs.rb}"
[ -f "$formula" ] || { echo "no formula at $formula" >&2; exit 1; }

archive="tribios-vfs-${version}.tar.gz"
base="https://github.com/JediNakDev/tribios-vfs/releases/download/v${version}"
url="${base}/${archive}"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

curl --fail --silent --show-error --location -o "$work/$archive" "$url"
curl --fail --silent --show-error --location -o "$work/$archive.sha256" "$url.sha256"

published_sha256="$(awk '{print $1}' "$work/$archive.sha256")"
downloaded_sha256="$(shasum -a 256 "$work/$archive" | awk '{print $1}')"
if [ "$published_sha256" != "$downloaded_sha256" ]; then
  echo "checksum mismatch for $archive: release says $published_sha256, download is $downloaded_sha256" >&2
  exit 1
fi

# Only the two pinning lines move. Anything else in the formula is hand-written.
tmp="$work/formula.rb"
sed -e "s|^  url \".*\"$|  url \"${url}\"|" \
    -e "s|^  sha256 \".*\"$|  sha256 \"${published_sha256}\"|" \
    "$formula" > "$tmp"
grep -q "^  url \"${url}\"$" "$tmp" || { echo "the url line was not rewritten" >&2; exit 1; }
grep -q "^  sha256 \"${published_sha256}\"$" "$tmp" || { echo "the sha256 line was not rewritten" >&2; exit 1; }
mv "$tmp" "$formula"

echo "$formula now pins v${version} at ${published_sha256}"
