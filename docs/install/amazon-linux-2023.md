# Installing on Amazon Linux 2023

Tribios VFS attaches Amazon Linux 2023 RPMs to each GitHub release, for `x86_64` and `aarch64`.
These are preview builds of a `0.y.z` release.
The command surface, the on-disk metadata format and the mounted filesystem semantics in `docs/release-and-install-contract.md` are the compatibility surface, and a minor version may break any of them.

Amazon Linux 2023 has its own package and is not served by the Fedora and EPEL Copr repository.
AWS states that no EPEL version is binary compatible with AL2023, and Copr has no AL2023 chroot.
The release assets are the distribution channel; there is no DNF repository to enable.
The first release that carries them is the first tag cut after this page was added.

## Install

```sh
version=0.0.1
sudo dnf install -y "https://github.com/JediNakDev/tribios-vfs/releases/download/v$version/tribios-vfs-$version-1.amzn2023.$(uname -m).rpm"
tribios version
```

The package pulls in `fuse3` and `git-core` from the Amazon Linux repositories.
It is built with the distribution's `gcc14` compiler, because the system GCC 11 has no `<expected>`, and the compiler is a build dependency only.

## Mounting

`tribios daemon start` mounts a Workspace through FUSE, so the user needs access to `/dev/fuse`.
In a container, pass `--device /dev/fuse --cap-add SYS_ADMIN`, or run the daemon with `--no-mount` and drive the Workspace through `tribios fs`.

## Upgrade and removal

Install the newer release's RPM the same way to upgrade, then:

```sh
sudo dnf remove tribios-vfs
```

Neither touches `.tribios`.
Project data lives inside your Project, and no package manager owns a path into it.

## What builds and tests this

`.github/workflows/al2023-rpm.yml` builds `packaging/rpm/tribios-vfs.spec` natively in an `amazonlinux:2023` container on a runner of each architecture.
It runs on every pull request that changes the build or the RPM packaging, and on every release tag.
Each build is then installed in a clean `amazonlinux:2023` container, where `packaging/rpm/al2023/verify-install.sh` configures a Project, runs the core workflow with the installed CLI, upgrades from an older Release of the same package, checks the Workspace survived, and removes the package without touching `.tribios`.
The daemon runs with `--no-mount` there, because a container has no `/dev/fuse` by default; the mounted path is covered by the strict Linux tiers in the pull request workflow.
On a tag, the verified RPMs are uploaded to the release.

To build the package yourself in a clean container:

```sh
docker run --rm -v "$PWD:/src" -w /src amazonlinux:2023 \
  packaging/rpm/al2023/build-rpm.sh /src /src/out
```
