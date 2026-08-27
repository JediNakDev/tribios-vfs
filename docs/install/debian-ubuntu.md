# Installing the Debian and Ubuntu preview packages

Tribios VFS publishes preview packages from a project-owned, signed APT repository.
These are preview builds of a `0.y.z` release.
The command surface, the on-disk metadata format and the mounted filesystem semantics in `docs/release-and-install-contract.md` are the compatibility surface, and a minor version may break any of them.

## Tested suites

A suite is listed here only when the release workflow builds it, installs it in a clean container of that exact image, and runs the core workflow against the installed CLI.

| Suite | Distribution | Image | Architectures |
| --- | --- | --- | --- |
| `bookworm` | Debian 12 | `debian:bookworm` | `amd64`, `arm64` |
| `trixie` | Debian 13 | `debian:trixie` | `amd64`, `arm64` |
| `noble` | Ubuntu 24.04 LTS | `ubuntu:24.04` | `amd64`, `arm64` |

`packaging/apt/suites.txt` is the machine-readable copy of this table, and `tests/e2e/debian_packaging.sh` fails if the two disagree.

### Untested

Everything else is untested, including Ubuntu 22.04 and every APT-based derivative: Linux Mint, Pop!\_OS, Kali Linux, Raspberry Pi OS, and the rest.
Sharing a package manager with Debian is not compatibility.
Each derivative needs its own install and workflow run before it can be listed above.

Ubuntu 22.04 jammy is excluded for a concrete reason: it ships CMake 3.22, and Tribios requires 3.24 or newer.

## Install

```sh
sudo apt-get install -y curl ca-certificates
sudo install -d -m 0755 /etc/apt/keyrings
curl -fsSL https://jedinakdev.github.io/tribios-vfs/apt/tribios-vfs.asc |
  sudo tee /etc/apt/keyrings/tribios-vfs.asc > /dev/null

# Replace bookworm with your suite from the table above.
echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/tribios-vfs.asc] https://jedinakdev.github.io/tribios-vfs/apt bookworm main" |
  sudo tee /etc/apt/sources.list.d/tribios-vfs.list > /dev/null

sudo apt-get update
sudo apt-get install -y tribios-vfs
tribios version
```

The key is scoped to this one repository with `Signed-By`.
It is deliberately not installed into `trusted.gpg.d`, where it would authorise that key for every repository the machine reads.

## Mounting

`tribios daemon start` mounts a Workspace through FUSE, so the user needs access to `/dev/fuse`.
The package depends on `fuse3`, which provides `fusermount3`.
In a container, pass `--device /dev/fuse --cap-add SYS_ADMIN`, or run the daemon with `--no-mount` and drive the Workspace through `tribios fs`.

## Upgrade and removal

```sh
sudo apt-get update && sudo apt-get install --only-upgrade tribios-vfs
sudo apt-get remove tribios-vfs
```

Neither touches `.tribios`.
Project data lives inside your Project, no package manager owns a path into it, and the release workflow tests both of these on every suite before it publishes.

## Versions

Package versions are `<upstream>-1~<suite>1`, for example `0.0.1-1~bookworm1`.
An upstream prerelease uses Debian's `~` ordering, so `0.2.0~beta.1-1` sorts before `0.2.0-1` and `apt-get upgrade` moves in the right direction.

## What publishes this

`.github/workflows/apt-preview.yml` runs on a release tag.
It builds one `.deb` per suite and architecture inside that suite's own container, on a runner native to that architecture, verifies each in a clean container of the same image, regenerates the signed indexes with `apt-ftparchive`, pushes the result to the `gh-pages` branch, and finally installs from the live repository on every suite.
The scripts it calls are in `packaging/apt/`, and the packaging sources are in `packaging/debian/`.
