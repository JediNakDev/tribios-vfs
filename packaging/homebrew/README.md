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

Then give the release workflow write access to it with a deploy key:

```sh
ssh-keygen -t ed25519 -N "" -f /tmp/tap-key -C "tribios-vfs release workflow"
ssh-keygen -lf /tmp/tap-key.pub          # record this fingerprint
```

Add `/tmp/tap-key.pub` at `https://github.com/JediNakDev/homebrew-tap/settings/keys/new`, titled `tribios-vfs release workflow`, with **Allow write access** ticked.
Without write access the release fails at the push rather than at the clone.
Add it from that page rather than with `gh repo deploy-key add`: a key added by `gh` is tied to the CLI's own authorization, so de-authorizing the GitHub CLI later would silently delete it and break the next release.

Then store the private half upstream and delete both copies:

```sh
gh secret set HOMEBREW_TAP_DEPLOY_KEY --repo JediNakDev/tribios-vfs < /tmp/tap-key
gh api repos/JediNakDev/homebrew-tap/keys --jq '.[] | [.title, .read_only] | @tsv'
rm /tmp/tap-key /tmp/tap-key.pub
```

The second command is the check that matters: `read_only` must be `false`.
Asking GitHub's record is a better answer than a test push, and it leaves no throwaway ref on the tap.
Rotating the key means repeating this and deleting the old key from the tap by its fingerprint.

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
