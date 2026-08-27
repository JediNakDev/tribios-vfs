# JediNakDev/homebrew-tap

This is the Homebrew tap for [Tribios VFS](https://github.com/JediNakDev/tribios-vfs).
Tribios creates native copy-on-write Workspaces for parallel coding agents.

## Install

```sh
brew install JediNakDev/tap/tribios-vfs
```

Tribios uses APFS sparse images and shadows through tools included with macOS.
It does not need macFUSE, a kernel extension, a reboot, or Reduced Security.

## Use

```sh
tribios configure /path/to/project
tribios daemon start --project /path/to/project
tribios workspace create feature-branch --project /path/to/project
```

`tribios help` lists the commands.
The [release and install contract](https://github.com/JediNakDev/tribios-vfs/blob/main/docs/release-and-install-contract.md) documents the installed layout, command surface, on-disk paths, and migration rules.

Installing, upgrading, and uninstalling never touch a Project's `.tribios` directory.

## Linux

This tap is macOS-only.
Use the distribution packages documented upstream for Linux.

## Maintaining the formula

`Formula/tribios-vfs.rb` is generated from the upstream `packaging/homebrew/` directory.
Edit it upstream, not in the tap repository.
