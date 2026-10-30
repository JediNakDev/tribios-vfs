#!/usr/bin/env bash
# The install acceptance test for Amazon Linux 2023, run as root inside a clean
# amazonlinux:2023 container.
#
#   packaging/rpm/al2023/verify-install.sh --rpm <new.rpm> [--upgrade-from <old.rpm>]
#
# It asserts what issue #23 asks for: the package installs, the core workflow
# runs from the installed CLI, an upgrade keeps Project data, and removing the
# package keeps it too.
#
# The daemon runs with --no-mount. A container has no /dev/fuse unless the host
# grants it, and the mounted path is gated by the invariant tier on a real
# runner; what is under test here is the package, not the FUSE adapter.
set -Eeuo pipefail

new_rpm="" old_rpm=""
while [ $# -gt 0 ]; do
  case "$1" in
    --rpm) new_rpm="$2"; shift 2 ;;
    --upgrade-from) old_rpm="$2"; shift 2 ;;
    *) echo "unknown argument $1" >&2; exit 2 ;;
  esac
done
[ -n "$new_rpm" ] || { echo "--rpm is required" >&2; exit 2; }

fail() { echo "FAIL: $*" >&2; exit 1; }

dnf -y install git-core

project="$(mktemp -d)/project"
mkdir -p "$project"
git -c init.defaultBranch=main init -q "$project"
git -C "$project" config user.email packaging@example.com
git -C "$project" config user.name "Packaging Test"
echo "base content" > "$project/tracked.txt"
git -C "$project" add tracked.txt
git -C "$project" -c commit.gpgsign=false commit -q -m "base"

run_core_workflow() {
  tribios --project "$project" daemon start --no-mount >/dev/null
  tribios --project "$project" workspace create packaged >/dev/null
  tribios --project "$project" fs write packaged notes.txt "$1" 0 >/dev/null
  [ "$(tribios --project "$project" fs read packaged notes.txt)" = "$1" ] ||
    fail "the installed CLI could not read back what it wrote"
  tribios --project "$project" daemon stop >/dev/null
}

read_after_upgrade() {
  tribios --project "$project" daemon start --no-mount >/dev/null
  [ "$(tribios --project "$project" fs read packaged notes.txt)" = "$1" ] ||
    fail "upgrade lost Workspace data"
  tribios --project "$project" workspace list | grep -q packaged ||
    fail "upgrade lost the Workspace record"
  tribios --project "$project" daemon stop >/dev/null
}

if [ -n "$old_rpm" ]; then
  dnf -y install "$(readlink -f "$old_rpm")"
  echo "installed the previous build: $(rpm -q tribios-vfs)"
  tribios configure "$project" >/dev/null
  run_core_workflow "written before the upgrade"
  dnf -y upgrade "$(readlink -f "$new_rpm")"
  echo "upgraded to: $(rpm -q tribios-vfs)"
  read_after_upgrade "written before the upgrade"
else
  dnf -y install "$(readlink -f "$new_rpm")"
  echo "installed: $(rpm -q tribios-vfs)"
  tribios configure "$project" >/dev/null
  run_core_workflow "written after a clean install"
fi

command -v tribios >/dev/null || fail "tribios is not on PATH after install"
[ -x /usr/libexec/tribios/tribios_daemon ] || fail "the daemon is not staged in libexec"
[ "$(rpm -ql tribios-vfs | grep -c -v '^/usr/share/licenses/')" -ge 4 ] ||
  fail "the package is missing files from the install contract"

dnf -y remove tribios-vfs
[ -f "$project/.tribios/meta.db" ] || fail "removing the package deleted Project metadata"
[ -f "$project/tracked.txt" ] || fail "removing the package deleted Project files"

echo "PASS Amazon Linux 2023 package install contract"
