# Linux setup and operation

Tribios supports x86-64 and arm64 Linux hosts on local storage.
It uses writable Btrfs snapshots when the Project is on Btrfs and kernel OverlayFS otherwise.
There is no FUSE fallback.

## Install dependencies

Debian and Ubuntu:

```sh
sudo apt-get install cmake ninja-build pkg-config libsqlite3-dev btrfs-progs git
```

Fedora and EPEL:

```sh
sudo dnf install cmake ninja-build pkgconf-pkg-config sqlite-devel btrfs-progs git-core
```

Arch Linux:

```sh
sudo pacman -S cmake ninja pkgconf sqlite btrfs-progs git
```

## Build and install

```sh
cmake -S . -B build -G Ninja
ninja -C build
sudo cmake --install build
sudo tribios install-privileges
```

The final command enables and starts the installed systemd storage service.
The service authenticates callers with Unix peer credentials and validates each private path against the caller's owned Project `.tribios` directory.
Normal Workspace commands stay unprivileged and never prompt for a password.

## Configure a Project

```sh
tribios configure /path/to/project
tribios --project /path/to/project daemon start
tribios --project /path/to/project info
```

Configuration probes both creation and destruction before recording a backend.
Btrfs is selected only when snapshot creation and deletion both work.
OverlayFS is selected only when a host-namespace mount and unmount both work.
Failure names the missing capability and leaves no configured Project database.

Every active Workspace appears below the configured mount path, which defaults to `.tribios/mnt`.
Editors, language servers, and other shells see the same path because OverlayFS is mounted in the host namespace.

## Troubleshooting

If Btrfs snapshot creation works but deletion fails, run `sudo tribios install-privileges` or mount the filesystem with `user_subvol_rm_allowed`.
If OverlayFS probing fails, confirm that the kernel enables OverlayFS and that `tribios-storage.service` is active.
Do not run the daemon as root.

If startup reports an unsafe recovery state, do not edit `.tribios/meta.db` or delete storage by hand.
Inspect the retained operation and diagnostic identifier:

```sh
tribios --project /path/to/project recovery inspect
```
