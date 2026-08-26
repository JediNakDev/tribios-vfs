# The AUR package

`PKGBUILD` and `.SRCINFO` here are the source of truth for the [`tribios-vfs`](https://aur.archlinux.org/packages/tribios-vfs) AUR package.
They are copied to the AUR Git repository by the `update-aur-package` job in `.github/workflows/release.yml`.

Edit the recipe here.
Never edit the AUR repository directly; the next release overwrites it.

The recipe builds the immutable release archive that `docs/release-and-install-contract.md` fixes, through the same `cmake --install` rules every other packaging recipe uses.
It adds one file to the documented layout, `/usr/share/licenses/tribios-vfs/LICENSE`, because Arch records licenses there.
`-DTRIBIOS_BUILD_TESTS=OFF` keeps the Catch2 fetch, which happens at configure time, out of a build running in a network-isolated chroot, so `check()` exercises the built CLI instead of the test suite.

## What a release does

1. `publish-archive` builds `tribios-vfs-<version>.tar.gz` with `git archive` and publishes it with its SHA-256.
2. `update-aur-package` runs `update-recipe.sh <version>`, which downloads both files, checks the archive against the published checksum, rewrites `pkgver`, `pkgrel` and `sha256sums`, and regenerates `.SRCINFO` with `makepkg`.
3. It pushes both files to the AUR over SSH.

`sha256sums` in this repository therefore holds a zeroed placeholder, and `.SRCINFO` holds the same.
That is deliberate.
A checked-in checksum would be a second source of truth for something the release already publishes, and it would be stale the moment a version is cut.
The placeholder is zeros rather than `SKIP` so a recipe published without the rewrite fails loudly instead of accepting any archive.

## One-time setup

Only a human with the accounts can do these.

- Register an SSH public key on an [AUR account](https://aur.archlinux.org/), under My Account.
- Add the private half to `JediNakDev/tribios-vfs` as the Actions secret `AUR_SSH_PRIVATE_KEY`.
- Claim the name by pushing the first commit by hand, using the steps under Publishing by hand below.
  The AUR creates a package repository on its first push, and the pusher becomes its maintainer.

The release workflow fails with a pointer to this file if the secret is missing, rather than skipping the AUR silently.

## Verifying in a clean Arch container

```sh
podman run --rm -it -v "$PWD/packaging/aur:/recipe:ro" archlinux:base-devel bash -c '
  pacman -Syu --noconfirm namcap &&
  useradd -m build && cp /recipe/PKGBUILD /recipe/update-recipe.sh /home/build/ &&
  chown -R build /home/build &&
  su build -c "cd ~ && ./update-recipe.sh 0.0.1 . && makepkg -si --noconfirm &&
               namcap PKGBUILD *.pkg.tar.zst && tribios version"'
```

That covers the acceptance criteria except the mounted workflow: mounting inside a container needs `--device /dev/fuse --cap-add SYS_ADMIN`.
Without those, run the core workflow through `tribios fs`, which drives the same Workspace engine without a mount.

## Publishing by hand

```sh
git clone ssh://aur@aur.archlinux.org/tribios-vfs.git aur-tribios-vfs
packaging/aur/update-recipe.sh 0.0.1 aur-tribios-vfs   # on Arch, so .SRCINFO regenerates
cd aur-tribios-vfs && git add -A && git commit -m "tribios-vfs v0.0.1" && git push
```

`.SRCINFO` must match `PKGBUILD`; the AUR rejects a push where it does not.

## What is not published here

There is no `yay` registry.
`yay` and other AUR helpers read PKGBUILDs from the AUR, so publishing here is the whole job: the package is discoverable to helper users immediately, with nothing else to submit anywhere.

`tribios-vfs-git`, a moving development package, is deferred until there is real demand.
So is a project-owned signed binary pacman repository built with `repo-add`.

Manjaro and EndeavourOS are not claimed.
Both lag or diverge from Arch's package set, so a passing Arch build says nothing about them.
Claiming either means running the same container check on that distribution first.

Official Arch repository promotion needs an Arch Package Maintainer to adopt the package.
[Issue #20](https://github.com/JediNakDev/tribios-vfs/issues/20) tracks it.
