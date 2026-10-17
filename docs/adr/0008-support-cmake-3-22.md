# Require CMake 3.22, not 3.24

Status: accepted.
Date: 2026-10-18.
Tracking issue: #23.

The build required CMake 3.24 since the first commit.
Nothing in it needs 3.24: the newest features the build uses are `CMAKE_CXX_STANDARD 23` and `FetchContent_MakeAvailable`, which CMake 3.20 and 3.14 support, and the test tier fetches Catch2 from Git rather than a URL archive, so the 3.24 download-timestamp policy never applies.

Amazon Linux 2023 ships CMake 3.22.2 and no newer package.
Keeping 3.24 would mean building CMake from source inside the package build, which an RPM build cannot download and Amazon Linux has no package for.
The minimum is therefore 3.22, the oldest CMake that a supported build target actually ships.
The Debian control file states the same floor, and `tests/e2e/debian_packaging.sh` asserts it, so a packaging recipe cannot drift from the build.

Ubuntu 22.04 jammy also ships CMake 3.22, so the CMake version no longer excludes it.
It stays excluded because its default compiler is GCC 11, whose libstdc++ has no `<expected>`.
That is now the stated reason in `packaging/apt/suites.txt` and `docs/install/debian-ubuntu.md`.

The cost is that no pull request job builds with a CMake older than the runner's own.
The Amazon Linux 2023 workflow is the one that builds with 3.22, and it runs whenever `CMakeLists.txt` changes, so a feature that needs a newer CMake fails there before it merges.

We rejected keeping 3.24 and building Amazon Linux with a vendored CMake, because it adds a build-time download to a package build and buys nothing the code uses.
We rejected lowering the floor further than 3.22, because no build target needs it and nothing would test it.
