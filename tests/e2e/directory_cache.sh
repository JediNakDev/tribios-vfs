#!/usr/bin/env bash
# Cached listings must reflect mutations through the public filesystem seam.
source "$(dirname "$0")/lib.sh"

make_project
mkdir -p "$PROJECT/cache-check/empty" "$PROJECT/cache-check/nested"
printf base > "$PROJECT/cache-check/old.txt"
printf nested > "$PROJECT/cache-check/nested/file.txt"
configure_project
start_daemon
tribios workspace create cached >/dev/null

listing() { tribios fs ls cached "${1:-cache-check}" | sort; }
assert_eq "empty
nested
old.txt" "$(listing)" "initial listing"
tribios fs create cached cache-check/created.txt >/dev/null
assert_eq "created.txt
empty
nested
old.txt" "$(listing)" "listing after create"
tribios fs mkdir cached cache-check/new-directory >/dev/null
tribios fs symlink cached created.txt cache-check/created.link >/dev/null
assert_eq "created.link
created.txt
empty
nested
new-directory
old.txt" "$(listing)" "listing after mkdir and symlink"
tribios fs mv cached cache-check/created.txt cache-check/renamed.txt >/dev/null
tribios fs rm cached cache-check/old.txt >/dev/null
tribios fs rmdir cached cache-check/empty >/dev/null
assert_eq "created.link
nested
new-directory
renamed.txt" "$(listing)" "listing after rename and removals"

assert_eq file.txt "$(listing cache-check/nested)" "cached descendant"
tribios fs mv cached cache-check/nested cache-check/moved >/dev/null
if tribios fs ls cached cache-check/nested >/dev/null 2>&1; then
  fail "a renamed directory must not retain a cached listing"
fi
assert_eq file.txt "$(listing cache-check/moved)" "renamed descendant"
tribios fs write cached cache-check/moved/new.txt upper 0 >/dev/null
assert_eq "file.txt
new.txt" "$(listing cache-check/moved)" "listing after copy-up"
stop_daemon
echo "PASS directory cache"
