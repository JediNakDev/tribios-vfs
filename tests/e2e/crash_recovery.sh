#!/usr/bin/env bash
# Recovery settles interrupted filesystem and Workspace lifecycle operations.
source "$(dirname "$0")/lib.sh"

crash_during() { # failpoint command...
  local failpoint="$1"
  shift
  stop_daemon
  export TRIBIOS_FAILPOINT="$failpoint"
  start_daemon
  "$@" >/dev/null 2>&1 || true
  if tribios daemon status >/dev/null 2>&1; then
    fail "$failpoint did not terminate the daemon"
  fi
  unset TRIBIOS_FAILPOINT
  start_daemon
}

remove_workspace_and_wait() {
  tribios workspace remove "$1" >/dev/null
  tribios workspace wait-reclaim
}

make_project
configure_project
start_daemon
tribios workspace create interrupted >/dev/null
tribios workspace create sibling >/dev/null
ws_write interrupted docs/notes.txt "private before unlink"
crash_during unlink.after_upper_remove tribios fs rm interrupted docs/notes.txt

ws_exists interrupted docs/notes.txt &&
  fail "an interrupted unlink exposed the Base-state file after recovery"
assert_eq "shared base content" "$(ws_read sibling docs/notes.txt)" "sibling isolation after recovery"

# A prepared write rolls back, while a published write finishes to one whole
# new version. Neither recovery outcome can splice Base-state and staged bytes.
ws_write interrupted prepared.txt "old"
crash_during write.after_journal tribios fs write interrupted prepared.txt new 0
assert_eq "old" "$(ws_read interrupted prepared.txt)" "prepared write rollback"

ws_write interrupted published.txt "old"
crash_during write.after_publish tribios fs write interrupted published.txt new 0
assert_eq "new" "$(ws_read interrupted published.txt)" "published write recovery"

# Every staged mutation rolls back while prepared and completes once its
# publishing intent is durable.
ws_mkdir interrupted matrix
for boundary in after_journal after_stage_flush before_publish after_publish after_metadata_commit; do
  suffix="${boundary//_/-}"
  expect_new=0
  case "$boundary" in before_publish|after_publish|after_metadata_commit) expect_new=1 ;; esac

  create_path="matrix/create-$suffix"
  crash_during "create.$boundary" tribios fs create interrupted "$create_path"
  if [ "$expect_new" = 1 ]; then
    ws_exists interrupted "$create_path" || fail "create.$boundary did not finish"
  else
    ws_exists interrupted "$create_path" && fail "create.$boundary did not roll back"
  fi

  mkdir_path="matrix/mkdir-$suffix"
  crash_during "mkdir.$boundary" tribios fs mkdir interrupted "$mkdir_path"
  if [ "$expect_new" = 1 ]; then
    assert_eq "dir" "$(ws_stat interrupted "$mkdir_path" | awk '{print $1}')" "mkdir.$boundary"
  else
    ws_exists interrupted "$mkdir_path" && fail "mkdir.$boundary did not roll back"
  fi

  link_path="matrix/link-$suffix"
  crash_during "symlink.$boundary" tribios fs symlink interrupted ./target "$link_path"
  if [ "$expect_new" = 1 ]; then
    assert_eq "./target" "$(ws_readlink interrupted "$link_path")" "symlink.$boundary"
  else
    ws_exists interrupted "$link_path" && fail "symlink.$boundary did not roll back"
  fi

  write_path="matrix/write-$suffix"
  ws_write interrupted "$write_path" old
  crash_during "write.$boundary" tribios fs write interrupted "$write_path" new 0
  assert_eq "$([ "$expect_new" = 1 ] && echo new || echo old)" \
    "$(ws_read interrupted "$write_path")" "write.$boundary"

  truncate_path="matrix/truncate-$suffix"
  ws_write interrupted "$truncate_path" content
  crash_during "truncate.$boundary" tribios fs truncate interrupted "$truncate_path" 0
  assert_eq "$([ "$expect_new" = 1 ] && echo 0 || echo 7)" \
    "$(tribios fs stat interrupted "$truncate_path" | sed -n 2p)" "truncate.$boundary"

  chmod_path="matrix/chmod-$suffix"
  ws_write interrupted "$chmod_path" content
  crash_during "chmod.$boundary" tribios fs chmod interrupted "$chmod_path" 600
  assert_eq "$([ "$expect_new" = 1 ] && echo 600 || echo 644)" \
    "$(ws_stat interrupted "$chmod_path" | awk '{print $2}')" "chmod.$boundary"

  time_path="matrix/time-$suffix"
  ws_write interrupted "$time_path" content
  old_mtime="$(tribios fs stat interrupted "$time_path" | sed -n 4p)"
  new_mtime=$((old_mtime - 1000))
  crash_during "utimens.$boundary" tribios fs utimens interrupted "$time_path" "$new_mtime" "$new_mtime"
  assert_eq "$([ "$expect_new" = 1 ] && echo "$new_mtime" || echo "$old_mtime")" \
    "$(tribios fs stat interrupted "$time_path" | sed -n 4p)" "utimens.$boundary"

  rename_source="matrix/rename-source-$suffix"
  rename_target="matrix/rename-target-$suffix"
  ws_write interrupted "$rename_source" source
  crash_during "rename.$boundary" tribios fs mv interrupted "$rename_source" "$rename_target"
  if [ "$expect_new" = 1 ]; then
    ws_exists interrupted "$rename_source" && fail "rename.$boundary retained its source"
    assert_eq source "$(ws_read interrupted "$rename_target")" "rename.$boundary target"
  else
    assert_eq source "$(ws_read interrupted "$rename_source")" "rename.$boundary source"
    ws_exists interrupted "$rename_target" && fail "rename.$boundary published its target"
  fi
