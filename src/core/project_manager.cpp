#include "core/project_manager.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <system_error>

#include "core/git_worktree.hpp"
#include "core/paths.hpp"

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
  paths.database = paths.tribios_dir / "meta.db";
  paths.socket = paths.tribios_dir / "control.sock";
  paths.mount_point = paths.tribios_dir / "mnt";
  paths.log = paths.tribios_dir / "daemon.log";

  // Unix sockets have a short path limit, so a deep Project falls back to a
  // short name in the temporary directory that the CLI derives the same way.
  if (paths.socket.string().size() >= 100) {
    const char* tmp = std::getenv("TMPDIR");
    char name[64];
    std::snprintf(name, sizeof(name), "tribios-%016llx.sock",
                  static_cast<unsigned long long>(std::hash<std::string>{}(root.string())));
    paths.socket = std::filesystem::path(tmp != nullptr ? tmp : "/tmp") / name;
  }
  return paths;
}

ProjectManager::~ProjectManager() { wait_for_reclamation(); }

std::filesystem::path ProjectManager::workspace_upper_directory(const std::string& name) const {
  return paths_.workspaces_dir / name / "upper";
}

Outcome<CaptureStats> ProjectManager::configure(const std::filesystem::path& project_root,
                                                const std::filesystem::path& mount_point,
                                                bool force) {
  const ProjectPaths paths = ProjectPaths::from_project_root(project_root);
  std::error_code ec;
  if (!std::filesystem::is_directory(paths.root, ec))
    return error(paths.root.string() + " is not a directory");
  if (!is_git_project(paths.root)) return error(paths.root.string() + " is not a Git Project");
  if (std::filesystem::exists(paths.database, ec) && !force) {
    return error("project is already configured; the Base state is captured once");
  }
  if (force) std::filesystem::remove_all(paths.tribios_dir, ec);
  for (const auto& dir : {paths.tribios_dir, paths.workspaces_dir, paths.mount_point}) {
    std::filesystem::create_directories(dir, ec);
    if (ec) return error("cannot create " + dir.string() + ": " + ec.message());
  }

  auto captured = capture_base_state(paths.root, paths.base_dir);
  if (!captured) return std::unexpected(captured.error());

  auto store = MetadataStore::open_database(paths.database);
  if (!store) return std::unexpected(store.error());

  ProjectRecord record;
  record.root = paths.root.string();
  record.mount_point = mount_point.empty() ? paths.mount_point.string()
                                           : std::filesystem::absolute(mount_point).string();
  record.base_captured_at = current_unix_time_seconds();
  record.base_capture_ms = captured->duration_ms;
  record.base_entry_count = captured->entry_count;
  record.base_bytes = captured->bytes;
  if (auto stored = (*store)->save_project_record(record); !stored) {
    return std::unexpected(stored.error());
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
  paths.mount_point = record->mount_point;

  std::unique_ptr<ProjectManager> manager(new ProjectManager(paths, *record, std::move(*store)));
  for (const auto& workspace : manager->store_->load_all_workspace_records()) {
    if (workspace.state == WorkspaceState::Active) {
      manager->engines_.emplace(
          workspace.name,
          std::make_shared<WorkspaceEngine>(workspace.name, paths.base_dir,
                                            manager->workspace_upper_directory(workspace.name),
                                            *manager->store_));
    } else if (workspace.state == WorkspaceState::Removed) {
      manager->start_workspace_reclamation(workspace.name);
    }
  }
  return manager;
}

Outcome<CreateResult> ProjectManager::create_workspace(const std::string& name,
                                                       const std::string& requested_branch) {
  if (!is_valid_workspace_name(name)) return error("invalid Workspace name: " + name);
  {
    std::shared_lock lock(engines_mutex_);
    if (engines_.contains(name)) return error("Workspace already exists: " + name);
  }
  if (auto existing = store_->load_workspace_record(name);
      existing && existing->state != WorkspaceState::Reclaimed) {
    return error("Workspace name is still in use: " + name);
  }

  const std::string branch = requested_branch.empty() ? name : requested_branch;
  const std::filesystem::path upper = workspace_upper_directory(name);
  const std::filesystem::path workspace_path = paths_.mount_point / name;
  const std::int64_t started = steady_clock_microseconds();

  // Creation never traverses the Base state: it makes one empty upper tree and
  // some metadata, so its cost does not grow with the Project.
  std::error_code ec;
  std::filesystem::create_directories(upper, ec);
  if (ec) return error("cannot create Workspace storage: " + ec.message());

  auto git_file =
      register_linked_worktree(paths_.root, branch, workspace_path, paths_.staging_dir / name);
  if (!git_file) {
    std::filesystem::remove_all(upper, ec);
    return std::unexpected(git_file.error());
  }
  std::ofstream(upper / kGitDirName) << *git_file;

  WorkspaceRecord record;
  record.name = name;
  record.branch = branch;
  record.state = WorkspaceState::Active;
  record.created_at = current_unix_time_seconds();
  record.create_us = steady_clock_microseconds() - started;
  if (auto stored = store_->save_workspace_record(record); !stored) {
    std::filesystem::remove_all(upper, ec);
    return std::unexpected(stored.error());
  }

  {
    std::unique_lock lock(engines_mutex_);
    engines_.emplace(name,
                     std::make_shared<WorkspaceEngine>(name, paths_.base_dir, upper, *store_));
  }
  return CreateResult{name, record.branch, record.create_us, workspace_path};
}

Outcome<RemoveResult> ProjectManager::remove_workspace(const std::string& name) {
  const std::int64_t started = steady_clock_microseconds();
  {
    std::unique_lock lock(engines_mutex_);
    if (!engines_.contains(name)) return error("no such active Workspace: " + name);
    engines_.erase(name);
  }

  // The Workspace is inaccessible and its removed state is committed before
  // this returns. Freeing its upper tree happens afterwards.
  auto record = store_->load_workspace_record(name);
  if (!record) return error("no Workspace record for " + name);
  record->state = WorkspaceState::Removed;
  record->removed_at = current_unix_time_seconds();
  record->logical_remove_us = steady_clock_microseconds() - started;
  record->reclaim_us = -1;
  if (auto stored = store_->save_workspace_record(*record); !stored) {
    return std::unexpected(stored.error());
  }

  start_workspace_reclamation(name);
  return RemoveResult{name, record->logical_remove_us};
}

void ProjectManager::start_workspace_reclamation(const std::string& name) {
  std::lock_guard lock(reclaimers_mutex_);
  reclaimers_.emplace_back([this, name] {
    const std::int64_t started = steady_clock_microseconds();
    std::error_code ec;
    std::filesystem::remove_all(paths_.workspaces_dir / name, ec);
    unregister_linked_worktree(paths_.root, name);
    if (!store_->clear_tombstones(name)) return;
    if (!store_->set_workspace_reclamation_duration(name, steady_clock_microseconds() - started)) {
      return;
    }
    if (!store_->set_workspace_state(name, WorkspaceState::Reclaimed)) return;
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
  std::shared_lock lock(engines_mutex_);
  std::vector<std::string> names;
  for (const auto& [name, engine] : engines_) names.push_back(name);
  return names;
}

std::shared_ptr<WorkspaceEngine> ProjectManager::find_active_workspace_engine(
    const std::string& name) {
  std::shared_lock lock(engines_mutex_);
  auto found = engines_.find(name);
  return found == engines_.end() ? nullptr : found->second;
}

}  // namespace tribios
