# Fedora and EPEL preview RPMs through Copr

Copr is the preview channel for Fedora and EPEL.
It builds the spec in [`packaging/rpm/tribios-vfs.spec`](../../packaging/rpm/tribios-vfs.spec) and publishes the result as an ordinary DNF repository.
This is a preview repository, not Fedora.
Official Fedora inclusion needs a package review and an ongoing maintainer, and is tracked separately in issue #20.

The spec builds from the release archive described in [the release and install contract](../release-and-install-contract.md), so a Copr build never packages an untagged tree.
Cut and publish the tag first, then build.

## What the package installs

The four files the contract stages, and nothing else:

```text
/usr/bin/tribios
/usr/libexec/tribios/tribios_daemon
/usr/share/doc/tribios-vfs/README.md
/usr/share/doc/tribios-vfs/LICENSE
```

There is no system service unit, no `/etc` file, and no post-install configuration.
The daemon is per-project and the user starts it with `tribios daemon start`.
Nothing in the package reads or writes `.tribios`, so an upgrade or an erase cannot touch project data.

Runtime dependencies are `fuse`, because the daemon mounts through the FUSE 2 API and unmounts by running `fusermount`, and `git-core`, because a Workspace is a Git worktree driven by the `git` command line.

## Chroots

Claim only what is actually built and tested.

- The current stable Fedora releases and rawhide, on `x86_64` and `aarch64`.
- `epel-10`, whose GCC 14 compiles C++23 as it stands.
- `epel-9` only if its build passes.
  RHEL 9's system GCC is 11 and has no `<expected>`, so the spec switches to `gcc-toolset-14-gcc-c++` there, and `cmake` must be 3.24 or newer, which means RHEL 9.4 or later.
  `fuse-devel` on EPEL 9 comes from CRB, so the chroot needs CRB enabled.
  Drop the chroot rather than weakening the code if any of that fails.

Rocky Linux and AlmaLinux are claimed only where the EPEL build is genuinely tested on them.
The spec carries no conditionals for arbitrary RHEL derivatives; Fedora's packaging guidelines prohibit them and they make the build untestable.

Amazon Linux 2023 is a separate build target, not a relabelled EPEL binary.
AWS states that no EPEL version is binary compatible with AL2023, so it needs its own build and its own test in an Amazon Linux environment.
It is tracked in its own issue and is not enabled here.

## Setting up the Copr project

One-time, from a Fedora account with a Copr login:

1. Create the project `tribios-vfs` and enable the chroots above.
2. Add a package with the **Custom** source type, pointing at this repository.
   The build command is `make -f .copr/Makefile srpm outdir=$outdir`, which [`.copr/Makefile`](../../.copr/Makefile) implements: it downloads the release archive named in `Source0` and builds a source RPM from the spec.
3. Enable the GitHub webhook if builds should follow pushes, or trigger each release by hand.

Then, per release:

```sh
copr-cli build tribios-vfs --nowait
```

## Prerelease ordering

RPM sorts a tilde before everything, so a preview is spelled `0.2.0~beta1` and sorts before `0.2.0`.
Never spell it `0.2.0-beta1`, which sorts after the release and would strand testers on the beta.

The upstream version stays `0.2.0-beta.1`; each ecosystem uses its own spelling because none sorts correctly everywhere.

## Install instructions to publish

```sh
sudo dnf copr enable jedinakdev/tribios-vfs
sudo dnf install tribios-vfs
```

## Verifying a build

The spec is checked against the install contract on every pull request by `tests/e2e/packaging_rpm_spec.sh`, which keeps the version, the license, the source archive name and the packaged file list from drifting.
That test cannot run a Fedora build, so the acceptance evidence for a release is a run in a clean container per chroot:

```sh
podman run --rm -it --device /dev/fuse --cap-add SYS_ADMIN fedora:latest bash -euxc '
  dnf -y install dnf-plugins-core
  dnf -y copr enable jedinakdev/tribios-vfs
  dnf -y install tribios-vfs git
  tribios version
  git init -q /tmp/project && cd /tmp/project
  git commit -q --allow-empty -m init
  tribios configure /tmp/project
  tribios daemon start
  tribios workspace create smoke
  tribios daemon stop
'
```

The upgrade check is the same container with the previous preview installed first: install the older version, create a Workspace with a file in it, `dnf upgrade tribios-vfs`, and confirm the Workspace and its contents are still there.
`dnf remove tribios-vfs` must leave `.tribios` untouched.
