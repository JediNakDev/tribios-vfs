#!/usr/bin/env bash
# Points the AUR recipe at a published release.
#
# The checksum is never computed from a local tree. It is read from the .sha256
# file the release workflow published beside the archive, then verified against
# the archive that URL actually serves, so the recipe can only ever pin a
# checksum that both the release and a fresh download agree on.
#
# .SRCINFO is regenerated with makepkg, which exists only on Arch. Elsewhere the
# script rewrites the PKGBUILD and stops, saying so.
#
# Usage: packaging/aur/update-recipe.sh <version> [recipe-dir]
set -Eeuo pipefail

version="${1:?usage: update-recipe.sh <version> [recipe-dir]}"
recipe_dir="${2:-$(dirname "$0")}"
pkgbuild="$recipe_dir/PKGBUILD"
[ -f "$pkgbuild" ] || { echo "no PKGBUILD at $pkgbuild" >&2; exit 1; }

archive="tribios-vfs-${version}.tar.gz"
url="https://github.com/JediNakDev/tribios-vfs/releases/download/v${version}/${archive}"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

curl --fail --silent --show-error --location -o "$work/$archive" "$url"
curl --fail --silent --show-error --location -o "$work/$archive.sha256" "$url.sha256"

published_sha256="$(awk '{print $1}' "$work/$archive.sha256")"
downloaded_sha256="$(sha256sum "$work/$archive" 2>/dev/null || shasum -a 256 "$work/$archive")"
downloaded_sha256="${downloaded_sha256%% *}"
if [ "$published_sha256" != "$downloaded_sha256" ]; then
  echo "checksum mismatch for $archive: release says $published_sha256, download is $downloaded_sha256" >&2
  exit 1
fi

# Only the three pinning lines move. Anything else in the PKGBUILD is hand-written.
tmp="$work/PKGBUILD"
sed -e "s|^pkgver=.*$|pkgver=${version}|" \
    -e "s|^pkgrel=.*$|pkgrel=1|" \
    -e "s|^sha256sums=('.*')$|sha256sums=('${published_sha256}')|" \
    "$pkgbuild" > "$tmp"
grep -q "^pkgver=${version}$" "$tmp" || { echo "the pkgver line was not rewritten" >&2; exit 1; }
grep -q "^sha256sums=('${published_sha256}')$" "$tmp" || { echo "the sha256sums line was not rewritten" >&2; exit 1; }
mv "$tmp" "$pkgbuild"

echo "$pkgbuild now pins v${version} at ${published_sha256}"

if ! command -v makepkg >/dev/null; then
  echo "makepkg is not available here, so .SRCINFO was not regenerated." >&2
  echo "Run 'makepkg --printsrcinfo > .SRCINFO' in $recipe_dir on Arch before publishing." >&2
  exit 0
fi
(cd "$recipe_dir" && makepkg --printsrcinfo > .SRCINFO)
echo "$recipe_dir/.SRCINFO regenerated"
