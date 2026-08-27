# Install only the public artifacts, and keep the daemon in libexec

Status: accepted.
Date: 2026-08-25.
Tracking issue: #15.

Every downstream packaging recipe in #16 through #19 builds from the same upstream tree and installs through the same CMake rules.
Nothing about those recipes can be verified until the upstream install output is fixed and documented, so the layout is a project decision rather than a per-packager one.

Tribios installs the `tribios` command, the `tribios_daemon` helper, `README.md`, and `LICENSE`.
Linux also installs the narrowly scoped `tribios_storage_service` beside the daemon and its systemd unit.
The command goes to `bindir`, helpers go to `libexecdir/tribios`, and the documents go to `datarootdir/doc/tribios-vfs`.
Debian and Fedora both require the license text to ship inside the binary package, which is why `LICENSE` is installed rather than merely present in the source tree.

The static libraries `tribios_core` and `tribios_control`, and the headers under `includes/`, are internal.
They are not installed.
Tribios has no consumers other than its own executables, and publishing an SDK would freeze internal interfaces that still change.
The packaging smoke test asserts that no header and no static library reaches the staging root, so an accidental SDK is a test failure rather than a compatibility promise discovered later.

`tribios_daemon` is spawned by the CLI and takes a `--project` argument that only the CLI knows how to supply.
It is not a command a person runs, so putting it on everyone's `PATH` would be wrong, and Debian and Fedora reviewers both treat a helper in `bindir` as a packaging defect.
The CLI therefore resolves the daemon in three steps: the `TRIBIOS_DAEMON` environment variable, then a sibling of the running executable, then `libexecdir/tribios` relative to the running executable's parent.
The sibling lookup keeps a build tree working unchanged.
Resolution is relative to the running executable rather than compiled in as an absolute path, because Homebrew installs into a versioned cellar directory and relocates it, and because `DESTDIR` staging must produce a tree that works after it is moved to its final root.
`TRIBIOS_DAEMON` stays first so the end-to-end tests can point at a build tree; the packaging test unsets it deliberately, which is how the libexec path gets covered.

The daemon stays per-Project.
Installing a machine-global service unit would define a service contract the product has not designed, and #15 lists that as a non-goal.

The Linux storage service is machine-global because OverlayFS mounts must live in the host mount namespace.
Packages install its systemd unit, while source installations enable it once through `tribios install-privileges`.
Its interface is restricted to mount, unmount, and Btrfs deletion for authenticated callers and validated Project paths.

Uninstall and upgrade never touch `.tribios` inside a Project.
Project data lives under the user's Project directory and is never staged by `install()`, so no package manager has a path by which to remove it.
The packaging smoke test asserts this after both a reinstall and a manifest-driven uninstall.

We rejected installing the daemon into `bindir` even though it needs no code change, because it puts a non-user-facing binary on `PATH` and creates a downstream review problem in two of the four target ecosystems.
We rejected compiling an absolute daemon path into the CLI because it breaks relocation, which Homebrew requires.
We rejected exporting a CMake package config and installing the headers, because no consumer exists and it would convert internal interfaces into a compatibility surface.
