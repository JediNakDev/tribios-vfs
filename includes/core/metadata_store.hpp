#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/error.hpp"

struct sqlite3;

namespace tribios {

struct ProjectRecord {
  std::string root;
  std::string mount_point;
  std::int64_t base_captured_at = 0;
  std::int64_t base_capture_ms = 0;
  std::int64_t base_entry_count = 0;
  std::int64_t base_bytes = 0;
};

enum class WorkspaceState { Creating, Active, Removed, Reclaimed };

std::string_view workspace_state_name(WorkspaceState state);

struct WorkspaceRecord {
  std::string name;
  std::string branch;
  WorkspaceState state = WorkspaceState::Active;
  std::int64_t created_at = 0;
  std::int64_t create_us = 0;
  std::int64_t removed_at = 0;
  std::int64_t logical_remove_us = 0;
  std::int64_t reclaim_us = -1;
};

enum class RecoveryPhase { Prepared, Publishing };

struct RecoveryOperation {
  std::int64_t id = 0;
  std::string workspace;
  std::string kind;
  std::string path;
  std::string target;
  RecoveryPhase phase = RecoveryPhase::Prepared;
  bool add_path_tombstone = false;
  bool drop_target_tombstones = false;
};

struct RecoveryDiagnostic {
  std::int64_t id = 0;
  std::int64_t created_at = 0;
  std::string project;
  std::string message;
};

// Project records, Workspace records, lifecycle state and tombstones. Private
// file data lives in ordinary directories, never here.
class MetadataStore {
 public:
  ~MetadataStore();
  static Outcome<std::unique_ptr<MetadataStore>> open_database(
      const std::filesystem::path& database_path);
  static Outcome<std::unique_ptr<MetadataStore>> open_database_read_only(
      const std::filesystem::path& database_path);

  OutcomeVoid save_project_record(const ProjectRecord& record);
  std::optional<ProjectRecord> load_project_record();

  OutcomeVoid save_workspace_record(const WorkspaceRecord& record);
  std::optional<WorkspaceRecord> load_workspace_record(const std::string& name);
  std::vector<WorkspaceRecord> load_all_workspace_records();
  OutcomeVoid set_workspace_state(const std::string& name, WorkspaceState state);
  OutcomeVoid set_workspace_reclamation_duration(const std::string& name, std::int64_t reclaim_us);

  OutcomeVoid add_tombstone(const std::string& workspace, const std::string& path);
  OutcomeVoid remove_tombstones_under(const std::string& workspace, const std::string& path);
  OutcomeVoid clear_tombstones(const std::string& workspace);
  std::vector<std::string> load_workspace_tombstones(const std::string& workspace);

  Outcome<std::int64_t> begin_recovery_operation(const RecoveryOperation& operation);
  OutcomeVoid set_recovery_operation_phase(std::int64_t id, RecoveryPhase phase);
  OutcomeVoid finish_recovery_operation(const RecoveryOperation& operation);
  OutcomeVoid abandon_recovery_operation(std::int64_t id);
  Outcome<std::vector<RecoveryOperation>> load_recovery_operations();
  OutcomeVoid validate_database_integrity();
  Outcome<std::int64_t> record_recovery_diagnostic(const std::string& project,
                                                   const std::string& message);
  Outcome<std::vector<RecoveryDiagnostic>> load_recovery_diagnostics();

 private:
  explicit MetadataStore(sqlite3* db) : db_(db) {}

  sqlite3* db_ = nullptr;
  std::mutex mutex_;
};

std::int64_t current_unix_time_seconds();
std::int64_t steady_clock_microseconds();

}  // namespace tribios
