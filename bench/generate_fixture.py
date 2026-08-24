#!/usr/bin/env python3
"""Generate the reproducible performance fixture for the Tribios prototype.

THROWAWAY PROTOTYPE - see docs/prototype/README.md.

The fixture is a Git Project of roughly 100,000 files and 2 GiB of logical data,
a large part of it ignored dependency content, so that the full-copy baseline
must reproduce the same Workspace contents.
"""

import argparse
import os
import random
import subprocess
import sys
from pathlib import Path

FILLER = ("// tribios fixture filler line to give files realistic size\n" * 8).encode()


def write_file(path: Path, size: int, seed: int) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    body = bytearray()
    body += f"// tribios fixture file {seed}\n".encode()
    while len(body) < size:
        body += FILLER
    path.write_bytes(bytes(body[:size]))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("destination")
    parser.add_argument("--files", type=int, default=100_000)
    parser.add_argument("--bytes", type=int, default=2 * 1024**3)
    parser.add_argument("--seed", type=int, default=20260824)
    parser.add_argument("--ignored-fraction", type=float, default=0.6)
    arguments = parser.parse_args()

    root = Path(arguments.destination)
    if root.exists() and any(root.iterdir()):
        print(f"{root} already exists and is not empty", file=sys.stderr)
        return 1
    root.mkdir(parents=True, exist_ok=True)
    random.seed(arguments.seed)

    average = max(64, arguments.bytes // arguments.files)
    ignored_files = int(arguments.files * arguments.ignored_fraction)
    tracked_files = arguments.files - ignored_files

    # Tracked sources.
    for index in range(tracked_files):
        module = index // 100
        write_file(root / f"src/module{module:04d}/file{index:06d}.cpp", average, index)

    # Ignored dependency and cache content: a Workspace must be able to build
    # without reinstalling it, and the full-copy baseline must reproduce it.
    for index in range(ignored_files):
        package = index // 200
        write_file(root / f"vendor/package{package:04d}/asset{index:06d}.dat", average, index)

    (root / ".gitignore").write_text("vendor/\nbuild/\nlocal.env\n")
    (root / "local.env").write_text("SECRET_TOKEN=fixture-only\n")
    (root / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.20)\n"
        "project(fixture CXX)\n"
        "file(GLOB_RECURSE fixture_sources src/module0000/*.cpp)\n"
        "add_library(fixture STATIC ${fixture_sources})\n"
    )
    for path in sorted((root / "src").glob("module0000/*.cpp")):
        path.write_text(f"int {path.stem}() {{ return 1; }}\n")

    subprocess.run(["git", "init", "--quiet", "--initial-branch=main", str(root)], check=True)
    subprocess.run(["git", "-C", str(root), "config", "user.email", "bench@example.invalid"], check=True)
    subprocess.run(["git", "-C", str(root), "config", "user.name", "Tribios Bench"], check=True)
    subprocess.run(["git", "-C", str(root), "add", "-A"], check=True)
    subprocess.run(["git", "-C", str(root), "commit", "--quiet", "-m", "fixture"], check=True)

    total = sum(f.stat().st_size for f in root.rglob("*") if f.is_file())
    print(f"fixture at {root}: {arguments.files} files, {total} bytes on disk")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