done

ws_write interrupted matrix/rename-after-source-remove old
crash_during rename.after_source_remove tribios fs mv interrupted \
  matrix/rename-after-source-remove matrix/rename-after-source-remove-target
ws_exists interrupted matrix/rename-after-source-remove &&
  fail "rename.after_source_remove retained its source"
assert_eq old "$(ws_read interrupted matrix/rename-after-source-remove-target)" \
  "rename.after_source_remove target"

# Rename recovery covers both replacement and directory publication shapes.
for boundary in after_journal before_publish after_publish after_metadata_commit; do
  suffix="${boundary//_/-}"
  expect_new=0
  case "$boundary" in before_publish|after_publish|after_metadata_commit) expect_new=1 ;; esac

  replace_source="matrix/replace-source-$suffix"
  replace_target="matrix/replace-target-$suffix"
  ws_write interrupted "$replace_source" source
  ws_write interrupted "$replace_target" target
  crash_during "rename.$boundary" tribios fs mv interrupted "$replace_source" "$replace_target"
  if [ "$expect_new" = 1 ]; then
    ws_exists interrupted "$replace_source" && fail "replacement rename retained its source"
    assert_eq source "$(ws_read interrupted "$replace_target")" "replacement rename target"
  else
    assert_eq source "$(ws_read interrupted "$replace_source")" "replacement rename source"
    assert_eq target "$(ws_read interrupted "$replace_target")" "replacement rename rollback"
  fi

  directory_source="matrix/directory-source-$suffix"
  directory_target="matrix/directory-target-$suffix"
  ws_mkdir interrupted "$directory_source"
  ws_write interrupted "$directory_source/child" child
  crash_during "rename.$boundary" tribios fs mv interrupted "$directory_source" "$directory_target"
  if [ "$expect_new" = 1 ]; then
    ws_exists interrupted "$directory_source" && fail "directory rename retained its source"
    assert_eq child "$(ws_read interrupted "$directory_target/child")" "directory rename target"
  else
    assert_eq child "$(ws_read interrupted "$directory_source/child")" "directory rename source"
    ws_exists interrupted "$directory_target" && fail "directory rename published its target"
  fi
done

# POSIX permits a directory to replace an existing empty directory.
ws_mkdir interrupted matrix/empty-replace-source
ws_write interrupted matrix/empty-replace-source/child child
ws_mkdir interrupted matrix/empty-replace-target
tribios fs mv interrupted matrix/empty-replace-source matrix/empty-replace-target >/dev/null
assert_eq child "$(ws_read interrupted matrix/empty-replace-target/child)" \
  "empty-directory replacement"

# Removal operations have no staged object: prepared intent rolls back, while
# every publishing boundary completes the removal.
for operation in unlink rmdir; do
  for boundary in after_journal before_upper_remove after_upper_remove after_metadata_commit; do
    suffix="$operation-${boundary//_/-}"
    path="matrix/$suffix"
    if [ "$operation" = unlink ]; then
      ws_write interrupted "$path" content
      command=(tribios fs rm interrupted "$path")
    else
      ws_mkdir interrupted "$path"
      command=(tribios fs rmdir interrupted "$path")
    fi
    crash_during "$operation.$boundary" "${command[@]}"
    if [ "$boundary" = after_journal ]; then
      ws_exists interrupted "$path" || fail "$operation.$boundary did not roll back"
    else
      ws_exists interrupted "$path" && fail "$operation.$boundary did not finish"
    fi
  done
done

