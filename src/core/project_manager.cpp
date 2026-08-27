#include "core/project_manager.hpp"

#include <cerrno>
#include <cctype>
#include <cstdint>
#include <fcntl.h>
#include <sys/stat.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <system_error>
#include <unistd.h>

#include "core/git_worktree.hpp"
#include "core/fault_injection.hpp"
#include "core/paths.hpp"
#include "core/recovery.hpp"

namespace tribios {
namespace {

bool is_valid_workspace_name(const std::string& name) {
  if (name.empty() || name.size() > 64 || name == "." || name == "..") return false;
  for (char c : name) {
    const bool allowed =
        std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_' || c == '.';
    if (!allowed) return false;
  }
  return true;
}

OutcomeVoid write_file_durably(const std::filesystem::path& path, std::string_view contents) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return error("cannot create " + path.string());
  std::size_t offset = 0;
  while (offset < contents.size()) {
    const ssize_t count = ::write(fd, contents.data() + offset, contents.size() - offset);
    if (count <= 0) {
      ::close(fd);
      return error("cannot write " + path.string());
    }
    offset += static_cast<std::size_t>(count);
  }
  ::close(fd);
  if (sync_file_data(path) != 0) return error("cannot flush " + path.string());
  if (sync_parent_directory(path) != 0) {
    return error("cannot flush directory " + path.parent_path().string());
  }
  return {};
}

std::uint64_t stable_project_path_hash(const std::string& path) {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const unsigned char byte : path) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

StorageConfiguration storage_configuration(const ProjectPaths& paths,
                                           std::uint64_t growth_allowance_bytes) {
  return StorageConfiguration{paths.root, paths.tribios_dir, paths.base_dir, paths.mount_point,
                              paths.private_storage_dir, growth_allowance_bytes};
}

}  // namespace

ProjectPaths ProjectPaths::from_project_root(const std::filesystem::path& project_root) {
  std::error_code ec;
  std::filesystem::path root = std::filesystem::weakly_canonical(project_root, ec);
  if (ec) root = project_root;

  ProjectPaths paths;
  paths.root = root;
  paths.tribios_dir = root / kTribiosDirName;
  paths.base_dir = paths.tribios_dir / "base";
  paths.workspaces_dir = paths.tribios_dir / "workspaces";
  paths.staging_dir = paths.tribios_dir / "staging";
  paths.private_storage_dir = paths.tribios_dir / "storage";
  paths.database = paths.tribios_dir / "meta.db";
  paths.mount_point = paths.tribios_dir / "mnt";
  paths.log = paths.tribios_dir / "daemon.log";

  // The socket is ephemeral and must stay below macOS's Unix socket path limit,
  // even when persistent Project storage and TMPDIR use long SSD paths.
  char socket_name[64];
  std::snprintf(socket_name, sizeof(socket_name), "tribios-%016llx.sock",
                static_cast<unsigned long long>(stable_project_path_hash(root.string())));
  paths.socket = std::filesystem::path("/tmp") / socket_name;
  return paths;
}

ProjectManager::~ProjectManager() { wait_for_reclamation(); }

