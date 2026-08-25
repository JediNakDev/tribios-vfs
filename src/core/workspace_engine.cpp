#include "core/workspace_engine.hpp"

#include <fcntl.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <system_error>

#include "core/fault_injection.hpp"
#include "core/paths.hpp"
#include "core/recovery.hpp"

namespace tribios {
namespace {

bool lstat_path(const std::filesystem::path& path, struct stat& out) {
  return ::lstat(path.c_str(), &out) == 0;
}

Attr attr_from_stat(const struct stat& st, std::uint64_t ino, bool from_upper) {
  return Attr{ino,         st.st_mode,  static_cast<std::uint64_t>(st.st_size),
              st.st_nlink, st.st_mtime, st.st_atime,
              st.st_ctime, from_upper};
}

std::uint64_t fnv1a_hash(std::string_view text) {
  std::uint64_t hash = 14695981039346656037ULL;
  for (const unsigned char byte : text) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

// The splitmix64 finalizer: it spreads the salted backing inode across the
// whole 64-bit range so that neighbouring inodes do not collide after mixing.
std::uint64_t mix_bits(std::uint64_t value) {
  value ^= value >> 30;
  value *= 0xbf58476d1ce4e5b9ULL;
  value ^= value >> 27;
  value *= 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}

int last_errno() { return errno != 0 ? errno : EIO; }

bool path_exists_without_following_symlinks(const std::filesystem::path& path) {
  struct stat st {};
  return lstat_path(path, st);
}

int sync_entry_tree(const std::filesystem::path& path) {
  struct stat st {};
  if (!lstat_path(path, st)) return last_errno();
  if (S_ISREG(st.st_mode)) return sync_file_data(path);
  if (!S_ISDIR(st.st_mode)) return 0;

  std::error_code ec;
  for (const auto& child : std::filesystem::directory_iterator(path, ec)) {
    if (ec) return EIO;
    if (const int synced = sync_entry_tree(child.path()); synced != 0) return synced;
  }
  return sync_directory(path);
}

}  // namespace

WorkspaceEngine::WorkspaceEngine(std::string workspace, std::filesystem::path base_dir,
                                 std::filesystem::path upper_dir, MetadataStore& store)
    : workspace_(std::move(workspace)),
      inode_salt_(fnv1a_hash(workspace_)),
      base_dir_(std::move(base_dir)),
      upper_dir_(std::move(upper_dir)),
      store_(store) {
  // Tombstones are persistent: a removed Base-state path stays absent across
  // daemon restarts.
  for (auto& path : store_.load_workspace_tombstones(workspace_)) {
    tombstones_.insert(std::move(path));
  }
}

WorkspaceEngine::~WorkspaceEngine() {
  std::lock_guard lock(handles_mutex_);
  for (const auto& [handle, open] : handles_) {
    (void)handle;
    if (open.backing_fd >= 0) ::close(open.backing_fd);
  }
}

std::filesystem::path WorkspaceEngine::upper_path(std::string_view relative) const {
  return relative.empty() ? upper_dir_ : upper_dir_ / std::string(relative);
}

std::filesystem::path WorkspaceEngine::base_path(std::string_view relative) const {
  return relative.empty() ? base_dir_ : base_dir_ / std::string(relative);
}

// A tombstone hides the whole Base-state subtree beneath it, so a re-created
// directory never resurrects the children that were removed with it.
bool WorkspaceEngine::base_is_visible(const std::string& relative) const {
  for (std::string path = relative; !path.empty(); path = parent_of(path)) {
    if (tombstones_.contains(path)) return false;
  }
  return true;
}

WorkspaceEngine::Layer WorkspaceEngine::find_visible_entry_layer(const std::string& relative,
                                                                 struct stat& out) const {
  if (lstat_path(upper_path(relative), out)) return Layer::Upper;
  if (base_is_visible(relative) && lstat_path(base_path(relative), out)) return Layer::Base;
  return Layer::Missing;
}

void WorkspaceEngine::merge_visible_directory_entries(
    const std::string& relative, std::map<std::string, DirEntry>& entries) const {
  std::error_code ec;
  if (base_is_visible(relative)) {
    for (const auto& entry : std::filesystem::directory_iterator(
             base_path(relative), std::filesystem::directory_options::none, ec)) {
      const std::string name = entry.path().filename().string();
      struct stat st{};
      if (tombstones_.contains(join_relative(relative, name)) || !lstat_path(entry.path(), st)) {
        continue;
      }
      entries[name] = DirEntry{name, workspace_inode_for(st), st.st_mode};
    }
  }
  ec.clear();
  for (const auto& entry : std::filesystem::directory_iterator(
           upper_path(relative), std::filesystem::directory_options::none, ec)) {
    const std::string name = entry.path().filename().string();
    struct stat st{};
    if (lstat_path(entry.path(), st)) {
      entries[name] = DirEntry{name, workspace_inode_for(st), st.st_mode};
    }
  }
}

Result<Attr> WorkspaceEngine::getattr(std::string_view path) {
  const std::string relative = normalize_relative(path);
  std::shared_lock lock(mutex_);
  struct stat st{};
  const Layer layer = find_visible_entry_layer(relative, st);
  if (layer == Layer::Missing) return fail(ENOENT);
  return attr_from_stat(st, workspace_inode_for(st), layer == Layer::Upper);
}

// Zero is reserved: a directory entry carrying inode zero reads as a deleted
// slot, and callers treat a zero inode as "unknown".
std::uint64_t WorkspaceEngine::workspace_inode_for(const struct stat& backing) const {
  const std::uint64_t mixed =
      mix_bits(static_cast<std::uint64_t>(backing.st_ino) + inode_salt_);
  return mixed == 0 ? 1 : mixed;
}

Result<std::vector<DirEntry>> WorkspaceEngine::readdir(std::string_view path) {
  const std::string relative = normalize_relative(path);
  std::shared_lock lock(mutex_);
  struct stat st{};
  if (find_visible_entry_layer(relative, st) == Layer::Missing) return fail(ENOENT);
  if (!S_ISDIR(st.st_mode)) return fail(ENOTDIR);

  std::map<std::string, DirEntry> merged;
  merge_visible_directory_entries(relative, merged);
  std::vector<DirEntry> out;
  out.reserve(merged.size());
  for (auto& [name, entry] : merged) out.push_back(entry);
  return out;
}

Result<std::string> WorkspaceEngine::readlink(std::string_view path) {
  const std::string relative = normalize_relative(path);
  std::shared_lock lock(mutex_);
  struct stat st{};
  const Layer layer = find_visible_entry_layer(relative, st);
  if (layer == Layer::Missing) return fail(ENOENT);
  if (!S_ISLNK(st.st_mode)) return fail(EINVAL);
  std::error_code ec;
  const std::filesystem::path target = std::filesystem::read_symlink(
      layer == Layer::Upper ? upper_path(relative) : base_path(relative), ec);
  if (ec) return fail(EIO);
  return target.string();
}

Status WorkspaceEngine::create_missing_upper_parent_directories(const std::string& relative) {
  std::vector<std::string> missing;
  for (std::string dir = parent_of(relative); !dir.empty(); dir = parent_of(dir)) {
    missing.push_back(dir);
  }
  std::reverse(missing.begin(), missing.end());

  for (const auto& dir : missing) {
    struct stat st{};
    if (lstat_path(upper_path(dir), st)) {
      if (!S_ISDIR(st.st_mode)) return fail(ENOTDIR);
      continue;
    }
    if (!base_is_visible(dir) || !lstat_path(base_path(dir), st)) return fail(ENOENT);
    if (!S_ISDIR(st.st_mode)) return fail(ENOTDIR);
    if (::mkdir(upper_path(dir).c_str(), st.st_mode & 07777) != 0 && errno != EEXIST) {
      return fail(last_errno());
    }
    if (sync_parent_directory(upper_path(dir)) != 0) return fail(EIO);
  }
  return {};
}

Status WorkspaceEngine::materialize_visible_entry_at(
    const std::string& relative, const std::filesystem::path& destination) {
  struct stat st{};
  const Layer layer = find_visible_entry_layer(relative, st);
  if (layer == Layer::Missing) return fail(ENOENT);
  const std::filesystem::path source =
      layer == Layer::Upper ? upper_path(relative) : base_path(relative);
  std::error_code ec;
  if (S_ISDIR(st.st_mode)) {
    if (::mkdir(destination.c_str(), st.st_mode & 07777) != 0 && errno != EEXIST) {
      return fail(last_errno());
    }
    std::map<std::string, DirEntry> children;
    merge_visible_directory_entries(relative, children);
    for (const auto& [name, entry] : children) {
      (void)entry;
      if (auto copied = materialize_visible_entry_at(join_relative(relative, name),
                                                     destination / name);
          !copied) {
        return copied;
      }
    }
    const timeval times[2] = {{st.st_atime, 0}, {st.st_mtime, 0}};
    ::utimes(destination.c_str(), times);
    return {};
  }
  if (S_ISLNK(st.st_mode)) {
    std::filesystem::create_symlink(std::filesystem::read_symlink(source, ec), destination, ec);
    return ec ? Status(fail(EIO)) : Status{};
  }
  std::filesystem::copy_file(source, destination, std::filesystem::copy_options::none, ec);
  if (ec) return fail(EIO);
  if (::chmod(destination.c_str(), st.st_mode & 07777) != 0) return fail(last_errno());
  const timeval times[2] = {{st.st_atime, 0}, {st.st_mtime, 0}};
  if (::utimes(destination.c_str(), times) != 0) return fail(last_errno());
  return {};
}

Status WorkspaceEngine::publish_staged_operation(
    RecoveryOperation operation,
    const std::function<Status(const std::filesystem::path&)>& prepare_stage) {
  if (const int fault = injected_io_error("journal.write"); fault != 0) return fail(fault);
  auto operation_id = store_.begin_recovery_operation(operation);
  if (!operation_id) return fail(EIO);
  operation.id = *operation_id;

  const std::filesystem::path workspace_dir = upper_dir_.parent_path();
  const std::filesystem::path recovery_dir = workspace_dir / "recovery";
  const std::filesystem::path stage = recovery_dir / std::to_string(operation.id);
  const std::filesystem::path tribios_dir = workspace_dir.parent_path().parent_path();
  const std::string context = "operation=" + std::to_string(operation.id) +
                              " workspace=" + workspace_ + " path=" + operation.path;
  const auto failpoint = [&](std::string_view suffix) {
    trigger_failpoint(operation.kind + "." + std::string(suffix), tribios_dir, context);
  };
  failpoint("after_journal");

  std::error_code ec;
  std::filesystem::create_directories(recovery_dir, ec);
  if (ec || sync_parent_directory(recovery_dir) != 0) {
    store_.abandon_recovery_operation(operation.id);
    return fail(ec ? EIO : last_errno());
  }
  if (auto prepared = prepare_stage(stage); !prepared) {
    std::filesystem::remove_all(stage, ec);
    store_.abandon_recovery_operation(operation.id);
    return prepared;
  }
  if (const int fault = injected_io_error("stage.flush"); fault != 0) {
    std::filesystem::remove_all(stage, ec);
    store_.abandon_recovery_operation(operation.id);
    return fail(fault);
  }
  if (const int synced = sync_entry_tree(stage); synced != 0 || sync_directory(recovery_dir) != 0) {
    std::filesystem::remove_all(stage, ec);
    store_.abandon_recovery_operation(operation.id);
    return fail(synced != 0 ? synced : EIO);
  }
  failpoint("after_stage_flush");

  if (const int fault = injected_io_error("journal.phase"); fault != 0) {
    std::filesystem::remove_all(stage, ec);
    store_.abandon_recovery_operation(operation.id);
    return fail(fault);
  }
  if (auto publishing =
          store_.set_recovery_operation_phase(operation.id, RecoveryPhase::Publishing);
      !publishing) {
    std::filesystem::remove_all(stage, ec);
    store_.abandon_recovery_operation(operation.id);
    return fail(EIO);
  }
  operation.phase = RecoveryPhase::Publishing;
  failpoint("before_publish");

  const std::string destination_relative =
      operation.kind == "rename" ? operation.target : operation.path;
  const std::filesystem::path destination = upper_path(destination_relative);
  const auto abandon_before_publication = [&](int fault) -> Status {
    std::filesystem::remove_all(stage, ec);
    store_.abandon_recovery_operation(operation.id);
    return fail(fault);
  };
  if (const int fault = injected_io_error("publish.rename"); fault != 0) {
    return abandon_before_publication(fault);
  }
  // Preflight injected flush and final-journal failures before publication so
  // a reported storage error leaves the old visible state intact.
  if (const int fault = injected_io_error("publish.directory_flush"); fault != 0) {
    return abandon_before_publication(fault);
  }
  if (const int fault = injected_io_error("journal.finish"); fault != 0) {
    return abandon_before_publication(fault);
  }
  if (::rename(stage.c_str(), destination.c_str()) != 0) {
    return abandon_before_publication(last_errno());
  }
  failpoint("after_publish");
  if (sync_parent_directory(destination) != 0) return fail(EIO);

  if (operation.kind == "rename") {
    const std::filesystem::path source = upper_path(operation.path);
    const bool source_was_materialized = path_exists_without_following_symlinks(source);
    std::filesystem::remove_all(source, ec);
    if (ec || (source_was_materialized && sync_parent_directory(source) != 0)) return fail(EIO);
    failpoint("after_source_remove");
  }

  if (auto finished = store_.finish_recovery_operation(operation); !finished) return fail(EIO);
  if (operation.add_path_tombstone) tombstones_.insert(operation.path);
  if (operation.drop_target_tombstones) {
    std::erase_if(tombstones_, [&](const std::string& path) {
      return path == operation.target || path.starts_with(operation.target + "/");
    });
  }
  failpoint("after_metadata_commit");
  std::filesystem::remove(recovery_dir, ec);
  return {};
}

Status WorkspaceEngine::remove_visible_entry_atomically(const std::string& kind,
                                                        const std::string& relative,
                                                        bool directory) {
  struct stat base_stat {};
  RecoveryOperation operation;
  operation.workspace = workspace_;
  operation.kind = kind;
  operation.path = relative;
  operation.add_path_tombstone =
      base_is_visible(relative) && lstat_path(base_path(relative), base_stat);
  auto operation_id = store_.begin_recovery_operation(operation);
  if (!operation_id) return fail(EIO);
  operation.id = *operation_id;

  const std::filesystem::path tribios_dir = upper_dir_.parent_path().parent_path().parent_path();
  const std::string context = "operation=" + std::to_string(operation.id) +
                              " workspace=" + workspace_ + " path=" + relative;
  trigger_failpoint(kind + ".after_journal", tribios_dir, context);
  if (auto publishing =
          store_.set_recovery_operation_phase(operation.id, RecoveryPhase::Publishing);
      !publishing) {
    store_.abandon_recovery_operation(operation.id);
    return fail(EIO);
  }
  operation.phase = RecoveryPhase::Publishing;
  trigger_failpoint(kind + ".before_upper_remove", tribios_dir, context);

  if (path_exists_without_following_symlinks(upper_path(relative))) {
    std::error_code ec;
    if (directory) {
      std::filesystem::remove_all(upper_path(relative), ec);
    } else {
      std::filesystem::remove(upper_path(relative), ec);
    }
    if (ec) {
      store_.abandon_recovery_operation(operation.id);
      return fail(EIO);
    }
    if (sync_parent_directory(upper_path(relative)) != 0) return fail(EIO);
  }
  trigger_failpoint(kind + ".after_upper_remove", tribios_dir, context);

  if (auto finished = store_.finish_recovery_operation(operation); !finished) return fail(EIO);
  if (operation.add_path_tombstone) tombstones_.insert(relative);
  trigger_failpoint(kind + ".after_metadata_commit", tribios_dir, context);
  return {};
}

Result<int> WorkspaceEngine::open_handle(std::string_view path, int flags) {
  const std::string relative = normalize_relative(path);
  if (relative.empty()) return fail(EISDIR);
  const bool for_writing = (flags & (O_WRONLY | O_RDWR | O_APPEND | O_TRUNC | O_CREAT)) != 0;
  std::unique_lock lock(mutex_);
  struct stat st{};
  Layer layer = find_visible_entry_layer(relative, st);
  if (layer == Layer::Missing) {
    if ((flags & O_CREAT) == 0) return fail(ENOENT);
    if (auto parents = create_missing_upper_parent_directories(relative); !parents) {
      return std::unexpected(parents.error());
    }
    RecoveryOperation operation;
    operation.workspace = workspace_;
    operation.kind = "create";
    operation.path = relative;
    auto created = publish_staged_operation(operation, [&](const std::filesystem::path& stage) {
      const int fd = ::open(stage.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0644);
      if (fd < 0) return Status(fail(last_errno()));
      ::close(fd);
      return Status{};
    });
    if (!created) return std::unexpected(created.error());
    layer = Layer::Upper;
  } else if (S_ISDIR(st.st_mode)) {
    return fail(EISDIR);
  } else if ((flags & O_CREAT) != 0 && (flags & O_EXCL) != 0) {
    return fail(EEXIST);
  }

  if (for_writing && (flags & O_TRUNC) != 0) {
    if (auto parents = create_missing_upper_parent_directories(relative); !parents) {
      return std::unexpected(parents.error());
    }
    RecoveryOperation operation;
    operation.workspace = workspace_;
    operation.kind = "truncate";
    operation.path = relative;
    auto truncated = publish_staged_operation(operation, [&](const std::filesystem::path& stage) {
      if (auto copied = materialize_visible_entry_at(relative, stage); !copied) return copied;
      if (::truncate(stage.c_str(), 0) != 0) return Status(fail(last_errno()));
      return Status{};
    });
    if (!truncated) return std::unexpected(truncated.error());
    layer = Layer::Upper;
  }

  int backing_flags = flags & ~(O_CREAT | O_EXCL | O_TRUNC);
  const std::filesystem::path backing_path =
      layer == Layer::Base ? base_path(relative) : upper_path(relative);
  if (layer == Layer::Base && for_writing) backing_flags = O_RDONLY;
  const int backing_fd = ::open(backing_path.c_str(), backing_flags, 0644);
  if (backing_fd < 0) return fail(last_errno());

  std::lock_guard handles_lock(handles_mutex_);
  const int handle = next_handle_++;
  handles_.emplace(handle, OpenHandle{backing_fd, flags, relative});
  return handle;
}

Result<std::size_t> WorkspaceEngine::read_handle(int fd, char* buffer, std::size_t size,
                                                 std::uint64_t offset) {
  std::lock_guard lock(handles_mutex_);
  auto found = handles_.find(fd);
  if (found == handles_.end()) return fail(EBADF);
  const ssize_t n = ::pread(found->second.backing_fd, buffer, size, static_cast<off_t>(offset));
  if (n < 0) return fail(last_errno());
  return static_cast<std::size_t>(n);
}

Result<std::size_t> WorkspaceEngine::write_handle(int fd, const char* buffer, std::size_t size,
                                                  std::uint64_t offset) {
  std::unique_lock lock(mutex_);
  std::lock_guard handles_lock(handles_mutex_);
  auto found = handles_.find(fd);
  if (found == handles_.end()) return fail(EBADF);
  const int access_mode = found->second.flags & O_ACCMODE;
  if (access_mode == O_RDONLY) return fail(EBADF);

  const std::string relative = found->second.relative;
  if (auto parents = create_missing_upper_parent_directories(relative); !parents) {
    return std::unexpected(parents.error());
  }
  std::size_t accepted = 0;
  RecoveryOperation operation;
  operation.workspace = workspace_;
  operation.kind = "write";
  operation.path = relative;
  auto written = publish_staged_operation(operation, [&](const std::filesystem::path& stage) {
    if (auto copied = materialize_visible_entry_at(relative, stage); !copied) return copied;
    const int stage_fd = ::open(stage.c_str(), O_WRONLY);
    if (stage_fd < 0) return Status(fail(last_errno()));
    const std::size_t write_size = injected_short_write_size(size);
    const ssize_t count = ::pwrite(stage_fd, buffer, write_size, static_cast<off_t>(offset));
    const int write_error = count < 0 ? last_errno() : 0;
    ::close(stage_fd);
    if (count < 0) return Status(fail(write_error));
    accepted = static_cast<std::size_t>(count);
    return Status{};
  });
  if (!written) return std::unexpected(written.error());

  ::close(found->second.backing_fd);
  const int reopen_flags = found->second.flags & ~(O_CREAT | O_EXCL | O_TRUNC);
  found->second.backing_fd = ::open(upper_path(relative).c_str(), reopen_flags, 0644);
  if (found->second.backing_fd < 0) return fail(last_errno());
  return accepted;
}

Status WorkspaceEngine::fsync_handle(int fd, bool data_only) {
  std::lock_guard lock(handles_mutex_);
  auto found = handles_.find(fd);
  if (found == handles_.end()) return fail(EBADF);
#ifdef __APPLE__
  (void)data_only;
  const int synced = ::fcntl(found->second.backing_fd, F_FULLFSYNC);
#else
  const int synced = data_only ? ::fdatasync(found->second.backing_fd) :
                                 ::fsync(found->second.backing_fd);
#endif
  if (synced != 0) return fail(last_errno());
  struct stat upper_stat {};
  if (lstat_path(upper_path(found->second.relative), upper_stat) &&
      sync_parent_directory(upper_path(found->second.relative)) != 0) {
    return fail(EIO);
  }
  return {};
}

Status WorkspaceEngine::fsync_path(std::string_view path, bool data_only, bool directory) {
  const std::string relative = normalize_relative(path);
  std::shared_lock lock(mutex_);
  struct stat st {};
  const Layer layer = find_visible_entry_layer(relative, st);
  if (layer == Layer::Missing) return fail(ENOENT);
  if (directory != S_ISDIR(st.st_mode)) return fail(directory ? ENOTDIR : EISDIR);

  const std::filesystem::path visible_path =
      layer == Layer::Upper ? upper_path(relative) : base_path(relative);
  int synced = 0;
  if (directory) {
    synced = sync_directory(visible_path);
  } else {
#ifdef __APPLE__
    (void)data_only;
    synced = sync_file_data(visible_path);
#else
    const int fd = ::open(visible_path.c_str(), O_RDONLY);
    if (fd < 0) return fail(last_errno());
    const int result = data_only ? ::fdatasync(fd) : ::fsync(fd);
    synced = result == 0 ? 0 : last_errno();
    ::close(fd);
#endif
  }
  if (synced != 0) return fail(synced);
  if (layer == Layer::Upper && sync_parent_directory(visible_path) != 0) return fail(EIO);
  return {};
}

void WorkspaceEngine::close_handle(int fd) {
  std::lock_guard lock(handles_mutex_);
  auto found = handles_.find(fd);
  if (found == handles_.end()) return;
  if (found->second.backing_fd >= 0) ::close(found->second.backing_fd);
  handles_.erase(found);
}

Result<std::string> WorkspaceEngine::read_file(std::string_view path, std::uint64_t size,
                                               std::uint64_t offset) {
  auto fd = open_handle(path, O_RDONLY);
  if (!fd) return std::unexpected(fd.error());
  std::string buffer(size, '\0');
  auto n = read_handle(*fd, buffer.data(), size, offset);
  close_handle(*fd);
  if (!n) return std::unexpected(n.error());
  buffer.resize(*n);
  return buffer;
}

Result<std::size_t> WorkspaceEngine::write_file(std::string_view path, std::string_view data,
                                                std::uint64_t offset) {
  auto fd = open_handle(path, O_RDWR | O_CREAT);
  if (!fd) return std::unexpected(fd.error());
  auto written = write_handle(*fd, data.data(), data.size(), offset);
  close_handle(*fd);
  return written;
}

Status WorkspaceEngine::create(std::string_view path, mode_t mode) {
  const std::string relative = normalize_relative(path);
  if (relative.empty()) return fail(EEXIST);
  std::unique_lock lock(mutex_);
  struct stat st{};
  if (find_visible_entry_layer(relative, st) != Layer::Missing) return fail(EEXIST);
  if (auto parents = create_missing_upper_parent_directories(relative); !parents) return parents;
  RecoveryOperation operation;
  operation.workspace = workspace_;
  operation.kind = "create";
  operation.path = relative;
  return publish_staged_operation(operation, [&](const std::filesystem::path& stage) {
    const int fd = ::open(stage.c_str(), O_CREAT | O_EXCL | O_WRONLY, mode & 07777);
    if (fd < 0) return Status(fail(last_errno()));
    ::close(fd);
    return Status{};
  });
}

Status WorkspaceEngine::mkdir(std::string_view path, mode_t mode) {
  const std::string relative = normalize_relative(path);
  if (relative.empty()) return fail(EEXIST);
  std::unique_lock lock(mutex_);
  struct stat st{};
  if (find_visible_entry_layer(relative, st) != Layer::Missing) return fail(EEXIST);
  if (auto parents = create_missing_upper_parent_directories(relative); !parents) return parents;
  RecoveryOperation operation;
  operation.workspace = workspace_;
  operation.kind = "mkdir";
  operation.path = relative;
  // Any tombstone on this path stays: the new directory starts empty.
  return publish_staged_operation(operation, [&](const std::filesystem::path& stage) {
    if (::mkdir(stage.c_str(), mode & 07777) != 0) return Status(fail(last_errno()));
    return Status{};
  });
}

Status WorkspaceEngine::unlink(std::string_view path) {
  const std::string relative = normalize_relative(path);
  if (relative.empty()) return fail(EISDIR);
  std::unique_lock lock(mutex_);
  struct stat st{};
  if (find_visible_entry_layer(relative, st) == Layer::Missing) return fail(ENOENT);
  if (S_ISDIR(st.st_mode)) return fail(EISDIR);
  return remove_visible_entry_atomically("unlink", relative, false);
}

Status WorkspaceEngine::rmdir(std::string_view path) {
  const std::string relative = normalize_relative(path);
  if (relative.empty()) return fail(EBUSY);
  std::unique_lock lock(mutex_);
  struct stat st{};
  if (find_visible_entry_layer(relative, st) == Layer::Missing) return fail(ENOENT);
  if (!S_ISDIR(st.st_mode)) return fail(ENOTDIR);
  std::map<std::string, DirEntry> children;
  merge_visible_directory_entries(relative, children);
  if (!children.empty()) return fail(ENOTEMPTY);
  return remove_visible_entry_atomically("rmdir", relative, true);
}

Status WorkspaceEngine::rename(std::string_view from_path, std::string_view to_path) {
  const std::string from = normalize_relative(from_path);
  const std::string to = normalize_relative(to_path);
  if (from.empty() || to.empty()) return fail(EBUSY);
  if (from == to) return {};
  if (to.starts_with(from + "/")) return fail(EINVAL);

  std::unique_lock lock(mutex_);
  struct stat source_stat {};
  if (find_visible_entry_layer(from, source_stat) == Layer::Missing) return fail(ENOENT);
  struct stat destination_stat {};
  if (find_visible_entry_layer(to, destination_stat) != Layer::Missing) {
    if (S_ISDIR(source_stat.st_mode) && !S_ISDIR(destination_stat.st_mode)) return fail(ENOTDIR);
    if (!S_ISDIR(source_stat.st_mode) && S_ISDIR(destination_stat.st_mode)) return fail(EISDIR);
    if (S_ISDIR(destination_stat.st_mode)) {
      std::map<std::string, DirEntry> destination_children;
      merge_visible_directory_entries(to, destination_children);
      if (!destination_children.empty()) return fail(ENOTEMPTY);
    }
  }
  if (auto parents = create_missing_upper_parent_directories(to); !parents) return parents;

  struct stat base_stat {};
  RecoveryOperation operation;
  operation.workspace = workspace_;
  operation.kind = "rename";
  operation.path = from;
  operation.target = to;
  operation.add_path_tombstone = base_is_visible(from) && lstat_path(base_path(from), base_stat);
  operation.drop_target_tombstones = true;
  return publish_staged_operation(operation, [&](const std::filesystem::path& stage) {
    return materialize_visible_entry_at(from, stage);
  });
}

Status WorkspaceEngine::symlink(std::string_view target, std::string_view link_path) {
  const std::string relative = normalize_relative(link_path);
  if (relative.empty()) return fail(EEXIST);
  std::unique_lock lock(mutex_);
  struct stat st{};
  if (find_visible_entry_layer(relative, st) != Layer::Missing) return fail(EEXIST);
  if (auto parents = create_missing_upper_parent_directories(relative); !parents) return parents;
  RecoveryOperation operation;
  operation.workspace = workspace_;
  operation.kind = "symlink";
  operation.path = relative;
  return publish_staged_operation(operation, [&](const std::filesystem::path& stage) {
    if (::symlink(std::string(target).c_str(), stage.c_str()) != 0) {
      return Status(fail(last_errno()));
    }
    return Status{};
  });
}

Status WorkspaceEngine::chmod(std::string_view path, mode_t mode) {
  const std::string relative = normalize_relative(path);
  std::unique_lock lock(mutex_);
  struct stat st {};
  if (find_visible_entry_layer(relative, st) == Layer::Missing) return fail(ENOENT);
  if (auto parents = create_missing_upper_parent_directories(relative); !parents) return parents;
  RecoveryOperation operation;
  operation.workspace = workspace_;
  operation.kind = "chmod";
  operation.path = relative;
  return publish_staged_operation(operation, [&](const std::filesystem::path& stage) {
    if (auto copied = materialize_visible_entry_at(relative, stage); !copied) return copied;
    if (::chmod(stage.c_str(), mode & 07777) != 0) return Status(fail(last_errno()));
    return Status{};
  });
}

Status WorkspaceEngine::truncate(std::string_view path, std::uint64_t size) {
  const std::string relative = normalize_relative(path);
  std::unique_lock lock(mutex_);
  struct stat st{};
  const Layer layer = find_visible_entry_layer(relative, st);
  if (layer == Layer::Missing) return fail(ENOENT);
  if (S_ISDIR(st.st_mode)) return fail(EISDIR);
  if (auto parents = create_missing_upper_parent_directories(relative); !parents) return parents;
  RecoveryOperation operation;
  operation.workspace = workspace_;
  operation.kind = "truncate";
  operation.path = relative;
  return publish_staged_operation(operation, [&](const std::filesystem::path& stage) {
    if (auto copied = materialize_visible_entry_at(relative, stage); !copied) return copied;
    if (::truncate(stage.c_str(), static_cast<off_t>(size)) != 0) {
      return Status(fail(last_errno()));
    }
    return Status{};
  });
}

Status WorkspaceEngine::utimens(std::string_view path, std::int64_t atime, std::int64_t mtime) {
  const std::string relative = normalize_relative(path);
  std::unique_lock lock(mutex_);
  struct stat st {};
  if (find_visible_entry_layer(relative, st) == Layer::Missing) return fail(ENOENT);
  if (auto parents = create_missing_upper_parent_directories(relative); !parents) return parents;
  RecoveryOperation operation;
  operation.workspace = workspace_;
  operation.kind = "utimens";
  operation.path = relative;
  return publish_staged_operation(operation, [&](const std::filesystem::path& stage) {
    if (auto copied = materialize_visible_entry_at(relative, stage); !copied) return copied;
    const timeval times[2] = {{static_cast<time_t>(atime), 0},
                              {static_cast<time_t>(mtime), 0}};
    if (::utimes(stage.c_str(), times) != 0) return Status(fail(last_errno()));
    return Status{};
  });
}

std::uint64_t WorkspaceEngine::upper_bytes() const {
  std::shared_lock lock(mutex_);
  std::uint64_t total = 0;
  std::error_code ec;
  for (auto it = std::filesystem::recursive_directory_iterator(
           upper_dir_, std::filesystem::directory_options::skip_permission_denied, ec);
       it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) break;
    struct stat st{};
    if (lstat_path(it->path(), st)) {
      total += static_cast<std::uint64_t>(st.st_blocks) * 512;
    }
  }
  return total;
}

}  // namespace tribios
