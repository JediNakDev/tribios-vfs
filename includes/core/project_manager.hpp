#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/base_capture.hpp"
#include "core/error.hpp"
#include "core/metadata_store.hpp"
#include "core/workspace_engine.hpp"

namespace tribios {

struct ProjectPaths {
  std::filesystem::path root;
  std::filesystem::path tribios_dir;
  std::filesystem::path base_dir;
  std::filesystem::path workspaces_dir;
  std::filesystem::path staging_dir;
  std::filesystem::path database;
  std::filesystem::path socket;
  std::filesystem::path mount_point;
  std::filesystem::path log;

  static ProjectPaths from_project_root(const std::filesystem::path& project_root);
};

struct CreateResult {
  std::string name;
  std::string branch;
  std::int64_t create_us = 0;
  std::filesystem::path path;
};

struct RemoveResult {
  std::string name;
  std::int64_t logical_remove_us = 0;
};

// One configured Project: its immutable Base state, its Workspace records and
// one engine per active Workspace.
class ProjectManager {
 public:
  ~ProjectManager();

  // Configures a Project and captures its single Base state.
  static Outcome<CaptureStats> configure(const std::filesystem::path& project_root,
                                         const std::filesystem::path& mount_point, bool force);
  // Opens a configured Project, bringing back every persisted Workspace.
  static Outcome<std::unique_ptr<ProjectManager>> open_configured_project(
      const std::filesystem::path& project_root);

  const ProjectPaths& project_paths() const { return paths_; }
  const ProjectRecord& project_record() const { return record_; }

  Outcome<CreateResult> create_workspace(const std::string& name, const std::string& branch);
  Outcome<RemoveResult> remove_workspace(const std::string& name);
  std::vector<WorkspaceRecord> workspace_records();
  std::vector<std::string> active_workspace_names();
  std::shared_ptr<WorkspaceEngine> find_active_workspace_engine(const std::string& name);

  void wait_for_reclamation();

 private:
  ProjectManager(ProjectPaths paths, ProjectRecord record, std::unique_ptr<MetadataStore> store)
      : paths_(std::move(paths)), record_(std::move(record)), store_(std::move(store)) {}

  std::filesystem::path workspace_upper_directory(const std::string& name) const;
  void start_workspace_reclamation(const std::string& name);

  ProjectPaths paths_;
  ProjectRecord record_;
  std::unique_ptr<MetadataStore> store_;
  std::shared_mutex engines_mutex_;
  std::map<std::string, std::shared_ptr<WorkspaceEngine>> engines_;
  std::mutex reclaimers_mutex_;
  std::vector<std::thread> reclaimers_;
};

}  // namespace tribios
