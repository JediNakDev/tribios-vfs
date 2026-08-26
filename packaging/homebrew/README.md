# The Homebrew tap

`tap/` is the full content of the [`JediNakDev/homebrew-tap`](https://github.com/JediNakDev/homebrew-tap) repository: the formula, the tap README, and the tap's CI.
It lives here so the recipe is reviewed alongside the code it builds, and it is copied wholesale to the tap by the `update-homebrew-tap` job in `.github/workflows/release.yml`.

Edit the tap here.
Never edit the tap repository directly; the next release overwrites it.

## What a release does

1. `publish-archive` builds `tribios-vfs-<version>.tar.gz` with `git archive` and publishes it with its SHA-256.
2. `update-homebrew-tap` runs `update-formula.sh <version>`, which downloads both files, checks the archive against the published checksum, and rewrites the `url` and `sha256` lines in the formula.
3. It copies `tap/` over a clone of the tap repository and pushes.

The `url` and `sha256` in `tap/Formula/tribios-vfs.rb` therefore hold a placeholder in this repository.
That is deliberate.
A checked-in checksum would be a second source of truth for something the release already publishes, and it would be stale the moment a version is cut.

## One-time setup

The tap needs a repository and a key, both of which only a human with the account can create.

Create `JediNakDev/homebrew-tap` as a public repository.
Homebrew resolves `JediNakDev/tap` to that name.
An empty repository is enough; the first release fills it.

Then give the release workflow write access to it:

```sh
packaging/homebrew/setup-tap-deploy-key.sh
```

The script generates the deploy key, walks you through registering it on the tap with write access, stores the private half as the `HOMEBREW_TAP_DEPLOY_KEY` Actions secret, checks against GitHub's own record of the key that it is not read-only, and prints the fingerprint to record.
It never pushes, tags, or writes to a remote branch: the verification is read-only, so nothing lands on the tap until a real release does.
The key only ever exists in a temp directory, so rotating it means running the script again and deleting the old key from the tap by fingerprint.

It registers the key through the browser rather than `gh repo deploy-key add` on purpose: a key added by `gh` is tied to the CLI's own authorization, so de-authorizing the GitHub CLI later would silently delete it and break the next release.

A deploy key rather than a personal access token, because this is one workflow pushing to one repository.
The key reaches nothing but the tap, and it needs no expiry date and so no rotation.
A token with the same reach would carry the account's identity and would have to be replaced every year.

The release workflow fails with a pointer to this file if the secret is missing, rather than skipping the tap silently.

## Testing a formula change without cutting a release

```sh
brew tap JediNakDev/tap
cp packaging/homebrew/tap/Formula/tribios-vfs.rb "$(brew --repository JediNakDev/tap)/Formula/"
# Point it at a real archive first; the checked-in checksum is a placeholder.
packaging/homebrew/update-formula.sh 0.0.1 "$(brew --repository JediNakDev/tap)/Formula/tribios-vfs.rb"
brew install --build-from-source JediNakDev/tap/tribios-vfs
brew test JediNakDev/tap/tribios-vfs
```

`brew test` needs a few minutes because it builds from source.
The tap's own CI runs the same thing on a clean runner on every push.

## homebrew/core

Not a target for now.
Core formulae may not depend on a cask, Tribios needs the macFUSE cask, and core also requires a stable upstream release.
[Issue #20](https://github.com/JediNakDev/tribios-vfs/issues/20) tracks the constraint and the ways out of it.
