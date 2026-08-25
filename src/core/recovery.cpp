#include "core/recovery.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "core/git_worktree.hpp"
#include "core/paths.hpp"
#include "core/proc.hpp"

namespace tribios {
namespace {

bool path_exists_without_following_symlinks(const std::filesystem::path& path) {
  struct stat st {};
  return ::lstat(path.c_str(), &st) == 0;
}

std::string read_first_line(const std::filesystem::path& path) {
  std::ifstream file(path);
  std::string line;
  std::getline(file, line);
  return line;
}

OutcomeVoid recovery_error(const std::filesystem::path& project_root,
                           const RecoveryOperation& operation, const std::string& detail) {
  return error("recovery refused Project " + project_root.string() + ", Workspace " +
               operation.workspace + ", operation " + std::to_string(operation.id) + " (" +
               operation.kind + ") at " + operation.path + ": " + detail);
}

bool valid_recovery_relative_path(const std::string& path, bool allow_empty) {
  if (path.empty()) return allow_empty;
  return normalize_relative(path) == path;
}

OutcomeVoid remove_upper_entry(const std::filesystem::path& path, bool directory) {
  std::error_code ec;
  if (!path_exists_without_following_symlinks(path)) return {};
  if (directory) {
    std::filesystem::remove_all(path, ec);
  } else {
    std::filesystem::remove(path, ec);
  }
  if (ec) return error("cannot remove " + path.string() + ": " + ec.message());
  if (const int synced = sync_parent_directory(path); synced != 0) {
    return error("cannot sync " + path.parent_path().string() + ": " +
                 std::generic_category().message(synced));
  }
  return {};
}

OutcomeVoid publish_staged_entry(const std::filesystem::path& stage,
                                 const std::filesystem::path& destination) {
  std::error_code ec;
  if (path_exists_without_following_symlinks(stage)) {
    std::filesystem::create_directories(destination.parent_path(), ec);
    if (ec) return error("cannot create recovery destination parent: " + ec.message());
    std::filesystem::rename(stage, destination, ec);
    if (ec) return error("cannot publish recovered entry: " + ec.message());
  } else if (!path_exists_without_following_symlinks(destination)) {
    return error("both the staged entry and published entry are missing");
  }
  if (const int synced = sync_parent_directory(destination); synced != 0) {
    return error("cannot sync recovered destination: " +
                 std::generic_category().message(synced));
  }
  return {};
}

bool staged_filesystem_operation(std::string_view kind) {
  return kind == "create" || kind == "mkdir" || kind == "symlink" || kind == "write" ||
         kind == "truncate" || kind == "chmod" || kind == "utimens" || kind == "rename";
}

OutcomeVoid validate_completed_workspace_creation(const std::filesystem::path& project_root,
                                                  const std::filesystem::path& upper_dir,
                                                  const RecoveryOperation& operation) {
  std::ifstream git_file(upper_dir / kGitDirName);
  std::string pointer;
  std::getline(git_file, pointer);
  constexpr std::string_view marker = "gitdir: ";
  if (!pointer.starts_with(marker)) {
    return recovery_error(project_root, operation, "active Workspace has no valid .git pointer");
  }
  const std::filesystem::path admin_dir = pointer.substr(marker.size());
  std::error_code ec;
  if (!std::filesystem::is_directory(admin_dir, ec)) {
    return recovery_error(project_root, operation,
                          "active Workspace Git administrative directory is missing");
  }
  const std::filesystem::path canonical_admin = std::filesystem::weakly_canonical(admin_dir, ec);
  const std::filesystem::path canonical_registry =
      std::filesystem::weakly_canonical(project_root / kGitDirName / "worktrees", ec);
  if (ec || canonical_admin.parent_path() != canonical_registry) {
    return recovery_error(project_root, operation,
                          "Git administrative directory is outside the Project registry");
  }
  const std::string registered_git_file = read_first_line(admin_dir / "gitdir");
  if (std::filesystem::path(registered_git_file).filename() != kGitDirName ||
      std::filesystem::path(registered_git_file).parent_path().filename() != operation.workspace) {
    return recovery_error(project_root, operation,
                          "Git linked-worktree record points at the wrong Workspace");
  }
  auto checked = run_process_and_capture_output(
      {"git", "--git-dir", admin_dir.string(), "fsck", "--connectivity-only"});
  if (!checked.ok()) {
    return recovery_error(project_root, operation, "Git integrity check failed: " + checked.output);
  }
  auto listed = run_process_and_capture_output(
      {"git", "-C", project_root.string(), "worktree", "list", "--porcelain"});
  if (!listed.ok() || listed.output.find("worktree " +
                                         std::filesystem::path(registered_git_file)
                                             .parent_path().string()) == std::string::npos ||
      (!operation.target.empty() &&
       listed.output.find("branch refs/heads/" + operation.target) == std::string::npos)) {
    return recovery_error(project_root, operation, "Git linked-worktree registry is inconsistent");
  }
  return {};
}

}  // namespace

void trigger_failpoint(std::string_view name, const std::filesystem::path& tribios_dir,
                       std::string_view context) {
  const char* selected = std::getenv("TRIBIOS_FAILPOINT");
  if (selected == nullptr || name != selected) return;

  const std::filesystem::path trace_path = tribios_dir / "recovery.trace";
  const int fd = ::open(trace_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd >= 0) {
    std::string trace = "failpoint=" + std::string(name) + " pid=" +
                        std::to_string(static_cast<long long>(::getpid()));
    if (const char* seed = std::getenv("TRIBIOS_FAULT_SEED"); seed != nullptr) {
      trace += " seed=" + std::string(seed);
    }
    if (!context.empty()) trace += " " + std::string(context);
    trace += "\n";
    const ssize_t ignored = ::write(fd, trace.data(), trace.size());
    (void)ignored;
    ::fsync(fd);
    ::close(fd);
    sync_parent_directory(trace_path);
  }
  ::_exit(137);
}

int sync_file_data(const std::filesystem::path& path) {
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return errno != 0 ? errno : EIO;
#ifdef __APPLE__
  const int result = ::fcntl(fd, F_FULLFSYNC);
#else
  const int result = ::fsync(fd);
#endif
  const int code = result == 0 ? 0 : (errno != 0 ? errno : EIO);
  ::close(fd);
  return code;
}

int sync_directory(const std::filesystem::path& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0) return errno != 0 ? errno : EIO;
  const int result = ::fsync(fd);
  const int code = result == 0 ? 0 : (errno != 0 ? errno : EIO);
  ::close(fd);
  return code;
}

