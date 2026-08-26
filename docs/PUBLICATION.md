# Publishing a release

Tagging is not publishing.
A tag publishes the source archive and the APT repository on its own.
Everything else waits for a person.

Read this before you tag, not after.

## What a tag does by itself

Pushing a `v*` tag starts two workflows, and nothing else in this repository reacts to a tag.

`.github/workflows/release.yml` refuses the tag if its version disagrees with `CMakeLists.txt`, then builds `tribios-vfs-<version>.tar.gz` with `git archive` and publishes it, plus its SHA-256, as a prerelease.
Every other channel builds from that archive, so it has to land first.

`.github/workflows/apt-preview.yml` builds one `.deb` per suite in `packaging/apt/suites.txt`, installs each one in a clean container of the same image, regenerates the signed repository, pushes it to `gh-pages`, and finishes by installing from the live repository the way a user would.
When the last job is green, `apt-get install tribios-vfs` works on every listed suite.
If it is red, nothing was published: the verification runs before the push.

The APT channel needs the signing key, the two repository secrets, the `gh-pages` branch and Pages, all of which are already set up.
`packaging/apt/setup-preview-channel.sh` is how they were created, if a fork or a key rotation ever needs them again.

## What you do afterwards

### Watch the two workflows

```sh
gh run list -L5
gh run watch "$(gh run list -w apt-preview.yml -L1 --json databaseId -q '.[0].databaseId')"
```

The APT run takes a while because it builds and verifies each suite in its own container.
Let it finish before touching the other channels.
A failure there usually means the packaging is wrong for one suite, and the same mistake is likely waiting in the others.

### Copr, for Fedora and EPEL

Copr does not follow tags.
The wizard and the spec arrive with issue #18, so this step only exists once that branch is on `main`.

```sh
./packaging/publish-copr-preview.sh
```

The wizard verifies the published archive against its checksum, creates or reuses the project, points the package at the tag, builds, runs the container acceptance for every chroot matching your architecture, and prints the project URL, repository slug and build ID.
`docs/packaging/copr.md` has the manual equivalent and the reasoning behind the chroot list.

### Homebrew and the AUR

Neither is in the repository yet.
Homebrew is issue #16, the AUR recipe is issue #19, and their steps belong in this file when they land.
Until then, a release publishes to APT and Copr only, and saying otherwise anywhere public would be a lie a user finds out about at install time.

### Announce nothing until the installs are green

Each channel proves itself by installing in a clean environment.
Wait for all of them, then update whatever points users at a version.

## If something has to be fixed after tagging

Do not move the tag.
Downstream recipes pin the archive checksum, and re-cutting a tag breaks every one of them at once, including copies you cannot see.

Fix the problem, bump the patch version in `CMakeLists.txt`, and tag again.
The cost of a wasted version number is nothing.
The cost of a mutated release is a user whose checksum no longer matches and who has no way to tell a fix from an attack.

## Version spelling

The upstream version is SemVer, `0.2.0-beta.1`.
Each ecosystem spells a prerelease its own way, because no single spelling sorts correctly everywhere.

| Channel | Spelling | Sorts before |
| --- | --- | --- |
| Upstream tag | `v0.2.0-beta.1` | |
| Debian | `0.2.0~beta.1-1~bookworm1` | `0.2.0-1` |
| RPM | `0.2.0~beta1` | `0.2.0` |

Get this wrong in the obvious direction, spelling it `0.2.0-beta1` in RPM, and the beta sorts *after* the release, which strands every tester on the prerelease with no upgrade path.
