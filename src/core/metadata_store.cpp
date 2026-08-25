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

}  // namespace

std::string_view workspace_state_name(WorkspaceState state) {
  switch (state) {
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

}  // namespace tribios