Outcome<BaseStateCapture> ProjectManager::configure(const std::filesystem::path& project_root,
                                                    const std::filesystem::path& mount_point,
                                                    bool force,
                                                    std::uint64_t growth_allowance_bytes) {
  ProjectPaths paths = ProjectPaths::from_project_root(project_root);
  std::error_code ec;
  if (!std::filesystem::is_directory(paths.root, ec))
    return error(paths.root.string() + " is not a directory");
  if (!is_git_project(paths.root)) return error(paths.root.string() + " is not a Git Project");
  if (std::filesystem::exists(paths.database, ec) && !force) {
    return error("project is already configured; the Base state is captured once");
  }
  if (force || (!std::filesystem::exists(paths.database, ec) &&
                std::filesystem::exists(paths.tribios_dir, ec))) {
    std::filesystem::remove_all(paths.tribios_dir, ec);
    if (ec) return error("cannot clear incomplete Project storage: " + ec.message());
  }
  paths.mount_point = mount_point.empty() ? paths.mount_point
                                          : std::filesystem::absolute(mount_point);
  for (const auto& dir : {paths.tribios_dir, paths.staging_dir, paths.private_storage_dir,
                          paths.mount_point}) {
    std::filesystem::create_directories(dir, ec);
    if (ec) return error("cannot create " + dir.string() + ": " + ec.message());
  }

  auto estimated = estimate_workspace_contents(paths.root);
  if (!estimated) return std::unexpected(estimated.error());
  const auto growth_allowance =
      growth_allowance_bytes == 0
          ? default_growth_allowance_bytes(static_cast<std::uint64_t>(estimated->bytes))
          : growth_allowance_bytes;
  const auto configuration = storage_configuration(paths, growth_allowance);
  const auto capabilities = probe_workspace_storage_capabilities(configuration);
  auto selected = choose_supported_backend(capabilities);
  if (!selected) return std::unexpected(selected.error());
  auto storage = open_workspace_storage(*selected, configuration);
  if (!storage) return std::unexpected(storage.error());
  auto captured = (*storage)->capture_base_state(
      paths.root, [](std::int64_t entries, std::int64_t bytes) {
        std::cerr << "capturing Base state: " << entries << " entries, " << bytes << " bytes\r";
      });
  std::cerr << "\n";
  if (!captured) return std::unexpected(captured.error());

  const auto staged_database = paths.tribios_dir / "meta.db.staging";
  auto store = MetadataStore::open_database(staged_database);
  if (!store) return std::unexpected(store.error());

  ProjectRecord record;
  record.root = paths.root.string();
  record.mount_point = paths.mount_point.string();
  record.base_captured_at = current_unix_time_seconds();
  record.base_capture_ms = captured->duration_ms;
  record.base_entry_count = captured->entry_count;
  record.base_bytes = captured->bytes;
  record.storage_backend = *selected;
  record.storage_format_version = kStorageFormatVersion;
  record.growth_allowance_bytes = growth_allowance;
  if (auto stored = (*store)->save_project_record(record); !stored) {
    return std::unexpected(stored.error());
  }
  store->reset();
  std::filesystem::rename(staged_database, paths.database, ec);
  if (ec) return error("cannot publish Project metadata: " + ec.message());
  if (sync_directory(paths.tribios_dir) != 0 || sync_parent_directory(paths.tribios_dir) != 0) {
    return error("cannot flush Project metadata directories");
  }
  return captured;
}

Outcome<std::unique_ptr<ProjectManager>> ProjectManager::open_configured_project(
    const std::filesystem::path& project_root) {
  ProjectPaths paths = ProjectPaths::from_project_root(project_root);
  std::error_code ec;
  if (!std::filesystem::exists(paths.database, ec)) {
    return error("project is not configured: run `tribios configure` first");
  }
  auto store = MetadataStore::open_database(paths.database);
  if (!store) return std::unexpected(store.error());
  auto record = (*store)->load_project_record();
  if (!record) return error("project record is missing from the metadata store");
  if (record->storage_format_version != kStorageFormatVersion) {
    return error("Project storage format " + std::to_string(record->storage_format_version) +
                 " is unsupported by this build");
  }
  paths.mount_point = record->mount_point;

  auto storage = open_workspace_storage(
      record->storage_backend, storage_configuration(paths, record->growth_allowance_bytes));
  if (!storage) return std::unexpected(storage.error());

  if (auto recovered = recover_interrupted_operations(paths.root, paths.tribios_dir,
                                                      **storage, **store);
      !recovered) {
    auto diagnostic = (*store)->record_recovery_diagnostic(paths.root.string(), recovered.error());
    if (diagnostic) {
      return error("recovery diagnostic R" + std::to_string(*diagnostic) + ": " +
                   recovered.error());
    }
    return std::unexpected(recovered.error());
  }
  for (const auto& workspace : (*store)->load_all_workspace_records()) {
    if (workspace.state != WorkspaceState::Active) continue;
    if (auto attached = (*storage)->attach_workspace(workspace.name, workspace.storage_locator);
        !attached) {
      return error("cannot restore Workspace " + workspace.name + ": " + attached.error());
    }
  }
  if (auto valid = validate_project_storage_invariants(paths.root, **storage, **store);
      !valid) {
    auto diagnostic = (*store)->record_recovery_diagnostic(paths.root.string(), valid.error());
    if (diagnostic) {
      return error("recovery diagnostic R" + std::to_string(*diagnostic) + ": " + valid.error());
    }
    return std::unexpected(valid.error());
  }

  return std::unique_ptr<ProjectManager>(
      new ProjectManager(paths, *record, std::move(*store), std::move(*storage)));
}

