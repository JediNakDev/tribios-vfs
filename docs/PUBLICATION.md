# Publishing a release

Pushing a `v*` tag does not publish everything.
Two of the four channels publish themselves.
The other two need a human, and one of them does not exist yet.

Read this before tagging, because the source archive is immutable once the tag lands.

## Before the tag

The tag has to match the `project()` version in `CMakeLists.txt` or the release workflow refuses it at the first step.

Everything the release ships comes from the tagged tree, including `README.md`, which goes into the archive and is installed to `share/doc/tribios-vfs/README.md`.
Whatever is uncommitted at tag time is not in the release.

Two Actions secrets have to exist, or their jobs fail the moment the tag lands:

| Secret | Used by | Set up in |
| --- | --- | --- |
| `HOMEBREW_TAP_DEPLOY_KEY` | `update-homebrew-tap` | `packaging/homebrew/README.md` |
| `AUR_SSH_PRIVATE_KEY` | `update-aur-package` | `packaging/aur/README.md` |

## What the tag does on its own

`publish-archive` builds `tribios-vfs-<version>.tar.gz` with `git archive`, records its SHA-256, and publishes both as a GitHub prerelease.

`update-homebrew-tap` then rewrites the formula's `url` and `sha256` from that checksum file and pushes the whole of `packaging/homebrew/tap/` to `JediNakDev/homebrew-tap`.
The tap's own CI picks the push up and runs `brew install` and `brew test` on clean macOS runners, so the Homebrew channel verifies itself.

`update-aur-package` rewrites `pkgver`, `pkgrel` and `sha256sums`, regenerates `.SRCINFO` with `makepkg`, and pushes both files to the AUR.
The AUR runs nothing.
Verify it yourself with the Arch container check in `packaging/aur/README.md`.

The archive is the only part that cannot be redone.
The two downstream jobs are ordinary workflow jobs, so a failed one can be re-run from the Actions page after you fix whatever broke it.
A missing secret is worth catching before the tag, but it is not fatal to the release.

## Fedora and EPEL, by hand

Copr builds from the release archive rather than from a checkout, so a build before the tag exists would package the previous version.
Cut the tag, confirm the release is published, then build.

The first release also needs the Copr project itself, which is a one-time setup described in `docs/packaging/copr.md`: create `tribios-vfs`, enable the chroots, and add a package with the SCM source type and the make srpm build method.

Per release, point the package's committish at the new tag, then:

```sh
copr-cli build-package --name tribios-vfs jedinakdev/tribios-vfs
```

Leaving the committish on an old tag is the easy mistake here.
Copr will build happily and publish the previous version again, and nothing will complain.

Acceptance evidence for a release is a container run per chroot.
`docs/packaging/copr.md` has the exact commands, including the upgrade check that a new package leaves an existing `.tribios` directory alone.

## Debian and Ubuntu

Nothing is published.
There is no `debian/` directory, no repository tooling, and no workflow, so the tag does nothing for apt users.
Issue [#17](https://github.com/JediNakDev/tribios-vfs/issues/17) tracks the work.

`APT_SIGNING_KEY` and `APT_SIGNING_KEY_ID` are already set, so the signing identity is ready and the packaging is not.

An APT repository is built from the published archive, the same as Copr, so this channel can be added after a release without recutting it.

## After everything is published

The GitHub release is created as a prerelease and should stay that way while Tribios is at `0.y.z`.

Check that each channel actually serves the new version:

```sh
brew update && brew info JediNakDev/tap/tribios-vfs   # tap
curl -s https://aur.archlinux.org/rpc/v5/info/tribios-vfs   # AUR
dnf --refresh info tribios-vfs                        # Copr, on a Fedora box
```

A channel that silently kept serving the old version is the failure worth looking for.
Each of the three has a different way of going quiet about it.
