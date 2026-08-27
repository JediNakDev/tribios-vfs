# Publishing Tribios VFS through package managers

Research for [issue #14](https://github.com/JediNakDev/tribios-vfs/issues/14), current as of 2026-08-25.

## Recommendation

Start the packaging work now, but do not publish the current throwaway prototype as a normal user release.
The first public packages should be clearly marked preview releases from project-controlled channels, using `0.y.z` versions and explicit compatibility warnings.
Do not wait for the internal architecture to become stable.
Wait only until there is a production-shaped build that installs cleanly, runs its core workflow, upgrades without losing user data, and has a documented public compatibility surface.

Official distribution repositories should come later.
They add review, maintenance, and compatibility commitments that make sense after Tribios has real users and a release process.
Homebrew's official `homebrew/core` has an additional macFUSE dependency problem that must be resolved before submission.

## Package managers are not publication targets

`apt`, `dnf`/`yum`, and `pacman` are repository clients, not single global registries.
They install packages from repositories configured by the operating system or user.
APT reads distribution sources and authenticates repository release metadata, DNF reads repository definitions from `/etc/yum.repos.d`, and pacman reads repository sections from `pacman.conf`. [APT authentication](https://manpages.debian.org/testing/apt/apt-secure.8.en.html) [DNF configuration](https://dnf.readthedocs.io/en/latest/conf_ref.html) [pacman configuration](https://man.archlinux.org/man/pacman.conf.5.en)

Issue #14 therefore combines distinct jobs:

| Ecosystem | Project-controlled route | Official route |
| --- | --- | --- |
| `apt` | Publish signed `.deb` repositories for named Debian and Ubuntu suites, or use a Launchpad PPA for Ubuntu previews. | Submit to Debian through ITP, sponsorship, and NEW review, then use Debian-to-Ubuntu sync where applicable or request Ubuntu sponsorship. |
| Homebrew | Maintain a `JediNakDev/homebrew-tap` formula. | Submit a stable formula to `homebrew/core` after meeting its acceptance rules and resolving the macFUSE dependency issue. |
| `dnf` / `yum` | Use Fedora Copr for Fedora and EPEL-targeted previews, or host a signed RPM repository. | Complete Fedora package review, then maintain Fedora and appropriate EPEL branches. |
| `pacman` | Host a signed binary repository generated with `repo-add`. | An Arch Package Maintainer must adopt and maintain the package in an official repository. |
| AUR / `yay` | Publish a `PKGBUILD` and `.SRCINFO` to the AUR Git repository. | There is no separate `yay` registry. AUR popularity and Package Maintainer support may eventually lead to the Arch `extra` repository. |

## Proper route by ecosystem

### APT

The practical preview route is a project-owned, signed APT repository with separate suites for each distribution release that Tribios actually tests.
Debian's third-party repository guidance says a suite should correspond to the target Debian release when binaries are built for a specific suite, and recommends scoping the signing key with `Signed-By` instead of adding a globally trusted key. [Debian third-party repository guidance](https://wiki.debian.org/DebianRepository/UseThirdParty)
Tools such as `apt-ftparchive` generate the `Packages`, `Sources`, and `Release` indexes that APT consumes. [apt-ftparchive manual](https://manpages.debian.org/testing/apt-utils/apt-ftparchive.1.en.html)

A Launchpad PPA is a good Ubuntu-only preview channel.
Launchpad accepts signed source packages, builds the binaries for selected Ubuntu suites and architectures, and hosts the result as an APT repository.
It does not accept prebuilt `.deb` uploads. [Launchpad PPA reference](https://documentation.ubuntu.com/launchpad/user/reference/packaging/ppas/ppa/) [PPA upload guide](https://documentation.ubuntu.com/launchpad/user/how-to/packaging/ppa-package-upload/)

One `.deb` must not be advertised as universally compatible with Debian, Ubuntu, Linux Mint, Pop!_OS, Kali Linux, and Raspberry Pi OS merely because all use APT.
Debian packages declare architecture and dependency metadata, while Launchpad warns that sources from another Debian-compatible distribution may fail when dependencies cannot be resolved. [Debian control fields](https://www.debian.org/doc/debian-policy/ch-controlfields.html) [PPA cross-distribution warning](https://documentation.ubuntu.com/launchpad/user/how-to/packaging/ppa-package-upload/)
Build and test Debian and Ubuntu suites directly, then treat derivative distributions as separate support claims backed by installation and workflow tests.

The official Debian route starts with an Intent To Package bug, followed by a policy-compliant source package, upload to mentors, a Request For Sponsorship, sponsor review, and manual NEW review for the first upload. [Debian mentors process](https://mentors.debian.net/intro-maintainers/)
Ubuntu can automatically sync packages newly accepted into Debian before its import freeze, while a direct Ubuntu addition needs sponsorship and archive review. [Ubuntu new-package guidance](https://documentation.ubuntu.com/project/staging/new-packages/) [Ubuntu sponsorship guidance](https://documentation.ubuntu.com/project/contributors/uploading/find-a-sponsor/)

APT does not require Tribios to have a stable API before any package can exist.
Debian provides `experimental` specifically for software that is too unstable for `unstable`, and Debian version ordering uses `~` for prereleases, such as `0.2.0~beta.1-1` sorting before `0.2.0-1`. [Debian experimental guidance](https://www.debian.org/doc/manuals/developers-reference/resources) [Debian version comparison](https://www.debian.org/doc/manuals/debian-handbook/sect.manipulating-packages-with-dpkg.en.html)

### Homebrew

Use a project tap for preview releases.
Homebrew documents taps as Git repositories that anyone can create, recommends a `homebrew-` repository name, and lets users install a formula by its fully qualified tap name. [Homebrew tap guide](https://docs.brew.sh/How-to-Create-and-Maintain-a-Tap)
The formula should build from an immutable, checksummed release archive, declare all build and runtime dependencies, install through the upstream install rules, and include a functional `test do` block. [Homebrew formula cookbook](https://docs.brew.sh/Formula-Cookbook)

Do not submit a preview to `homebrew/core`.
Core requires upstream to identify a version as stable, provide an immutable tag or release, use a compatible open-source license, and build without downstream-only patches.
Software without a stable release is not eligible. [Homebrew acceptable formulae](https://docs.brew.sh/Acceptable-Formulae)

The current macOS design creates a second obstacle.
Tribios requires macFUSE for its mounted filesystem, macFUSE is distributed as a Homebrew cask, and `homebrew/core` formulae may not depend on a cask or proprietary software. [Tribios prototype platform requirements](../prototype/README.md) [macFUSE cask](https://formulae.brew.sh/cask/macfuse) [Homebrew acceptable formulae](https://docs.brew.sh/Acceptable-Formulae)
This makes `homebrew/core` a poor fit for the product as currently designed.
A project tap can support the preview and document the required macFUSE installation and system approval.
Before seeking core inclusion, Tribios should either remove that required cask dependency, split a genuinely useful cask-independent component, or obtain an explicit policy-compatible packaging design from Homebrew maintainers.

### DNF and YUM

Use Fedora Copr for preview RPMs.
Copr is Fedora's build service for third-party repositories, accepts source RPMs, spec files, or source-control builds, supports selected build chroots, and automatically publishes successful builds to a DNF/YUM repository. [Copr documentation](https://docs.copr.fedorainfracloud.org/) [Copr user guide](https://docs.copr.fedorainfracloud.org/user_documentation.html)
RPM has explicit prerelease ordering, so a package version such as `0.2.0~beta1` sorts before `0.2.0`. [RPM version manual](https://rpm-software-management.github.io/rpm/man/rpm-version.7)

The official Fedora route requires a spec and source RPM that comply with Fedora's packaging and licensing rules, followed by package review and ongoing maintenance. [Fedora packaging guidelines](https://docs.fedoraproject.org/en-US/packaging-guidelines/) [Fedora package lifecycle](https://fedoraproject.org/wiki/Fedora_Package_Lifecycle_notes)
After Fedora inclusion, appropriate EPEL branches are the normal route to RHEL and compatible enterprise distributions. [EPEL documentation](https://docs.fedoraproject.org/en-US/epel/)

Do not treat all RPM systems as one binary target.
Fedora's own guidelines distinguish Fedora and EPEL and prohibit Fedora spec conditionals for arbitrary derivatives. [Fedora packaging guidelines](https://docs.fedoraproject.org/en-US/packaging-guidelines/)
Amazon Linux 2023 is especially separate: AWS states that no EPEL version is binary-compatible with AL2023 and that AL2023 has an independent lifecycle and a mixture of Fedora, CentOS Stream, and independent components. [Amazon Linux EPEL compatibility](https://docs.aws.amazon.com/linux/al2023/ug/al2023-ug.pdf) [Amazon Linux relationship to Fedora](https://docs.aws.amazon.com/linux/al2023/ug/relationship-to-fedora.html)
Tribios should build and test an Amazon Linux RPM in an Amazon Linux environment rather than re-label an EPEL binary.

### Pacman, the AUR, and yay

For a project-owned binary channel, build packages with `makepkg`, sign them, generate the repository database with `repo-add`, and publish a `pacman.conf` repository stanza.
The pacman manual documents custom repositories and defaults to requiring trusted package signatures while treating repository database signatures as optional through `Required DatabaseOptional`. [repo-add manual](https://man.archlinux.org/man/repo-add.8.en) [pacman configuration](https://man.archlinux.org/man/pacman.conf.5.en)

The easier public preview route is the AUR.
The AUR stores Git repositories containing `PKGBUILD` recipes and related metadata, not binary packages.
A source build of a fixed release should use the repository-aligned `tribios-vfs` name, a moving Git build should use `tribios-vfs-git`, and a prebuilt upstream binary should use `tribios-vfs-bin`. [AUR submission guidelines](https://wiki.archlinux.org/title/AUR_submission_guidelines) [Arch VCS package guidelines](https://wiki.archlinux.org/title/VCS_package_guidelines)
Tribios should initially publish only the fixed-release `tribios-vfs` recipe unless there is real demand for a moving development package.

There is nothing to publish to `yay`.
Yay identifies itself as an AUR helper and downloads PKGBUILDs from the AUR, so an AUR package automatically becomes discoverable to yay users. [yay repository](https://github.com/Jguer/yay)
The Arch project warns that AUR helpers and AUR packages are unsupported, and Arch packages may enter the official `extra` repository only when they have community interest and Package Maintainer support. [Arch User Repository](https://wiki.archlinux.org/title/Arch_User_Repository)
Manjaro and EndeavourOS support should therefore be validated separately instead of inferred from Arch compatibility.

## Current repository blockers

The repository at commit `708eba5a14177082adc1d7fbe076fd0d3181e63f` is not ready for a public preview package:

- The user-visible help and documentation call the code a throwaway prototype, and the prototype document says it is not the production implementation. [prototype documentation](../prototype/README.md)
- [`CMakeLists.txt`](../../CMakeLists.txt) defines `tribios_vfs_prototype` without a project version.
- [`CMakeLists.txt`](../../CMakeLists.txt) has no `install()` rules, so packagers have no upstream-controlled staging layout.
CMake explicitly describes `DESTDIR` as the mechanism commonly used by packagers to stage installed files. [CMake DESTDIR documentation](https://cmake.org/cmake/help/latest/envvar/DESTDIR.html)
- The repository has no `LICENSE` or `COPYING` file.
This blocks responsible publication and official review because Homebrew requires a compatible open-source license, Debian NEW reviews copyright and DFSG status, Fedora requires an allowed and declared license, and Arch packaging records the source license. [Homebrew acceptable formulae](https://docs.brew.sh/Acceptable-Formulae) [Debian mentors process](https://mentors.debian.net/intro-maintainers/) [Fedora packaging guidelines](https://docs.fedoraproject.org/en-US/packaging-guidelines/) [PKGBUILD manual](https://man.archlinux.org/man/PKGBUILD.5.en)
- The GitHub repository has no [release tags](https://github.com/JediNakDev/tribios-vfs/tags) or [releases](https://github.com/JediNakDev/tribios-vfs/releases).
Homebrew core expressly requires an immutable release, and reproducible recipes in every ecosystem benefit from a fixed source archive and checksum. [Homebrew acceptable formulae](https://docs.brew.sh/Acceptable-Formulae) [Semantic Versioning 2.0.0](https://semver.org/)
- [Issue #3](https://github.com/JediNakDev/tribios-vfs/issues/3) defines production Linux support through libfuse3 and explicitly blocks v1.0.0 until that support is verified.
The Linux adapter now targets libfuse3 while macOS keeps the API supplied by macFUSE. [platform decision](../adr/0003-prototype-runs-on-macos-and-linux.md)

Before the first preview, the upstream install contract should install only `tribios`, `tribios_daemon`, documentation, and required support files.
It should not expose the internal C++ headers or static libraries as an SDK, auto-configure a Project, delete `.tribios` data during upgrade or uninstall, or invent a machine-global service contract.
The daemon is currently per-Project, so packaging should preserve that model until the product defines a different public contract.

## When to publish

Semantic Versioning does not say that software must wait for a stable API before publication.
It defines `0.y.z` as initial development where anything may change, and says `1.0.0` defines the public API. [Semantic Versioning 2.0.0](https://semver.org/)

For Tribios, the public compatibility surface is broader than C++ headers.
It includes CLI command names, options, output and exit behavior; configuration and Project paths; on-disk metadata; mounted filesystem semantics; and the upgrade, migration, recovery, and uninstall contract.
Those surfaces may evolve during `0.y.z`, but each release should document important incompatibilities and never silently destroy user data.

Internal C++ architecture does not need to be stable before either preview or `1.0.0`.
It can change behind the public surface at any time.
The gate for `1.0.0` is confidence that the public surface and migration contract can be maintained, not confidence that the private module graph will never change.

Use three stages:

1. Work on packaging now.
Add the license, project version, install rules, immutable release automation, package smoke tests, and per-platform dependency declarations while production work proceeds.
2. Publish preview packages after a production-shaped `0.1.0` or `0.2.0` passes clean install, core workflow, upgrade, and uninstall-preserves-data tests on each claimed platform.
Use the Homebrew tap, PPA or signed APT repository, Copr, and AUR.
Label them preview quality and support only the exact operating-system releases tested.
3. Pursue official repositories after the preview has users, repeatable releases, and a maintainer willing to follow each downstream's update policy.
Submit `homebrew/core` only after a stable upstream release and resolution of the macFUSE rule.
Reserve `1.0.0` for the compatibility contract already tracked by [issue #2](https://github.com/JediNakDev/tribios-vfs/issues/2), including Linux support from issue #3.

## Split issue #14

Keep #14 as a distribution epic and split implementation into independently verifiable child issues:

1. Define the upstream release and install contract, including the license, version, immutable source archive, checksums, install layout, upgrade safety, and packaging smoke tests.
2. Publish the Homebrew preview tap and record the path to eventual core eligibility.
3. Publish Debian and Ubuntu preview packages through a signed APT repository or PPA, with an explicit tested-suite matrix.
4. Publish Fedora and EPEL preview RPMs through Copr, with Amazon Linux tracked as a separate build target.
5. Publish and maintain the Arch AUR recipe, with a binary pacman repository deferred until demand justifies operating one.
6. Track official Debian/Ubuntu, Fedora/EPEL, Homebrew core, and Arch repository promotion separately, because each depends on external review and a long-term downstream maintainer.

This split prevents one checkbox from hiding several unrelated release systems.
It also lets preview distribution finish without pretending that external official-repository reviews are under the project's direct control.
