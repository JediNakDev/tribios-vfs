#include "core/metadata_store.hpp"

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <variant>

namespace tribios {
namespace {

using SqlValue = std::variant<std::string, std::int64_t>;

// SQLite LIKE reads "_" and "%" as wildcards, so an unescaped path containing
// either would match unrelated siblings. Pairs with an ESCAPE clause.
std::string escape_like_wildcards(const std::string& text) {
  std::string escaped;
  for (char c : text) {
    if (c == '\\' || c == '%' || c == '_') escaped.push_back('\\');
    escaped.push_back(c);
  }
  return escaped;
}

constexpr const char* kSchema = R"sql(
PRAGMA journal_mode = WAL;
PRAGMA synchronous = FULL;
CREATE TABLE IF NOT EXISTS project (
  id INTEGER PRIMARY KEY CHECK (id = 1),
  root TEXT NOT NULL,
  mount_point TEXT NOT NULL,
  base_captured_at INTEGER NOT NULL,
  base_capture_ms INTEGER NOT NULL,
  base_entry_count INTEGER NOT NULL,
  base_bytes INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS workspace (
  name TEXT PRIMARY KEY,
  branch TEXT NOT NULL,
  state TEXT NOT NULL,
  created_at INTEGER NOT NULL,
  create_us INTEGER NOT NULL,
  removed_at INTEGER NOT NULL,
  logical_remove_us INTEGER NOT NULL,
  reclaim_us INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS tombstone (
  workspace TEXT NOT NULL,
  path TEXT NOT NULL,
  PRIMARY KEY (workspace, path)
);
CREATE TABLE IF NOT EXISTS metadata_format (
  id INTEGER PRIMARY KEY CHECK (id = 1),
  version INTEGER NOT NULL
);
INSERT OR IGNORE INTO metadata_format (id, version) VALUES (1, 1);
CREATE TABLE IF NOT EXISTS recovery_operation (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  workspace TEXT NOT NULL,
  kind TEXT NOT NULL,
  path TEXT NOT NULL,
  target TEXT NOT NULL,
  phase TEXT NOT NULL CHECK (phase IN ('prepared', 'publishing')),
  add_path_tombstone INTEGER NOT NULL CHECK (add_path_tombstone IN (0, 1)),
  drop_target_tombstones INTEGER NOT NULL CHECK (drop_target_tombstones IN (0, 1))
);
CREATE TABLE IF NOT EXISTS recovery_diagnostic (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  created_at INTEGER NOT NULL,
  project TEXT NOT NULL,
  message TEXT NOT NULL UNIQUE
);
PRAGMA user_version = 1;
)sql";

constexpr const char* kWorkspaceColumns =
    "name, branch, state, created_at, create_us, removed_at, "
    "logical_remove_us, reclaim_us";

sqlite3_stmt* prepare_bound_statement(sqlite3* db, const std::string& sql,
                                      const std::vector<SqlValue>& values) {
  sqlite3_stmt* statement = nullptr;
  if (sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK) return nullptr;
  int index = 1;
  for (const auto& value : values) {
    if (const auto* text = std::get_if<std::string>(&value)) {
      sqlite3_bind_text(statement, index, text->c_str(), -1, SQLITE_TRANSIENT);
    } else {
      sqlite3_bind_int64(statement, index, std::get<std::int64_t>(value));
    }
    ++index;
  }
  return statement;
}

std::string column_text(sqlite3_stmt* statement, int column) {
  const unsigned char* value = sqlite3_column_text(statement, column);
  return value == nullptr ? std::string{} : reinterpret_cast<const char*>(value);
}

std::optional<WorkspaceState> parse_workspace_state(std::string_view state) {
  if (state == "creating") return WorkspaceState::Creating;
  if (state == "active") return WorkspaceState::Active;
  if (state == "removed") return WorkspaceState::Removed;
  if (state == "reclaimed") return WorkspaceState::Reclaimed;
  return std::nullopt;
}

std::optional<WorkspaceRecord> read_workspace_record(sqlite3_stmt* statement) {
  auto state = parse_workspace_state(column_text(statement, 2));
  if (!state) return std::nullopt;
  return WorkspaceRecord{column_text(statement, 0),
                         column_text(statement, 1),
                         *state,
                         sqlite3_column_int64(statement, 3),
                         sqlite3_column_int64(statement, 4),
                         sqlite3_column_int64(statement, 5),
                         sqlite3_column_int64(statement, 6),
                         sqlite3_column_int64(statement, 7)};
}

std::string_view recovery_phase_name(RecoveryPhase phase) {
  return phase == RecoveryPhase::Publishing ? "publishing" : "prepared";
}

std::optional<RecoveryPhase> parse_recovery_phase(std::string_view phase) {
  if (phase == "prepared") return RecoveryPhase::Prepared;
  if (phase == "publishing") return RecoveryPhase::Publishing;
  return std::nullopt;
}

OutcomeVoid execute_sql(sqlite3* db, const char* sql) {
  char* message = nullptr;
  if (sqlite3_exec(db, sql, nullptr, nullptr, &message) == SQLITE_OK) return {};
  const std::string detail = message == nullptr ? sqlite3_errmsg(db) : message;
  sqlite3_free(message);
  return error("metadata store: " + detail);
}

}  // namespace

std::string_view workspace_state_name(WorkspaceState state) {
  switch (state) {
    case WorkspaceState::Creating:
      return "creating";
    case WorkspaceState::Removed:
      return "removed";
    case WorkspaceState::Reclaimed:
      return "reclaimed";
    case WorkspaceState::Active:
      break;
  }
  return "active";
}

std::int64_t current_unix_time_seconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::int64_t steady_clock_microseconds() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

MetadataStore::~MetadataStore() { sqlite3_close(db_); }

Outcome<std::unique_ptr<MetadataStore>> MetadataStore::open_database(
    const std::filesystem::path& database_path) {
  sqlite3* db = nullptr;
  const int opened =
      sqlite3_open_v2(database_path.c_str(), &db,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
  if (opened != SQLITE_OK) {
    std::string message = db == nullptr ? "cannot open database" : sqlite3_errmsg(db);
    sqlite3_close(db);
    return error("metadata store: " + message);
  }
  char* schema_error = nullptr;
  if (sqlite3_exec(db, kSchema, nullptr, nullptr, &schema_error) != SQLITE_OK) {
    const std::string message = schema_error == nullptr ? "cannot create schema" : schema_error;
    sqlite3_free(schema_error);
    sqlite3_close(db);
    return error("metadata store: " + message);
  }
  sqlite3_stmt* version = prepare_bound_statement(
      db, "SELECT version FROM metadata_format WHERE id = 1", {});
  if (version == nullptr || sqlite3_step(version) != SQLITE_ROW ||
      sqlite3_column_int(version, 0) != 1) {
    if (version != nullptr) sqlite3_finalize(version);
    sqlite3_close(db);
    return error("metadata store: unsupported metadata format version");
  }
  sqlite3_finalize(version);
  return std::unique_ptr<MetadataStore>(new MetadataStore(db));
}

Outcome<std::unique_ptr<MetadataStore>> MetadataStore::open_database_read_only(
    const std::filesystem::path& database_path) {
  sqlite3* db = nullptr;
  const int opened = sqlite3_open_v2(database_path.c_str(), &db,
                                     SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, nullptr);
  if (opened != SQLITE_OK) {
    const std::string message = db == nullptr ? "cannot open database" : sqlite3_errmsg(db);
    sqlite3_close(db);
    return error("metadata store: " + message);
  }
  sqlite3_stmt* version = prepare_bound_statement(
      db, "SELECT version FROM metadata_format WHERE id = 1", {});
  if (version == nullptr || sqlite3_step(version) != SQLITE_ROW ||
      sqlite3_column_int(version, 0) != 1) {
    if (version != nullptr) sqlite3_finalize(version);
    sqlite3_close(db);
    return error("metadata store: unsupported metadata format version");
  }
  sqlite3_finalize(version);
  return std::unique_ptr<MetadataStore>(new MetadataStore(db));
}

namespace {

OutcomeVoid execute_statement_without_rows(sqlite3* db, std::mutex& mutex, const std::string& sql,
                                           const std::vector<SqlValue>& values) {
  std::lock_guard lock(mutex);
  sqlite3_stmt* statement = prepare_bound_statement(db, sql, values);
  if (statement == nullptr) return error(std::string("metadata store: ") + sqlite3_errmsg(db));
  const int stepped = sqlite3_step(statement);
  sqlite3_finalize(statement);
  if (stepped != SQLITE_DONE) return error(std::string("metadata store: ") + sqlite3_errmsg(db));
  return {};
}

}  // namespace

OutcomeVoid MetadataStore::save_project_record(const ProjectRecord& record) {
  return execute_statement_without_rows(
      db_, mutex_,
      "INSERT OR REPLACE INTO project (id, root, mount_point, "
      "base_captured_at, "
      "base_capture_ms, base_entry_count, base_bytes) VALUES (1, ?, ?, ?, ?, "
      "?, ?)",
      {record.root, record.mount_point, record.base_captured_at, record.base_capture_ms,
       record.base_entry_count, record.base_bytes});
}

std::optional<ProjectRecord> MetadataStore::load_project_record() {
  std::lock_guard lock(mutex_);
  sqlite3_stmt* statement = prepare_bound_statement(db_,
                                                    "SELECT root, mount_point, base_captured_at, "
                                                    "base_capture_ms, base_entry_count, "
                                                    "base_bytes FROM project",
                                                    {});
  if (statement == nullptr) return std::nullopt;
  std::optional<ProjectRecord> record;
  if (sqlite3_step(statement) == SQLITE_ROW) {
    record = ProjectRecord{column_text(statement, 0),          column_text(statement, 1),
                           sqlite3_column_int64(statement, 2), sqlite3_column_int64(statement, 3),
                           sqlite3_column_int64(statement, 4), sqlite3_column_int64(statement, 5)};
  }
  sqlite3_finalize(statement);
  return record;
}

OutcomeVoid MetadataStore::save_workspace_record(const WorkspaceRecord& record) {
  return execute_statement_without_rows(
      db_, mutex_,
      std::string("INSERT OR REPLACE INTO workspace (") + kWorkspaceColumns +
          ") VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
      {record.name, record.branch, std::string(workspace_state_name(record.state)),
       record.created_at, record.create_us, record.removed_at, record.logical_remove_us,
       record.reclaim_us});
}

std::optional<WorkspaceRecord> MetadataStore::load_workspace_record(const std::string& name) {
  std::lock_guard lock(mutex_);
  sqlite3_stmt* statement = prepare_bound_statement(
      db_, std::string("SELECT ") + kWorkspaceColumns + " FROM workspace WHERE name = ?", {name});
  if (statement == nullptr) return std::nullopt;
  std::optional<WorkspaceRecord> record;
  if (sqlite3_step(statement) == SQLITE_ROW) record = read_workspace_record(statement);
  sqlite3_finalize(statement);
  return record;
}

std::vector<WorkspaceRecord> MetadataStore::load_all_workspace_records() {
  std::lock_guard lock(mutex_);
  std::vector<WorkspaceRecord> records;
  sqlite3_stmt* statement = prepare_bound_statement(
      db_, std::string("SELECT ") + kWorkspaceColumns + " FROM workspace ORDER BY created_at, name",
      {});
  if (statement == nullptr) return records;
  while (sqlite3_step(statement) == SQLITE_ROW) {
    if (auto record = read_workspace_record(statement)) records.push_back(*record);
  }
  sqlite3_finalize(statement);
  return records;
}

OutcomeVoid MetadataStore::set_workspace_state(const std::string& name, WorkspaceState state) {
  return execute_statement_without_rows(db_, mutex_,
                                        "UPDATE workspace SET state = ? WHERE name = ?",
                                        {std::string(workspace_state_name(state)), name});
}

OutcomeVoid MetadataStore::set_workspace_reclamation_duration(const std::string& name,
                                                              std::int64_t reclaim_us) {
  return execute_statement_without_rows(
      db_, mutex_, "UPDATE workspace SET reclaim_us = ? WHERE name = ?", {reclaim_us, name});
}

OutcomeVoid MetadataStore::add_tombstone(const std::string& workspace, const std::string& path) {
  return execute_statement_without_rows(
      db_, mutex_, "INSERT OR IGNORE INTO tombstone (workspace, path) VALUES (?, ?)",
      {workspace, path});
}

OutcomeVoid MetadataStore::remove_tombstones_under(const std::string& workspace,
                                                   const std::string& path) {
  return execute_statement_without_rows(
      db_, mutex_,
      "DELETE FROM tombstone WHERE workspace = ? AND (path = ? OR path LIKE ? ESCAPE '\\')",
      {workspace, path, escape_like_wildcards(path) + "/%"});
}

OutcomeVoid MetadataStore::clear_tombstones(const std::string& workspace) {
  return execute_statement_without_rows(db_, mutex_, "DELETE FROM tombstone WHERE workspace = ?",
                                        {workspace});
}

std::vector<std::string> MetadataStore::load_workspace_tombstones(const std::string& workspace) {
  std::lock_guard lock(mutex_);
  std::vector<std::string> paths;
  sqlite3_stmt* statement =
      prepare_bound_statement(db_, "SELECT path FROM tombstone WHERE workspace = ?", {workspace});
  if (statement == nullptr) return paths;
  while (sqlite3_step(statement) == SQLITE_ROW) paths.push_back(column_text(statement, 0));
  sqlite3_finalize(statement);
  return paths;
}

Outcome<std::int64_t> MetadataStore::begin_recovery_operation(
    const RecoveryOperation& operation) {
  std::lock_guard lock(mutex_);
  sqlite3_stmt* statement = prepare_bound_statement(
      db_,
      "INSERT INTO recovery_operation (workspace, kind, path, target, phase, "
      "add_path_tombstone, drop_target_tombstones) VALUES (?, ?, ?, ?, ?, ?, ?)",
      {operation.workspace, operation.kind, operation.path, operation.target,
       std::string(recovery_phase_name(operation.phase)),
       static_cast<std::int64_t>(operation.add_path_tombstone),
       static_cast<std::int64_t>(operation.drop_target_tombstones)});
  if (statement == nullptr) return error(std::string("metadata store: ") + sqlite3_errmsg(db_));
  const int stepped = sqlite3_step(statement);
  sqlite3_finalize(statement);
  if (stepped != SQLITE_DONE) return error(std::string("metadata store: ") + sqlite3_errmsg(db_));
  return sqlite3_last_insert_rowid(db_);
}

OutcomeVoid MetadataStore::set_recovery_operation_phase(std::int64_t id, RecoveryPhase phase) {
  return execute_statement_without_rows(
      db_, mutex_, "UPDATE recovery_operation SET phase = ? WHERE id = ?",
      {std::string(recovery_phase_name(phase)), id});
}

OutcomeVoid MetadataStore::finish_recovery_operation(const RecoveryOperation& operation) {
  std::lock_guard lock(mutex_);
  if (auto begun = execute_sql(db_, "BEGIN IMMEDIATE"); !begun) return begun;

  auto run = [&](const std::string& sql, const std::vector<SqlValue>& values) -> OutcomeVoid {
    sqlite3_stmt* statement = prepare_bound_statement(db_, sql, values);
    if (statement == nullptr) return error(std::string("metadata store: ") + sqlite3_errmsg(db_));
    const int stepped = sqlite3_step(statement);
    sqlite3_finalize(statement);
    if (stepped != SQLITE_DONE) return error(std::string("metadata store: ") + sqlite3_errmsg(db_));
    return {};
  };

  OutcomeVoid result;
  if (operation.add_path_tombstone) {
    result = run("INSERT OR IGNORE INTO tombstone (workspace, path) VALUES (?, ?)",
                 {operation.workspace, operation.path});
  }
  if (result && operation.drop_target_tombstones) {
    result = run(
        "DELETE FROM tombstone WHERE workspace = ? AND (path = ? OR path LIKE ? ESCAPE '\\')",
        {operation.workspace, operation.target,
         escape_like_wildcards(operation.target) + "/%"});
  }
  if (result) {
    result = run("DELETE FROM recovery_operation WHERE id = ?", {operation.id});
  }
  if (!result) {
    execute_sql(db_, "ROLLBACK");
    return result;
  }
  return execute_sql(db_, "COMMIT");
}

OutcomeVoid MetadataStore::abandon_recovery_operation(std::int64_t id) {
  return execute_statement_without_rows(db_, mutex_,
                                        "DELETE FROM recovery_operation WHERE id = ?", {id});
}

Outcome<std::vector<RecoveryOperation>> MetadataStore::load_recovery_operations() {
  std::lock_guard lock(mutex_);
  sqlite3_stmt* statement = prepare_bound_statement(
      db_,
      "SELECT id, workspace, kind, path, target, phase, add_path_tombstone, "
      "drop_target_tombstones FROM recovery_operation ORDER BY id",
      {});
  if (statement == nullptr) return error(std::string("metadata store: ") + sqlite3_errmsg(db_));

  std::vector<RecoveryOperation> operations;
  while (sqlite3_step(statement) == SQLITE_ROW) {
    auto phase = parse_recovery_phase(column_text(statement, 5));
    if (!phase) {
      sqlite3_finalize(statement);
      return error("metadata store: recovery operation has an invalid phase");
    }
    operations.push_back(RecoveryOperation{
        sqlite3_column_int64(statement, 0), column_text(statement, 1),
        column_text(statement, 2), column_text(statement, 3), column_text(statement, 4),
        *phase, sqlite3_column_int(statement, 6) != 0,
        sqlite3_column_int(statement, 7) != 0});
  }
  sqlite3_finalize(statement);
  return operations;
}

OutcomeVoid MetadataStore::validate_database_integrity() {
  std::lock_guard lock(mutex_);
  sqlite3_stmt* statement = prepare_bound_statement(db_, "PRAGMA quick_check", {});
  if (statement == nullptr) return error(std::string("metadata store: ") + sqlite3_errmsg(db_));
  const bool valid = sqlite3_step(statement) == SQLITE_ROW && column_text(statement, 0) == "ok";
  const std::string detail = valid ? std::string{} : column_text(statement, 0);
  sqlite3_finalize(statement);
  if (!valid) return error("metadata store integrity check failed: " + detail);
  return {};
}

Outcome<std::int64_t> MetadataStore::record_recovery_diagnostic(const std::string& project,
                                                                const std::string& message) {
  std::lock_guard lock(mutex_);
  sqlite3_stmt* inserted = prepare_bound_statement(
      db_,
      "INSERT OR IGNORE INTO recovery_diagnostic (created_at, project, message) VALUES (?, ?, ?)",
      {current_unix_time_seconds(), project, message});
  if (inserted == nullptr) return error(std::string("metadata store: ") + sqlite3_errmsg(db_));
  const int stepped = sqlite3_step(inserted);
  sqlite3_finalize(inserted);
  if (stepped != SQLITE_DONE) return error(std::string("metadata store: ") + sqlite3_errmsg(db_));

  sqlite3_stmt* selected = prepare_bound_statement(
      db_, "SELECT id FROM recovery_diagnostic WHERE message = ?", {message});
  if (selected == nullptr || sqlite3_step(selected) != SQLITE_ROW) {
    if (selected != nullptr) sqlite3_finalize(selected);
    return error(std::string("metadata store: ") + sqlite3_errmsg(db_));
  }
  const std::int64_t id = sqlite3_column_int64(selected, 0);
  sqlite3_finalize(selected);
  return id;
}

Outcome<std::vector<RecoveryDiagnostic>> MetadataStore::load_recovery_diagnostics() {
  std::lock_guard lock(mutex_);
  sqlite3_stmt* statement = prepare_bound_statement(
      db_, "SELECT id, created_at, project, message FROM recovery_diagnostic ORDER BY id", {});
  if (statement == nullptr) return error(std::string("metadata store: ") + sqlite3_errmsg(db_));
  std::vector<RecoveryDiagnostic> diagnostics;
  while (sqlite3_step(statement) == SQLITE_ROW) {
    diagnostics.push_back(RecoveryDiagnostic{sqlite3_column_int64(statement, 0),
                                             sqlite3_column_int64(statement, 1),
                                             column_text(statement, 2),
                                             column_text(statement, 3)});
  }
  sqlite3_finalize(statement);
  return diagnostics;
}

}  // namespace tribios