int sync_parent_directory(const std::filesystem::path& path) {
  return sync_directory(path.parent_path());
}

OutcomeVoid recover_interrupted_operations(const std::filesystem::path& project_root,
                                           const std::filesystem::path& tribios_dir,
                                           const std::filesystem::path& workspaces_dir,
                                           MetadataStore& store) {
  auto loaded = store.load_recovery_operations();
  if (!loaded) return std::unexpected(loaded.error());

  for (auto operation : *loaded) {
    if (operation.kind == "workspace_create") {
      const auto record = store.load_workspace_record(operation.workspace);
      const std::filesystem::path workspace_dir = workspaces_dir / operation.workspace;
      if (record && record->state == WorkspaceState::Active) {
        if (auto valid = validate_completed_workspace_creation(
                project_root, workspace_dir / "upper", operation);
            !valid) {
          return valid;
        }
      } else {
        const auto project = store.load_project_record();
        if (!project) {
          return recovery_error(project_root, operation,
                                "Project record is missing during Workspace creation rollback");
        }
        std::error_code ec;
        std::filesystem::remove_all(workspace_dir, ec);
        if (ec) return recovery_error(project_root, operation, ec.message());
        if (auto rolled_back = rollback_linked_worktree_creation(
                project_root, operation.target,
                std::filesystem::path(project->mount_point) / operation.workspace,
                tribios_dir / "staging" / operation.workspace);
            !rolled_back) {
          return recovery_error(project_root, operation, rolled_back.error());
        }
        if (record) {
          if (auto reclaimed =
                  store.set_workspace_state(operation.workspace, WorkspaceState::Reclaimed);
              !reclaimed) {
            return recovery_error(project_root, operation, reclaimed.error());
          }
        }
      }
      if (auto abandoned = store.abandon_recovery_operation(operation.id); !abandoned) {
        return recovery_error(project_root, operation, abandoned.error());
      }
      continue;
    }
    if (operation.kind == "workspace_remove") {
      const auto record = store.load_workspace_record(operation.workspace);
      if (!record || record->state == WorkspaceState::Active) {
        if (auto abandoned = store.abandon_recovery_operation(operation.id); !abandoned) {
          return recovery_error(project_root, operation, abandoned.error());
        }
        continue;
      }
      const std::string context = "operation=" + std::to_string(operation.id) +
                                  " workspace=" + operation.workspace;
      const auto project = store.load_project_record();
      if (!project) return recovery_error(project_root, operation, "Project record is missing");
      trigger_failpoint("recovery.workspace_remove.before_git_cleanup", tribios_dir, context);
      if (auto unregistered = unregister_linked_worktree(
              project_root, std::filesystem::path(project->mount_point) / operation.workspace);
          !unregistered) {
        return recovery_error(project_root, operation, unregistered.error());
      }
      trigger_failpoint("recovery.workspace_remove.after_git_cleanup", tribios_dir, context);
      std::error_code ec;
      std::filesystem::remove_all(workspaces_dir / operation.workspace, ec);
      if (ec || sync_directory(workspaces_dir) != 0) {
        return recovery_error(project_root, operation,
                              ec ? ec.message() : "cannot flush Workspace cleanup");
      }
      trigger_failpoint("recovery.workspace_remove.after_storage_cleanup", tribios_dir, context);
      if (auto cleared = store.clear_tombstones(operation.workspace); !cleared) {
        return recovery_error(project_root, operation, cleared.error());
      }
      if (auto duration = store.set_workspace_reclamation_duration(operation.workspace, 0);
          !duration) {
        return recovery_error(project_root, operation, duration.error());
      }
      if (auto reclaimed = store.set_workspace_state(operation.workspace,
                                                     WorkspaceState::Reclaimed);
          !reclaimed) {
        return recovery_error(project_root, operation, reclaimed.error());
      }
      trigger_failpoint("recovery.workspace_remove.after_reclaimed_commit", tribios_dir, context);
      if (auto abandoned = store.abandon_recovery_operation(operation.id); !abandoned) {
        return recovery_error(project_root, operation, abandoned.error());
      }
      continue;
    }

    if (operation.workspace.empty() || operation.workspace.find('/') != std::string::npos ||
        !valid_recovery_relative_path(operation.path, false) ||
        !valid_recovery_relative_path(operation.target, true)) {
      return recovery_error(project_root, operation, "journal record contains an unsafe path");
    }
    if (operation.kind != "unlink" && operation.kind != "rmdir" &&
        !staged_filesystem_operation(operation.kind)) {
      return recovery_error(project_root, operation, "journal record has an unknown operation kind");
    }

    const std::filesystem::path workspace_dir = workspaces_dir / operation.workspace;
    const std::filesystem::path upper_dir = workspace_dir / "upper";
    const std::filesystem::path stage = workspace_dir / "recovery" /
                                        std::to_string(operation.id);
    const std::string recovery_context = "operation=" + std::to_string(operation.id) +
                                         " workspace=" + operation.workspace +
                                         " path=" + operation.path;

    if (operation.phase == RecoveryPhase::Prepared) {
      trigger_failpoint("recovery.before_prepared_rollback", tribios_dir, recovery_context);
      std::error_code ec;
      std::filesystem::remove_all(stage, ec);
      if (ec) return recovery_error(project_root, operation, ec.message());
      if (auto abandoned = store.abandon_recovery_operation(operation.id); !abandoned) {
        return recovery_error(project_root, operation, abandoned.error());
      }
      trigger_failpoint("recovery.after_prepared_rollback", tribios_dir, recovery_context);
      continue;
    }

    trigger_failpoint("recovery.before_apply", tribios_dir, recovery_context);
    OutcomeVoid recovered;
    if (operation.kind == "unlink") {
      recovered = remove_upper_entry(upper_dir / operation.path, false);
    } else if (operation.kind == "rmdir") {
      recovered = remove_upper_entry(upper_dir / operation.path, true);
    } else {
      const std::string destination_relative =
          operation.kind == "rename" ? operation.target : operation.path;
      recovered = publish_staged_entry(stage, upper_dir / destination_relative);
      if (recovered && operation.kind == "rename") {
        recovered = remove_upper_entry(upper_dir / operation.path, true);
      }
    }
    if (!recovered) return recovery_error(project_root, operation, recovered.error());
    trigger_failpoint("recovery.after_apply", tribios_dir, recovery_context);

    trigger_failpoint("recovery.before_metadata_commit", tribios_dir, recovery_context);
    if (auto finished = store.finish_recovery_operation(operation); !finished) {
      return recovery_error(project_root, operation, finished.error());
    }
    trigger_failpoint("recovery.after_metadata_commit", tribios_dir, recovery_context);
  }

  // Once every journal record is settled, recovery directories contain no
  // live state. Reclaim leftovers from crashes during rollback or cleanup.
  std::error_code cleanup_error;
  for (const auto& workspace : std::filesystem::directory_iterator(workspaces_dir, cleanup_error)) {
    if (cleanup_error) return error("cannot inspect recovery artifacts: " + cleanup_error.message());
    if (!workspace.is_directory(cleanup_error)) continue;
    const std::filesystem::path recovery_dir = workspace.path() / "recovery";
    std::filesystem::remove_all(recovery_dir, cleanup_error);
    if (cleanup_error) return error("cannot reclaim " + recovery_dir.string() + ": " +
                                    cleanup_error.message());
  }
  const std::filesystem::path staging_dir = tribios_dir / "staging";
  std::filesystem::remove_all(staging_dir, cleanup_error);
  if (cleanup_error) return error("cannot reclaim staging artifacts: " + cleanup_error.message());
  std::filesystem::create_directories(staging_dir, cleanup_error);
  if (cleanup_error || sync_parent_directory(staging_dir) != 0) {
    return error("cannot recreate durable staging directory");
  }
  return {};
}

