#include "core/recovery.hpp"

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

#include "core/git_worktree.hpp"
#include "core/paths.hpp"
#include "core/proc.hpp"

namespace tribios {
namespace {

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
               operation.kind + "): " + detail);
}

OutcomeVoid validate_completed_workspace_creation(const std::filesystem::path& project_root,
                                                  const std::filesystem::path& workspace_path,
                                                  const RecoveryOperation& operation) {
  std::ifstream git_file(workspace_path / kGitDirName);
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
  const auto canonical_admin = std::filesystem::weakly_canonical(admin_dir, ec);
  const auto canonical_registry =
      std::filesystem::weakly_canonical(project_root / kGitDirName / "worktrees", ec);
  if (ec || canonical_admin.parent_path() != canonical_registry) {
    return recovery_error(project_root, operation,
                          "Git administrative directory is outside the Project registry");
  }
  const std::filesystem::path registered_git_file = read_first_line(admin_dir / "gitdir");
  if (registered_git_file != workspace_path / kGitDirName) {
    return recovery_error(project_root, operation,
                          "Git linked-worktree record points at the wrong Workspace");
  }
  auto checked = run_process_and_capture_output(
      {"git", "--git-dir", admin_dir.string(), "fsck", "--connectivity-only"});
  if (!checked.ok()) {
    return recovery_error(project_root, operation, "Git integrity check failed: " + checked.output);
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
                                           WorkspaceStorage& storage, MetadataStore& store) {
  auto loaded = store.load_recovery_operations();
  if (!loaded) return std::unexpected(loaded.error());
  const auto project = store.load_project_record();
  if (!project) return error("Project record is missing during recovery");

  for (const auto& operation : *loaded) {
    const auto record = store.load_workspace_record(operation.workspace);
    const auto workspace_path = storage.workspace_path(operation.workspace);
    if (operation.kind == "workspace_create") {
      if (record && record->state == WorkspaceState::Active) {
        if (auto attached = storage.attach_workspace(operation.workspace,
                                                     record->storage_locator);
            !attached) {
          return recovery_error(project_root, operation, attached.error());
        }
        if (auto valid = validate_completed_workspace_creation(project_root, workspace_path,
                                                               operation);
            !valid) {
          return valid;
        }
      } else {
        if (auto rolled_back = rollback_linked_worktree_creation(
                project_root, operation.target, workspace_path,
                tribios_dir / "staging" / operation.workspace);
            !rolled_back) {
          return recovery_error(project_root, operation, rolled_back.error());
        }
        const std::string locator = record ? record->storage_locator : std::string{};
        if (auto detached = storage.detach_workspace(operation.workspace, locator); !detached) {
          return recovery_error(project_root, operation, detached.error());
        }
        if (auto reclaimed = storage.reclaim_workspace(operation.workspace, locator); !reclaimed) {
          return recovery_error(project_root, operation, reclaimed.error());
        }
        if (record) {
          if (auto updated = store.set_workspace_state(operation.workspace,
                                                       WorkspaceState::Reclaimed);
              !updated) {
            return recovery_error(project_root, operation, updated.error());
          }
        }
      }
    } else if (operation.kind == "workspace_remove") {
      if (!record || record->state == WorkspaceState::Active) {
        if (record) {
          if (auto attached = storage.attach_workspace(operation.workspace,
                                                       record->storage_locator);
              !attached) {
            return recovery_error(project_root, operation, attached.error());
          }
        }
      } else {
        if (auto detached = storage.detach_workspace(operation.workspace,
                                                     record->storage_locator);
            !detached) {
          return recovery_error(project_root, operation, detached.error());
        }
        if (auto unregistered = unregister_linked_worktree(project_root, workspace_path);
            !unregistered) {
          return recovery_error(project_root, operation, unregistered.error());
        }
        if (auto reclaimed = storage.reclaim_workspace(operation.workspace,
                                                       record->storage_locator);
            !reclaimed) {
          return recovery_error(project_root, operation, reclaimed.error());
        }
        if (auto duration = store.set_workspace_reclamation_duration(operation.workspace, 0);
            !duration) {
          return recovery_error(project_root, operation, duration.error());
        }
        if (auto updated = store.set_workspace_state(operation.workspace,
                                                     WorkspaceState::Reclaimed);
            !updated) {
          return recovery_error(project_root, operation, updated.error());
        }
      }
    } else {
      return recovery_error(project_root, operation,
                            "metadata format 2 contains an unknown lifecycle operation");
    }
    if (auto abandoned = store.abandon_recovery_operation(operation.id); !abandoned) {
      return recovery_error(project_root, operation, abandoned.error());
    }
  }

  std::error_code ec;
  const auto staging_dir = tribios_dir / "staging";
  std::filesystem::remove_all(staging_dir, ec);
  std::filesystem::create_directories(staging_dir, ec);
  if (ec) return error("cannot recreate lifecycle staging storage: " + ec.message());
  return {};
}

OutcomeVoid validate_project_storage_invariants(const std::filesystem::path& project_root,
                                                WorkspaceStorage& storage,
                                                MetadataStore& store) {
  if (auto database = store.validate_database_integrity(); !database) return database;
  const auto project = store.load_project_record();
  if (!project) return error("storage invariant failed: Project record is missing");
  std::error_code ec;
  if (std::filesystem::weakly_canonical(project->root, ec) !=
      std::filesystem::weakly_canonical(project_root, ec)) {
    return error("storage invariant failed: Project record points at " + project->root);
  }
  auto pending = store.load_recovery_operations();
  if (!pending) return std::unexpected(pending.error());
  if (!pending->empty()) return error("storage invariant failed: recovery left an operation open");

  for (const auto& workspace : store.load_all_workspace_records()) {
    if (workspace.name.empty() || workspace.name == "." || workspace.name == ".." ||
        workspace.name.find('/') != std::string::npos) {
      return error("storage invariant failed: Workspace record has an unsafe name");
    }
    if (workspace.state == WorkspaceState::Creating) {
      return error("storage invariant failed: Workspace creation has no recovery operation");
    }
    if (workspace.state != WorkspaceState::Active) continue;
    auto status = storage.inspect_workspace(workspace.name, workspace.storage_locator);
    if (!status || !status->attached) {
      return error("storage invariant failed: active Workspace " + workspace.name +
                   " is not attached");
    }
    RecoveryOperation operation;
    operation.workspace = workspace.name;
    operation.kind = "validate";
    operation.target = workspace.branch;
    if (auto valid = validate_completed_workspace_creation(
            project_root, storage.workspace_path(workspace.name), operation);
        !valid) {
      return valid;
    }
  }
  return {};
}

}  // namespace tribios
