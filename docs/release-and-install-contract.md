# Release and install contract

This is the upstream contract every packaging recipe depends on.
The Homebrew tap, the Debian source package, the Copr spec, and the AUR `PKGBUILD` all build from the same release archive and install through the same CMake rules described here.
`docs/adr/0006-install-only-the-public-artifacts.md` records why the layout is what it is.
`docs/packaging/copr.md` records how the Fedora and EPEL preview repository builds against it.

Everything on this page is a compatibility surface.
Changing it is a versioned change, not a refactor.
Everything not on this page is internal and may change in any release.

## Versioning

Tribios follows Semantic Versioning.
It is in initial development at `0.y.z`, so the minor number carries breaking changes and no compatibility is promised across minor versions yet.

The version lives in one place, the `project()` call in `CMakeLists.txt`, and is compiled into the CLI.
`tribios version` prints `tribios <version>` and exits 0.
Release tags are `v<version>`, and the release workflow refuses a tag whose version does not match `CMakeLists.txt`.

Prerelease versions use each ecosystem's own ordering rather than a shared spelling, because no shared spelling sorts correctly everywhere: `0.2.0~beta.1-1` in Debian, `0.2.0~beta1` in RPM, `0.2.0-beta.1` upstream.

## Release artifacts

A tagged release publishes exactly two files:

- `tribios-vfs-<version>.tar.gz`, a source archive produced by `git archive` from the tag
- `tribios-vfs-<version>.tar.gz.sha256`, its SHA-256 checksum

The archive is immutable.
A published tag is never re-cut, moved, or overwritten, because downstream recipes pin the checksum and a changed archive breaks every one of them at once.
No prebuilt binaries are published; each ecosystem builds from source.

## Installed layout

`cmake --install` stages the public files below, relative to the install prefix:

```text
bin/tribios
libexec/tribios/tribios_daemon
libexec/tribios/tribios_storage_service   Linux only
lib/systemd/system/tribios-storage.service   Linux only
share/doc/tribios-vfs/README.md
share/doc/tribios-vfs/LICENSE
```

The directories follow `GNUInstallDirs`, so a packager overriding `CMAKE_INSTALL_LIBEXECDIR` or `CMAKE_INSTALL_DATAROOTDIR` gets their distribution's spelling.
Staging honours `DESTDIR`.

Internal static libraries and the headers under `includes/` are deliberately not installed.
Tribios publishes no C++ SDK and no CMake package config.

`tribios` locates `tribios_daemon` relative to the running executable, so an installed tree can be relocated after staging.
Setting `TRIBIOS_DAEMON` overrides the lookup.

The build requires CMake 3.24 or newer, a C++23 compiler, `pkg-config`, and SQLite 3.
Linux runtime support also requires the `btrfs` command when using Btrfs.
macOS uses `hdiutil`, which ships with the operating system.
Configuring with `-DTRIBIOS_BUILD_TESTS=OFF` skips the test suite, and with it the Catch2 fetch that would otherwise need network access at configure time.
Packaging recipes building in an isolated chroot use it; `packaging/aur/README.md` is the worked example.
A downstream recipe may add its distribution's own conventional files beside the four above, such as the Arch `usr/share/licenses/tribios-vfs/LICENSE` copy.

Builds have no macFUSE or libfuse dependency.
Backend availability is probed when a Project is configured, not when Tribios is compiled.

## Command surface

These command names, their options, and their output shape are stable within a minor version:

```text
tribios configure <project> [--mount <path>] [--growth-allowance-bytes <bytes>] [--force]
tribios info [--project <path>]
tribios daemon start [--project <path>]
tribios daemon stop|status [--project <path>]
tribios workspace create <name> [--branch <branch>] [--project <path>]
tribios workspace list [--project <path>]
tribios workspace status <name> [--project <path>]
tribios workspace remove <name> [--project <path>]
tribios workspace wait-reclaim [--project <path>]
tribios recovery inspect [--project <path>]
tribios install-privileges
tribios version
tribios help
```

Exit codes: 0 on success, 2 for an unknown or malformed command, 1 for any other failure.
`tribios daemon status` exits 1 when the daemon is not running, which is a status answer rather than an error.
Diagnostics go to standard error prefixed with `tribios: `; command results go to standard output.

`tribios install-privileges` is required once on Linux after installation.
It enables the narrowly scoped system service used for host-namespace OverlayFS mounts, unmounts, and privileged Btrfs deletion.

## Paths and environment

A Project is resolved from `--project`, then `TRIBIOS_PROJECT`, then the nearest configured ancestor of the working directory.

All Project state lives under `.tribios` inside the Project:

```text
.tribios/meta.db        SQLite metadata, format version 2
.tribios/base           Base-state directory or capture mount path
.tribios/storage        private images, shadows, upper trees, and reclaim paths
.tribios/staging        lifecycle staging data
.tribios/mnt            default native Workspace paths
.tribios/daemon.log     daemon output
```

The control socket lives in `/tmp` under a name derived from the Project path, because a Unix socket path inside a deep Project directory can exceed the macOS length limit.
It is ephemeral and is not Project state.

`TRIBIOS_PROJECT`, `TRIBIOS_DAEMON`, and `TRIBIOS_STORAGE_SERVICE_SOCKET` are the environment variables in the contract.
The failpoint variables are test controls and are not.

There is no machine-global configuration file, no system service unit, and no per-user configuration directory.
The daemon is per-Project and is started by the user, not by the package.

## Native filesystem semantics

Each active Workspace is a native kernel filesystem path.
The Base state and sibling Workspaces are not modified by activity in one Workspace.
APFS shadows and Btrfs snapshots share unchanged blocks.
OverlayFS uses its normal lower, upper, work, and whiteout semantics.
ADR 0007 records the storage design.

Linux release builds support x86-64 and arm64.
Btrfs storage uses writable snapshots, while supported non-Btrfs local storage uses OverlayFS.
OverlayFS mounts live in the host mount namespace and Workspace entries belong to the invoking user.
The selected filesystem defines ordinary file behavior and durability.

## Migration contract

The on-disk metadata format is versioned independently of the product version and is currently at version 2.
Tribios refuses to open a Project whose metadata format it does not recognise, and reports it rather than guessing.

A release that changes the format must either migrate an older Project forward on open or refuse it with an actionable message.
It must never silently reinterpret older data.
Downgrading across a format change is not supported.

Installing, upgrading, and uninstalling a package never create, modify, or delete `.tribios`.
Project data lives inside the user's Project, so no package manager owns a path into it.
Configuring a Project is always an explicit `tribios configure` run by the user.

## What the packaging smoke test enforces

`tests/e2e/packaging_install.sh`, in the design tier, stages an install under `DESTDIR`, asserts the platform layout above file for file, and asserts that no header, static library, FUSE binary, or FUSE dependency was staged.

It does not cover a version-to-version metadata migration.
That test arrives with the first release that changes the metadata format.
