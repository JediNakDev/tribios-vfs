# JediNakDev/homebrew-tap

The Homebrew tap for [Tribios VFS](https://github.com/JediNakDev/tribios-vfs), a copy-on-write virtual filesystem for parallel agent workspaces.

This tap carries preview packages.
Tribios is at `0.y.z`, so anything may change between minor versions.

## Install

```sh
brew install --cask macfuse
brew install JediNakDev/tap/tribios-vfs
```

macFUSE has to come first.
It is a cask, and Homebrew installs casks after formulae in a single command, so a combined install would build Tribios before its FUSE headers exist.

## Approve macFUSE

macOS will not load the macFUSE system extension until you approve it by hand.
Nothing can mount until you do.

1. Open System Settings > Privacy & Security.
2. Scroll to Security and allow the system software from developer "Benjamin Fleischer".
3. Restart the Mac.
   macOS only loads the extension after a reboot.
4. Confirm it took: `tribios info` inside a configured Project reports `mount backend: mounted`.

Before approval `tribios daemon start` still runs, but it falls back to an unmounted daemon.
Workspace operations then go through `tribios fs` instead of a mounted path.

On an Apple Silicon Mac, loading a third-party kernel extension also requires Reduced Security in Recovery.
The [macFUSE documentation](https://github.com/macfuse/macfuse/wiki) covers that.

## Use

```sh
tribios configure /path/to/project
tribios daemon start --project /path/to/project
tribios workspace create feature-branch --project /path/to/project
```

`tribios help` lists the commands.
[`docs/release-and-install-contract.md`](https://github.com/JediNakDev/tribios-vfs/blob/main/docs/release-and-install-contract.md) is the compatibility surface: the installed layout, the command surface, the on-disk paths, and the migration contract.

Installing, upgrading, and uninstalling never touch the `.tribios` directory inside a Project.
Project data is yours, and no package manager owns a path into it.

## What is tested

`brew install` and `brew test` run on every push against macos-14 and macos-15, both Apple Silicon.
`brew test` drives the core workflow with `daemon start --no-mount`, because a CI runner cannot approve a system extension.

Intel macOS is not tested.
The formula should work there, but nobody has run it, so it is not a support claim.

## Linux

This tap is macOS-only, declared with `depends_on :macos`.

Tribios needs libfuse3 on Linux, and Homebrew does not supply the kernel-side FUSE device.
On Linux, build from source against the distribution's own `libfuse3-dev`:

```sh
sudo apt-get install cmake ninja-build pkg-config libsqlite3-dev libfuse3-dev fuse3
cmake -S . -B build -G Ninja && ninja -C build && sudo cmake --install build
```

Native Linux packages are tracked upstream in [#17](https://github.com/JediNakDev/tribios-vfs/issues/17), [#18](https://github.com/JediNakDev/tribios-vfs/issues/18), and [#19](https://github.com/JediNakDev/tribios-vfs/issues/19).

## Why this is not in homebrew/core

`homebrew/core` formulae may not depend on a cask, and Tribios needs the macFUSE cask for its mounted filesystem.
Core also requires a stable upstream release, and Tribios is at `0.y.z`.
[JediNakDev/tribios-vfs#20](https://github.com/JediNakDev/tribios-vfs/issues/20) tracks the constraint and the possible resolutions.

## Maintaining the formula

`Formula/tribios-vfs.rb` is generated into this repository from
[`packaging/homebrew/`](https://github.com/JediNakDev/tribios-vfs/tree/main/packaging/homebrew) upstream.
Edit it there, not here.
Publishing a Tribios release pushes the updated formula to this tap automatically.
