# Publication steps

Three of the four channels publish themselves when you push a `v*` tag.
Copr does not, and never will, because Copr builds run in Fedora's infrastructure against an account no workflow holds a token for.

Read this before tagging, not after.
A tag is immutable, so anything wrong in the tagged tree costs a new version number.

| Channel | On a `v*` tag | Needs a human |
| --- | --- | --- |
| Homebrew | `update-homebrew-tap` pushes the formula to the tap | no, once the deploy key is set |
| APT | `apt-preview.yml` builds, verifies and republishes the repository | no, once the signing key is set |
| AUR | `update-aur-package` pushes `PKGBUILD` and `.SRCINFO` | no, once the SSH key is set |
| Copr | nothing happens | yes, every release |

## Before you tag

Check the version first.
`CMakeLists.txt` and `packaging/rpm/tribios-vfs.spec` both carry it, the design tier fails if they disagree, and the release workflow refuses a tag that does not match `CMakeLists.txt`.

Then confirm the secrets exist, because a missing one turns a release run red halfway through:

```sh
gh secret list
```

`HOMEBREW_TAP_DEPLOY_KEY`, `APT_SIGNING_KEY` and `APT_SIGNING_KEY_ID` are set.
`AUR_SSH_PRIVATE_KEY` is not, so the AUR job fails today.
The archive still publishes when a downstream job fails, so a red run is recoverable by fixing the secret and re-running that job alone.

Tag from `main` once the packaging branches you want in this release are merged:

```sh
git tag -a v0.0.1 -m v0.0.1
git push origin v0.0.1
```

## Homebrew

Automatic.
`update-homebrew-tap` regenerates the formula against the published archive and its checksum, then pushes the whole of `packaging/homebrew/tap` to `JediNakDev/homebrew-tap`.

Nothing is left to do afterwards except confirm it worked:

```sh
brew tap JediNakDev/tap
brew install tribios-vfs
tribios version
```

The tap repository is generated wholesale on every release.
Editing it by hand is pointless; the next tag overwrites it.

## APT

Automatic, and the longest of the four.
`apt-preview.yml` builds one `.deb` per suite in `packaging/apt/suites.txt`, installs each one in a clean container, signs the repository with `APT_SIGNING_KEY`, and republishes it on the `gh-pages` branch, which GitHub Pages already serves from `https://jedinakdev.github.io/tribios-vfs/`.

This is the one channel where a tag can publish nothing at all without failing.
The workflow lives on the branch for issue #17.
If that branch is not merged when you tag, the tag simply has no APT job in it, and the omission is silent.

Afterwards, check the repository the workflow published rather than the workflow's own green tick, since the two can disagree if a publish step races:

```sh
curl -fsS https://jedinakdev.github.io/tribios-vfs/apt/dists/bookworm/Release | head
```

## Copr

Manual, every time.

Copr clones this repository, so the spec is not frozen by the tag the way the tarball is.
A broken spec is fixable without cutting a new version: push the fix to `main` and point the Copr package at the newer commit.
`Source0` still fetches the tagged archive.

Run the wizard, which walks the account, the project, the chroots, the build and the per-chroot container acceptance, and prints the values worth recording at the end:

```sh
./packaging/publish-copr-preview.sh
```

It refuses to start until the release exists, and it never tags or pushes.
It is deliberately untracked, like the Homebrew tap setup script, because it is an operator tool rather than part of the package.

`docs/packaging/copr.md` has the same procedure by hand, plus the chroot list and the reasoning behind it.

Two chroots to watch.
Fedora still ships `fuse-devel`, so those builds are fine.
EPEL 10 may not, since RHEL has been moving consumers to fuse3, and EPEL 9 needs `gcc-toolset-14` for C++23.
Drop a chroot that fails rather than weakening the spec for it.

## AUR

Automatic once the key exists, and blocked until it does.

`update-aur-package` runs `update-recipe.sh` inside an Arch container, which rewrites `pkgver`, `pkgrel` and `sha256sums` against the published archive and regenerates `.SRCINFO` with `makepkg`, then pushes both files over SSH.

The one-time setup is in `packaging/aur/README.md` and needs an AUR account:

- register an SSH public key on `https://aur.archlinux.org/` under My Account
- add the private half as the `AUR_SSH_PRIVATE_KEY` Actions secret
- claim the name `tribios-vfs` with a first push by hand

Claiming by hand matters more than it looks.
The AUR creates a package repository on its first push and makes whoever pushed it the maintainer, so leaving the first push to a workflow makes the key's owner the maintainer of record.

Afterwards:

```sh
git clone https://aur.archlinux.org/tribios-vfs.git
grep -E 'pkgver|sha256sums' tribios-vfs/PKGBUILD
```

## When a release goes wrong

Never re-cut a tag.
Downstream recipes pin the archive checksum, and moving a tag breaks every one of them at once, silently, at different times.

For a bad spec, PKGBUILD or formula, fix it on `main` and re-run the failed job against the same tag.
Only a bad tagged tree needs a new version.
