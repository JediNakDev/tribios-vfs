# Linux setup and operation

Tribios supports x86-64 and arm64 Linux hosts backed by ext4 or XFS.
It uses libfuse3 and mounts as the invoking user.
Do not run the daemon as root.

## Install dependencies

Debian and Ubuntu:

```sh
sudo apt-get install cmake ninja-build pkg-config libsqlite3-dev libfuse3-dev fuse3
```

Fedora and EPEL:

```sh
sudo dnf install cmake ninja-build pkgconf-pkg-config sqlite-devel fuse3-devel fuse3
```

Arch Linux:

```sh
sudo pacman -S cmake ninja pkgconf sqlite fuse3
```

## Build

```sh
cmake -S . -B build -G Ninja
ninja -C build
```

The configure output must report `FUSE 3` and `mount support enabled`.
If it reports an unmounted build, install the libfuse3 development package and configure a fresh build directory.

## Configure and mount a Project

```sh
build/tribios configure /path/to/project
build/tribios --project /path/to/project daemon start
build/tribios --project /path/to/project info
```

`tribios info` reports the mount path and `mount backend: mounted` after the kernel accepts the mount.
Workspace directories appear immediately below that path.
The mount is private to the invoking user because Tribios never enables `allow_other` by default.

Stop the daemon normally so it can unmount cleanly:

```sh
build/tribios --project /path/to/project daemon stop
```

If the daemon is already gone and a stale mount remains, unmount it as the same user:

```sh
fusermount3 -u /path/to/project/.tribios/mnt
```

## Host filesystem behavior

ext4 and XFS are the release-tested backing filesystems.
Both are case-sensitive in their ordinary configurations, so `README` and `readme` are different Workspace paths.
Tribios reports timestamps with one-second precision even when the backing filesystem records finer values.
Renames within one Workspace are atomic and crash-recovered.
Renames across Workspaces fail with `EXDEV`.
Permission bits are enforced by the kernel through `default_permissions`.
Files and directories report the invoking user's uid and gid.
Changing ownership to another user, hard links, extended attributes, special files, and advisory locks are unsupported and return `ENOTSUP`.

Keep the Project and its `.tribios` directory on storage that provides atomic rename, file `fsync`, and directory `fsync`.
Network filesystems are unsupported.

## Troubleshooting

If `/dev/fuse` is missing, load the kernel module with `sudo modprobe fuse` or expose `/dev/fuse` to the virtual machine or container.
Containers also need permission to use the device and to mount filesystems.

If Tribios reports that the invoking user cannot read and write `/dev/fuse`, check the device ownership and distribution-specific `fuse` group membership, then start a new login session after changing groups.
Do not work around this by running the daemon as root.

If the mount point is stale, run `fusermount3 -u` as shown above and start the daemon again.
If unmount reports that the target is busy, stop tools whose working directory or open files are inside the mount and retry.

If startup reports an unsafe recovery state, do not remove `.tribios` or edit `meta.db`.
Inspect the retained operation and diagnostic identifier:

```sh
build/tribios --project /path/to/project recovery inspect
```

Recovery runs before mounting on every start.
An unresolved recovery error deliberately keeps the Project unmounted.
