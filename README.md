# Tribios VFS

Tribios Virtual Filesystem is a layer that run on top of FUSE (or macFUSE for macOS) that is designed to optimise parallel agent workspace.

It is named after ['Tribios'](https://honkai-star-rail.fandom.com/wiki/Tribios) a character for 'Honkai: Star Rail'.
She is a demigod (or Chrysos Heirs) that **split her soul into a thousand pieces** after taking the authority of Janus.

The project is heavily inspired by [MacOS Is Making Your Mac Slow](https://youtu.be/4wVNFaFDIn8?si=UpLJ4oilWGVCGMNL) from @t3dotgg.
tldr; APFS often struggle with modern, agentic development workflows as it introduces massive overhead when handling high volumes of small files and complex linking. He recommend moving off to Linux and use XFS combined with VDO and LZ4.

Then 2 questions rise, is combining the existing solution plus some (possibly) crazy hack really a solution for a newly introduced niche(?) problem? and do we really need to change an OS just because a filesystem sucks? This is an experiment to answer them.

## Install

**macOS**

```sh
brew install --cask macfuse
brew install JediNakDev/tap/tribios-vfs
```

macFUSE has to be installed first, because Tribios builds against its headers.

macOS will not load the macFUSE system extension until you approve it: open System Settings > Privacy & Security, allow the system software from developer "Benjamin Fleischer", then restart the Mac.

**Debian and Ubuntu**

```sh
sudo apt install tribios-vfs
```

**Fedora**

```sh
sudo dnf copr enable jedinakdev/tribios-vfs
sudo dnf install tribios-vfs
```

**Arch Linux**

```sh
yay -S tribios-vfs
```

## Running

Verify the installation:

```sh
tribios version
```

Configure an existing Git project and start its daemon:

```sh
cd /path/to/project
tribios configure .
tribios daemon start
tribios info
```

`tribios configure` captures the project's current files as an immutable Base state, including ignored and untracked files.
Review the warning it prints before continuing.
Run `tribios info` and confirm that it reports `mount backend: mounted`.

Create a Workspace for an agent, then work inside its mounted directory:

```sh
tribios workspace create agent-one
tribios workspace list
cd .tribios/mnt/agent-one
```

The Workspace is a normal Git working tree on a branch named `agent-one`.
Pass `--branch <branch>` to `workspace create` to use a different branch name.
Use `tribios workspace create` and `tribios workspace remove` instead of `git worktree add` and `git worktree remove` because Tribios owns the mounted worktree state.

Remove the Workspace and stop the daemon when you are done:

```sh
cd /path/to/project
tribios workspace remove agent-one
tribios workspace wait-reclaim
tribios daemon stop
```

Workspace removal returns after the Workspace disappears.
`tribios workspace wait-reclaim` waits for its storage to be reclaimed in the background and is optional.