OutcomeVoid validate_project_storage_invariants(const std::filesystem::path& project_root,
                                                const std::filesystem::path& base_dir,
                                                const std::filesystem::path& workspaces_dir,
                                                MetadataStore& store) {
  if (auto database = store.validate_database_integrity(); !database) return database;

  std::error_code ec;
  if (!std::filesystem::is_directory(base_dir, ec)) {
    return error("storage invariant failed for Project " + project_root.string() +
                 ": Base state directory is missing");
  }
  auto project = store.load_project_record();
  if (!project) {
    return error("storage invariant failed for Project " + project_root.string() +
                 ": Project record is missing");
  }
  if (std::filesystem::weakly_canonical(project->root, ec) !=
      std::filesystem::weakly_canonical(project_root, ec)) {
    return error("storage invariant failed for Project " + project_root.string() +
                 ": Project record points at " + project->root);
  }

  auto pending = store.load_recovery_operations();
  if (!pending) return std::unexpected(pending.error());
  if (!pending->empty()) {
    return error("storage invariant failed for Project " + project_root.string() +
                 ": recovery left operation " + std::to_string(pending->front().id) +
                 " unsettled");
  }

  for (const auto& workspace : store.load_all_workspace_records()) {
    if (workspace.name.empty() || workspace.name.find('/') != std::string::npos ||
        workspace.name == "." || workspace.name == "..") {
      return error("storage invariant failed for Project " + project_root.string() +
                   ": Workspace record has an unsafe name " + workspace.name);
    }
    const std::filesystem::path workspace_dir = workspaces_dir / workspace.name;
    if (workspace.state == WorkspaceState::Creating) {
      return error("storage invariant failed for Project " + project_root.string() +
                   ", Workspace " + workspace.name +
                   ": creation has no recoverable operation record");
    }
    if (workspace.state == WorkspaceState::Active) {
      RecoveryOperation operation;
      operation.workspace = workspace.name;
      operation.kind = "validate";
      operation.path = kGitDirName;
      operation.target = workspace.branch;
      if (!std::filesystem::is_directory(workspace_dir / "upper", ec)) {
        return recovery_error(project_root, operation, "upper tree is missing");
      }
      if (auto valid = validate_completed_workspace_creation(
              project_root, workspace_dir / "upper", operation);
          !valid) {
        return valid;
      }
    }
    for (const auto& tombstone : store.load_workspace_tombstones(workspace.name)) {
      if (!valid_recovery_relative_path(tombstone, false)) {
        return error("storage invariant failed for Project " + project_root.string() +
                     ", Workspace " + workspace.name + ", path " + tombstone +
                     ": tombstone path is unsafe");
      }
    }
  }
  return {};
}

}  // namespace tribios