Outcome<CreateResult> ProjectManager::create_workspace(const std::string& name,
                                                       const std::string& requested_branch) {
  if (!is_valid_workspace_name(name)) return error("invalid Workspace name: " + name);
  std::lock_guard lifecycle_lock(lifecycle_mutex_);
  if (auto existing = store_->load_workspace_record(name);
      existing && existing->state != WorkspaceState::Reclaimed) {
    return error("Workspace name is still in use: " + name);
  }

  const std::string branch = requested_branch.empty() ? name : requested_branch;
  const std::filesystem::path workspace_path = paths_.mount_point / name;
  const std::int64_t started = steady_clock_microseconds();

  RecoveryOperation operation;
  operation.workspace = name;
  operation.kind = "workspace_create";
  operation.path = name;
  operation.target = branch;
  if (const int fault = injected_io_error("lifecycle.journal.write"); fault != 0) {
    return error("Workspace creation journal failed: " +
                 std::generic_category().message(fault));
  }
  auto operation_id = store_->begin_recovery_operation(operation);
  if (!operation_id) return std::unexpected(operation_id.error());
  operation.id = *operation_id;
  const std::string operation_context = "operation=" + std::to_string(operation.id) +
                                        " workspace=" + name + " branch=" + branch;
  trigger_failpoint("workspace_create.after_journal", paths_.tribios_dir, operation_context);

  WorkspaceRecord record;
  record.name = name;
  record.branch = branch;
  record.state = WorkspaceState::Creating;
  record.created_at = current_unix_time_seconds();
  if (const int fault = injected_io_error("lifecycle.state.write"); fault != 0) {
    store_->abandon_recovery_operation(operation.id);
    return error("Workspace creation state failed: " +
                 std::generic_category().message(fault));
  }
  if (auto stored = store_->save_workspace_record(record); !stored) {
    store_->abandon_recovery_operation(operation.id);
    return std::unexpected(stored.error());
  }
  trigger_failpoint("workspace_create.after_state_commit", paths_.tribios_dir,
                    operation_context);

  if (const int fault = injected_io_error("lifecycle.storage.create"); fault != 0) {
    store_->set_workspace_state(name, WorkspaceState::Reclaimed);
    store_->abandon_recovery_operation(operation.id);
    return error("Workspace storage creation failed: " +
                 std::generic_category().message(fault));
  }
  auto locator = storage_->create_workspace(name);
  if (!locator) {
    store_->set_workspace_state(name, WorkspaceState::Reclaimed);
    store_->abandon_recovery_operation(operation.id);
    return std::unexpected(locator.error());
  }
  record.storage_locator = *locator;
  if (auto stored = store_->save_workspace_record(record); !stored) {
    storage_->reclaim_workspace(name, *locator);
    store_->abandon_recovery_operation(operation.id);
    return std::unexpected(stored.error());
  }
  trigger_failpoint("workspace_create.after_storage", paths_.tribios_dir, operation_context);

  auto git_file =
      register_linked_worktree(paths_.root, branch, workspace_path, paths_.staging_dir / name);
  if (!git_file) {
    storage_->reclaim_workspace(name, *locator);
    store_->set_workspace_state(name, WorkspaceState::Reclaimed);
    store_->abandon_recovery_operation(operation.id);
    return std::unexpected(git_file.error());
  }
  trigger_failpoint("workspace_create.after_git_register", paths_.tribios_dir,
                    operation_context);
  if (const int fault = injected_io_error("lifecycle.git_pointer.write"); fault != 0) {
    rollback_linked_worktree_creation(paths_.root, branch, workspace_path,
                                      paths_.staging_dir / name);
    storage_->reclaim_workspace(name, *locator);
    store_->set_workspace_state(name, WorkspaceState::Reclaimed);
    store_->abandon_recovery_operation(operation.id);
    return error("Workspace Git pointer failed: " + std::generic_category().message(fault));
  }
  if (auto written = write_file_durably(workspace_path / kGitDirName, *git_file); !written) {
    rollback_linked_worktree_creation(paths_.root, branch, workspace_path,
                                      paths_.staging_dir / name);
    storage_->reclaim_workspace(name, *locator);
    store_->set_workspace_state(name, WorkspaceState::Reclaimed);
    store_->abandon_recovery_operation(operation.id);
    return std::unexpected(written.error());
  }
  trigger_failpoint("workspace_create.after_git_pointer", paths_.tribios_dir,
                    operation_context);

  record.state = WorkspaceState::Active;
  record.create_us = steady_clock_microseconds() - started;
  if (const int fault = injected_io_error("lifecycle.active.write"); fault != 0) {
    rollback_linked_worktree_creation(paths_.root, branch, workspace_path,
                                      paths_.staging_dir / name);
    storage_->reclaim_workspace(name, *locator);
    store_->set_workspace_state(name, WorkspaceState::Reclaimed);
    store_->abandon_recovery_operation(operation.id);
    return error("Workspace activation failed: " + std::generic_category().message(fault));
  }
  if (auto stored = store_->save_workspace_record(record); !stored) {
    rollback_linked_worktree_creation(paths_.root, branch, workspace_path,
                                      paths_.staging_dir / name);
    storage_->reclaim_workspace(name, *locator);
    store_->set_workspace_state(name, WorkspaceState::Reclaimed);
    store_->abandon_recovery_operation(operation.id);
    return std::unexpected(stored.error());
  }
  trigger_failpoint("workspace_create.after_active_commit", paths_.tribios_dir,
                    operation_context);
  if (auto completed = store_->abandon_recovery_operation(operation.id); !completed) {
    return std::unexpected(completed.error());
  }

  trigger_failpoint("workspace_create.before_reply", paths_.tribios_dir, operation_context);
  return CreateResult{name, record.branch, record.create_us, workspace_path};
}

