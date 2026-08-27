# Tribios VFS

Tribios VFS creates persistent, isolated Workspaces for parallel coding agents.
Each Workspace is an ordinary filesystem path backed by native copy-on-write storage.

The name comes from [Tribios](https://honkai-star-rail.fandom.com/wiki/Tribios), a Honkai: Star Rail character who split her soul into a thousand pieces.
That is roughly the joke: one Project, many independent working copies.

## Install

### macOS

```sh
brew install JediNakDev/tap/tribios-vfs
```

Tribios uses APFS sparse images and shadows through tools included with macOS.
It does not require macFUSE, a kernel extension, or Reduced Security.

### Debian and Ubuntu

```sh
sudo apt install tribios-vfs
tribios install-privileges
```

### Fedora

```sh
sudo dnf copr enable jedinakdev/tribios-vfs
sudo dnf install tribios-vfs
tribios install-privileges
```

### Arch Linux

An AUR package is planned after the registry is available again.

### From source

```sh
cmake -S . -B build -G Ninja
ninja -C build
sudo cmake --install build --prefix /usr/local
```

The build needs CMake 3.24 or newer, Ninja, a C++23 compiler, and SQLite with its development headers.
On Linux the install stages the Workspace storage service and its systemd unit next to the CLI, which is what the packages above install for you.

## Workspace storage privileges on Linux

Linux mounts every Workspace in the host mount namespace, which an unprivileged process cannot do.
A small system service performs the mounting, unmounting, and privileged Btrfs deletion for the calling user.
It authenticates callers through Unix peer credentials and accepts paths only below a Project directory the caller owns.

Enable it once per machine, after installing Tribios:

```sh
tribios install-privileges
```

The command enables and starts `tribios-storage.service` through `sudo systemctl`, so it asks for a password once.
Ordinary Workspace commands never do.
Until it runs, `tribios configure` reports that no supported Workspace storage backend is available.

A Project also has to live on a filesystem one of the Linux backends supports: Btrfs, which Tribios prefers, or any local filesystem the kernel accepts as an OverlayFS upper layer, such as ext4 or XFS.

macOS needs no equivalent step, because its APFS sparse images and shadows need no privilege beyond the user's own.

## Use

Configure an existing Git Project and start its daemon:

```sh
cd /path/to/project
tribios configure .
tribios daemon start
tribios info
```

Configuration captures the Project's current regular files, directories, and symlinks as one immutable Base state.
Ignored and untracked files are included, so read the secrets warning before continuing.
Tribios selects and records one storage backend for the Project.
Pass `--growth-allowance-bytes <bytes>` during configuration when the default capacity is too small for the Project's build and dependency trees.

Create a Workspace and enter its native path:

```sh
tribios workspace create agent-one
tribios workspace list
cd .tribios/mnt/agent-one
```

The Workspace is a normal Git working tree on a branch named `agent-one`.
Pass `--branch <branch>` to choose another branch name.
Use Tribios instead of `git worktree add` and `git worktree remove` because Tribios coordinates Git state with native storage lifecycle state.

Check writable capacity before a large build:

```sh
tribios workspace status agent-one
```

Remove the Workspace when finished:

```sh
cd /path/to/project
tribios workspace remove agent-one
tribios workspace wait-reclaim
```

Removal returns after the Workspace path becomes inaccessible.
Physical reclamation continues in the background, and `workspace wait-reclaim` waits for it when needed.
