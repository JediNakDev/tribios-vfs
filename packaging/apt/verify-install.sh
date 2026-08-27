#!/usr/bin/env bash
# The install acceptance test for one suite, run inside a clean container.
#
#   packaging/apt/verify-install.sh --deb <new.deb> [--upgrade-from <old.deb>]
#   packaging/apt/verify-install.sh --from-repo <base-url> --key <key.asc> --suite <suite>
#
# It asserts what issue #17 asks for: the package installs, the core workflow
# runs from the installed CLI, an upgrade between two preview versions keeps
# Project data, and removing the package keeps it too.
#
# The daemon runs with --no-mount. A container has no /dev/fuse unless the host
# grants it, and the mounted path is already gated by the invariant tier on a
# real runner; what is under test here is the package, not the FUSE adapter.
set -Eeuo pipefail

new_deb="" old_deb="" base_url="" key_file="" suite=""
while [ $# -gt 0 ]; do
  case "$1" in
    --deb) new_deb="$2"; shift 2 ;;
    --upgrade-from) old_deb="$2"; shift 2 ;;
    --from-repo) base_url="$2"; shift 2 ;;
    --key) key_file="$2"; shift 2 ;;
    --suite) suite="$2"; shift 2 ;;
    *) echo "unknown argument $1" >&2; exit 2 ;;
  esac
done

fail() { echo "FAIL: $*" >&2; exit 1; }

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends git ca-certificates

install_from_repo() {
  [ -n "$key_file" ] && [ -n "$suite" ] || fail "--from-repo needs --key and --suite"
  install -D -m 0644 "$key_file" /etc/apt/keyrings/tribios-vfs.asc
  echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/tribios-vfs.asc] $base_url $suite main" \
    > /etc/apt/sources.list.d/tribios-vfs.list
  apt-get update
  apt-get install -y tribios-vfs
}

install_local_deb() {
  apt-get install -y "$(readlink -f "$1")"
}

# --- a Project with real data, created before any upgrade -------------------

project="$(mktemp -d)/project"
mkdir -p "$project"
git -c init.defaultBranch=main init -q "$project"
git -C "$project" config user.email packaging@example.com
git -C "$project" config user.name "Packaging Test"
echo "base content" > "$project/tracked.txt"
git -C "$project" add tracked.txt
git -C "$project" -c commit.gpgsign=false commit -q -m "base"

run_core_workflow() {
  local marker="$1"
  tribios --project "$project" daemon start --no-mount >/dev/null
  tribios --project "$project" workspace create packaged >/dev/null
  tribios --project "$project" fs write packaged notes.txt "$marker" 0 >/dev/null
  [ "$(tribios --project "$project" fs read packaged notes.txt)" = "$marker" ] ||
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

# --- install, or upgrade from the previous preview --------------------------

if [ -n "$old_deb" ]; then
  install_local_deb "$old_deb"
  echo "installed the previous preview: $(tribios version)"
  tribios configure "$project" >/dev/null
  run_core_workflow "written before the upgrade"
fi

if [ -n "$base_url" ]; then
  install_from_repo
elif [ -n "$new_deb" ]; then
  install_local_deb "$new_deb"
else
  fail "one of --deb or --from-repo is required"
fi

echo "installed: $(tribios version)"
command -v tribios >/dev/null || fail "tribios is not on PATH after install"
[ -x /usr/libexec/tribios/tribios_daemon ] || fail "the daemon is not staged in libexec"

if [ -n "$old_deb" ]; then
  read_after_upgrade "written before the upgrade"
else
  tribios configure "$project" >/dev/null
  run_core_workflow "written after a clean install"
fi

# --- removal keeps Project data ---------------------------------------------

apt-get remove -y tribios-vfs
[ -f "$project/.tribios/meta.db" ] || fail "removing the package deleted Project metadata"
[ -f "$project/tracked.txt" ] || fail "removing the package deleted Project files"

echo "PASS package install contract${suite:+ on $suite}"