Outcome<RemoveResult> ProjectManager::remove_workspace(const std::string& name) {
  std::lock_guard lifecycle_lock(lifecycle_mutex_);
  const std::int64_t started = steady_clock_microseconds();
  auto record = store_->load_workspace_record(name);
  if (!record || record->state != WorkspaceState::Active) {
    return error("no such active Workspace: " + name);
  }

  RecoveryOperation operation;
  operation.workspace = name;
  operation.kind = "workspace_remove";
  operation.path = name;
  if (const int fault = injected_io_error("lifecycle.journal.write"); fault != 0) {
    return error("Workspace removal journal failed: " +
                 std::generic_category().message(fault));
  }
  auto operation_id = store_->begin_recovery_operation(operation);
  if (!operation_id) return std::unexpected(operation_id.error());
  operation.id = *operation_id;
  const std::string operation_context = "operation=" + std::to_string(operation.id) +
                                        " workspace=" + name;
  trigger_failpoint("workspace_remove.after_journal", paths_.tribios_dir, operation_context);

  if (auto detached = storage_->detach_workspace(name, record->storage_locator); !detached) {
    store_->abandon_recovery_operation(operation.id);
    return std::unexpected(detached.error());
  }
  trigger_failpoint("workspace_remove.after_detach", paths_.tribios_dir, operation_context);

  record->state = WorkspaceState::Removed;
  record->removed_at = current_unix_time_seconds();
  record->logical_remove_us = steady_clock_microseconds() - started;
  record->reclaim_us = -1;
  if (const int fault = injected_io_error("lifecycle.removed.write"); fault != 0) {
    storage_->attach_workspace(name, record->storage_locator);
    store_->abandon_recovery_operation(operation.id);
    return error("Workspace removal state failed: " +
                 std::generic_category().message(fault));
  }
  if (auto stored = store_->save_workspace_record(*record); !stored) {
    storage_->attach_workspace(name, record->storage_locator);
    store_->abandon_recovery_operation(operation.id);
    return std::unexpected(stored.error());
  }
  trigger_failpoint("workspace_remove.after_removed_commit", paths_.tribios_dir,
                    operation_context);
  start_workspace_reclamation(name, operation.id);
  trigger_failpoint("workspace_remove.before_reply", paths_.tribios_dir, operation_context);
  return RemoveResult{name, record->logical_remove_us};
}