# Rename publication and source hiding settle as one recovered engine state.
crash_during rename.after_publish tribios fs mv interrupted docs/list.txt docs/recovered-list.txt
ws_exists interrupted docs/list.txt && fail "the recovered rename source is still visible"
assert_eq "one" "$(ws_read interrupted docs/recovered-list.txt | head -1)" "recovered rename"

# Active creation is durable even if the daemon dies before replying.
crash_during workspace_create.after_active_commit tribios workspace create created
assert_contains $'created\tactive' "$(tribios workspace list)"
assert_eq "shared base content" "$(ws_read created docs/notes.txt)" "recovered Workspace creation"

# An earlier creation phase rolls back its storage, linked worktree and branch.
crash_during workspace_create.after_git_register tribios workspace create rolled-back
tribios fs stat rolled-back docs/notes.txt >/dev/null 2>&1 &&
  fail "a rolled-back Workspace creation became active"
git -C "$PROJECT" branch --list rolled-back | grep -q . &&
  fail "a rolled-back Workspace branch was not reclaimed"

# Logical removal remains removed when the daemon dies before replying, and
# physical reclamation resumes on the next startup.
crash_during workspace_remove.after_removed_commit tribios workspace remove created
tribios fs stat created docs/notes.txt >/dev/null 2>&1 &&
  fail "a durably removed Workspace reappeared"
tribios workspace wait-reclaim
assert_contains $'created\treclaimed' "$(tribios workspace list)"

# Every lifecycle boundary resolves to its durable old or new state and leaves
# Git's linked-worktree registry usable.
for boundary in after_journal after_state_commit after_storage after_git_register after_git_pointer after_active_commit before_reply; do
  name="create-${boundary//_/-}"
  crash_during "workspace_create.$boundary" tribios workspace create "$name"
  case "$boundary" in
    after_active_commit|before_reply)
      assert_contains "$name"$'\tactive' "$(tribios workspace list)"
      git -C "$PROJECT" worktree list --porcelain >/dev/null
      ;;
    *)
      tribios fs stat "$name" docs/notes.txt >/dev/null 2>&1 &&
        fail "workspace_create.$boundary exposed a rolled-back Workspace"
      git -C "$PROJECT" branch --list "$name" | grep -q . &&
        fail "workspace_create.$boundary leaked its branch"
      ;;
  esac
done

for boundary in after_journal after_removed_commit before_git_cleanup after_git_cleanup before_storage_cleanup after_storage_cleanup before_tombstone_cleanup after_tombstone_cleanup before_reclaimed_commit after_reclaimed_commit before_reply; do
  name="remove-${boundary//_/-}"
  tribios workspace create "$name" >/dev/null
  crash_during "workspace_remove.$boundary" remove_workspace_and_wait "$name"
  if [ "$boundary" = after_journal ]; then
    assert_contains "$name"$'\tactive' "$(tribios workspace list)"
  else
    tribios fs stat "$name" docs/notes.txt >/dev/null 2>&1 &&
      fail "workspace_remove.$boundary resurrected a removed Workspace"
    assert_contains "$name"$'\treclaimed' "$(tribios workspace list)"
  fi
  git -C "$PROJECT" worktree list --porcelain >/dev/null
  git -C "$PROJECT" fsck --connectivity-only >/dev/null
done

inspection="$(tribios recovery inspect)"
assert_contains "metadata format: 1" "$inspection"
assert_contains "pending operations: 0" "$inspection"
assert_contains "recovery diagnostics: 0" "$inspection"
assert_contains "failpoint=rename.after_publish" "$(cat "$PROJECT/.tribios/recovery.trace")"

stop_daemon

# Damaged state fails closed before a mount is exposed, and repeated attempts
# retain one stable diagnostic identifier for read-only inspection.
printf 'invalid Git pointer\n' > "$PROJECT/.tribios/workspaces/interrupted/upper/.git"
if first_failure="$($TRIBIOS_DAEMON --project "$PROJECT" --no-mount 2>&1)"; then
  fail "the daemon exposed a Project with invalid Git metadata"
fi
assert_contains "recovery diagnostic R" "$first_failure"
if second_failure="$($TRIBIOS_DAEMON --project "$PROJECT" --no-mount 2>&1)"; then
  fail "the damaged Project started on a repeated attempt"
fi
first_diagnostic="$(echo "$first_failure" | grep -o 'R[0-9][0-9]*' | head -1)"
second_diagnostic="$(echo "$second_failure" | grep -o 'R[0-9][0-9]*' | head -1)"
assert_eq "$first_diagnostic" "$second_diagnostic" "stable recovery diagnostic identifier"
assert_contains "$first_diagnostic" "$(tribios recovery inspect)"

echo "PASS crash recovery"
