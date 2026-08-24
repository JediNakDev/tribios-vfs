#!/usr/bin/env bash
# Unsupported semantics fail explicitly and mutate nothing.
source "$(dirname "$0")/lib.sh"

make_project
configure_project
start_daemon
tribios workspace create limits >/dev/null
unsupported_errno="$(python3 -c 'import errno; print(errno.ENOTSUP)')"

for verb in hardlink setxattr lock mknod; do
  if out="$(tribios fs "$verb" limits docs/notes.txt extra 2>&1)"; then
    fail "$verb must fail explicitly"
  fi
  assert_contains "errno $unsupported_errno" "$out"
done

# No partial mutation was left behind by the rejected operations.
ws_exists limits extra && fail "a rejected operation must not create anything"
assert_eq "shared base content" "$(ws_read limits docs/notes.txt)" "content after rejected operations"
assert_eq "base" "$(ws_layer limits docs/notes.txt)" "a rejected operation must not force a copy-up"

# Operations on paths that do not exist fail with the ordinary errno.
if out="$(tribios fs read limits does/not/exist 2>&1)"; then
  fail "reading a missing path must fail"
fi
assert_contains "errno 2" "$out"

stop_daemon
echo "PASS unsupported operations"