void ProjectManager::start_workspace_reclamation(const std::string& name,
                                                  std::int64_t operation_id) {
  std::lock_guard lock(reclaimers_mutex_);
  reclaimers_.emplace_back([this, name, operation_id] {
    const std::int64_t started = steady_clock_microseconds();
    const std::string context = "operation=" + std::to_string(operation_id) +
                                " workspace=" + name;
    const auto record = store_->load_workspace_record(name);
    if (!record) return;
    trigger_failpoint("workspace_remove.before_git_cleanup", paths_.tribios_dir, context);
    if (!unregister_linked_worktree(paths_.root, paths_.mount_point / name)) return;
    trigger_failpoint("workspace_remove.after_git_cleanup", paths_.tribios_dir, context);
    trigger_failpoint("workspace_remove.before_storage_cleanup", paths_.tribios_dir, context);
    if (!storage_->reclaim_workspace(name, record->storage_locator)) return;
    trigger_failpoint("workspace_remove.after_storage_cleanup", paths_.tribios_dir, context);
    if (!store_->set_workspace_reclamation_duration(name, steady_clock_microseconds() - started)) {
      return;
    }
    trigger_failpoint("workspace_remove.before_reclaimed_commit", paths_.tribios_dir, context);
    if (!store_->set_workspace_state(name, WorkspaceState::Reclaimed)) return;
    trigger_failpoint("workspace_remove.after_reclaimed_commit", paths_.tribios_dir, context);
    store_->abandon_recovery_operation(operation_id);
  });
}

void ProjectManager::wait_for_reclamation() {
  std::vector<std::thread> pending;
  {
    std::lock_guard lock(reclaimers_mutex_);
    pending.swap(reclaimers_);
  }
  for (auto& reclaimer : pending) {
    if (reclaimer.joinable()) reclaimer.join();
  }
}

std::vector<WorkspaceRecord> ProjectManager::workspace_records() {
  return store_->load_all_workspace_records();
}

std::vector<std::string> ProjectManager::active_workspace_names() {
  std::vector<std::string> names;
  for (const auto& workspace : store_->load_all_workspace_records()) {
    if (workspace.state == WorkspaceState::Active) names.push_back(workspace.name);
  }
  return names;
}

Outcome<WorkspaceStorageStatus> ProjectManager::workspace_status(const std::string& name) {
  auto record = store_->load_workspace_record(name);
  if (!record || record->state != WorkspaceState::Active) {
    return error("no such active Workspace: " + name);
  }
  return storage_->inspect_workspace(name, record->storage_locator);
}

}  // namespace tribios
