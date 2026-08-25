#include "core/workspace_engine.hpp"

#include <fcntl.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <system_error>

#include "core/paths.hpp"

namespace tribios {
namespace {

bool lstat_path(const std::filesystem::path& path, struct stat& out) {
  return ::lstat(path.c_str(), &out) == 0;
}

Attr attr_from_stat(const struct stat& st, bool from_upper) {
  return Attr{st.st_mode,  static_cast<std::uint64_t>(st.st_size),
              st.st_nlink, st.st_mtime,
              st.st_atime, st.st_ctime,
              from_upper};
}

int last_errno() { return errno != 0 ? errno : EIO; }

}  // namespace

WorkspaceEngine::WorkspaceEngine(std::string workspace, std::filesystem::path base_dir,
                                 std::filesystem::path upper_dir, MetadataStore& store)
    : workspace_(std::move(workspace)),
      base_dir_(std::move(base_dir)),
      upper_dir_(std::move(upper_dir)),
      store_(store) {
  // Tombstones are persistent: a removed Base-state path stays absent across
  // daemon restarts.
  for (auto& path : store_.load_workspace_tombstones(workspace_)) {
    tombstones_.insert(std::move(path));
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
      entries[name] = DirEntry{name, st.st_mode};
    }
  }
  ec.clear();
  for (const auto& entry : std::filesystem::directory_iterator(
           upper_path(relative), std::filesystem::directory_options::none, ec)) {
    const std::string name = entry.path().filename().string();
    struct stat st{};
    if (lstat_path(entry.path(), st)) entries[name] = DirEntry{name, st.st_mode};
  }
}

Result<Attr> WorkspaceEngine::getattr(std::string_view path) {
  const std::string relative = normalize_relative(path);
  std::shared_lock lock(mutex_);
  struct stat st{};
  const Layer layer = find_visible_entry_layer(relative, st);
  if (layer == Layer::Missing) return fail(ENOENT);
  return attr_from_stat(st, layer == Layer::Upper);
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
  }
  return {};
}

// The first mutation of a shared entry gives this Workspace a private
// whole-file copy. Directories copy up alone: their children keep resolving
// through the Base tree until they are mutated themselves.
Status WorkspaceEngine::copy_visible_entry_to_upper(const std::string& relative) {
  struct stat st{};
  if (lstat_path(upper_path(relative), st)) return {};
  if (!base_is_visible(relative) || !lstat_path(base_path(relative), st)) return fail(ENOENT);
  if (auto parents = create_missing_upper_parent_directories(relative); !parents) return parents;

  const std::filesystem::path from = base_path(relative);
  const std::filesystem::path to = upper_path(relative);
  std::error_code ec;
  if (S_ISDIR(st.st_mode)) {
    if (::mkdir(to.c_str(), st.st_mode & 07777) != 0 && errno != EEXIST) return fail(last_errno());
    return {};
  }
  if (S_ISLNK(st.st_mode)) {
    std::filesystem::create_symlink(std::filesystem::read_symlink(from, ec), to, ec);
    return ec ? Status(fail(EIO)) : Status{};
  }
  std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) return fail(EIO);
  ::chmod(to.c_str(), st.st_mode & 07777);
  return {};
}

// Renaming a Base-state directory materializes that subtree into this
// Workspace. The prototype accepts the cost rather than hiding it.
Status WorkspaceEngine::move_visible_entry_to_upper(const std::string& from,
                                                    const std::string& to) {
  struct stat st{};
  if (find_visible_entry_layer(from, st) == Layer::Missing) return fail(ENOENT);

  if (!S_ISDIR(st.st_mode)) {
    if (auto copied = copy_visible_entry_to_upper(from); !copied) return copied;
    if (auto parents = create_missing_upper_parent_directories(to); !parents) return parents;
    std::error_code ec;
    std::filesystem::rename(upper_path(from), upper_path(to), ec);
    return ec ? Status(fail(EIO)) : Status{};
  }

  if (auto parents = create_missing_upper_parent_directories(to); !parents) return parents;
  if (::mkdir(upper_path(to).c_str(), st.st_mode & 07777) != 0 && errno != EEXIST) {
    return fail(last_errno());
  }
  std::map<std::string, DirEntry> children;
  merge_visible_directory_entries(from, children);
  for (const auto& [name, entry] : children) {
    auto moved = move_visible_entry_to_upper(join_relative(from, name), join_relative(to, name));
    if (!moved) return moved;
  }
  return {};
}

Status WorkspaceEngine::add_tombstone(const std::string& relative) {
  struct stat st{};
  if (!base_is_visible(relative) || !lstat_path(base_path(relative), st)) return {};
  if (!store_.add_tombstone(workspace_, relative)) return fail(EIO);
  tombstones_.insert(relative);
  return {};
}

Status WorkspaceEngine::drop_tombstones_under(const std::string& relative) {
  if (!store_.remove_tombstones_under(workspace_, relative)) return fail(EIO);
  std::erase_if(tombstones_, [&](const std::string& path) {
    return path == relative || path.starts_with(relative + "/");
  });
  return {};
}

Result<int> WorkspaceEngine::open_handle(std::string_view path, int flags) {
  const std::string relative = normalize_relative(path);
  const bool for_writing = (flags & (O_WRONLY | O_RDWR | O_APPEND | O_TRUNC | O_CREAT)) != 0;
  std::unique_lock lock(mutex_);
  struct stat st{};
  Layer layer = find_visible_entry_layer(relative, st);
  if (layer == Layer::Missing) {
    if ((flags & O_CREAT) == 0) return fail(ENOENT);
    if (auto parents = create_missing_upper_parent_directories(relative); !parents) {
      return std::unexpected(parents.error());
    }
    layer = Layer::Upper;
  } else if (S_ISDIR(st.st_mode)) {
    return fail(EISDIR);
  } else if (for_writing && layer == Layer::Base) {
    if (auto copied = copy_visible_entry_to_upper(relative); !copied) {
      return std::unexpected(copied.error());
    }
    layer = Layer::Upper;
  }
  const int fd = ::open((layer == Layer::Base ? base_path(relative) : upper_path(relative)).c_str(),
                        flags, 0644);
  if (fd < 0) return fail(last_errno());
  return fd;
}

Result<std::size_t> WorkspaceEngine::read_handle(int fd, char* buffer, std::size_t size,
                                                 std::uint64_t offset) {
  const ssize_t n = ::pread(fd, buffer, size, static_cast<off_t>(offset));
  if (n < 0) return fail(last_errno());
  return static_cast<std::size_t>(n);
}

Result<std::size_t> WorkspaceEngine::write_handle(int fd, const char* buffer, std::size_t size,
                                                  std::uint64_t offset) {
  const ssize_t n = ::pwrite(fd, buffer, size, static_cast<off_t>(offset));
  if (n < 0) return fail(last_errno());
  return static_cast<std::size_t>(n);
}

void WorkspaceEngine::close_handle(int fd) {
  if (fd >= 0) ::close(fd);
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
  const int fd = ::open(upper_path(relative).c_str(), O_CREAT | O_EXCL | O_WRONLY, mode & 07777);
  if (fd < 0) return fail(last_errno());
  ::close(fd);
  return {};
}

Status WorkspaceEngine::mkdir(std::string_view path, mode_t mode) {
  const std::string relative = normalize_relative(path);
  if (relative.empty()) return fail(EEXIST);
  std::unique_lock lock(mutex_);
  struct stat st{};
  if (find_visible_entry_layer(relative, st) != Layer::Missing) return fail(EEXIST);
  if (auto parents = create_missing_upper_parent_directories(relative); !parents) return parents;
  // Any tombstone on this path stays: the new directory starts empty.
  if (::mkdir(upper_path(relative).c_str(), mode & 07777) != 0) return fail(last_errno());
  return {};
}

Status WorkspaceEngine::unlink(std::string_view path) {
  const std::string relative = normalize_relative(path);
  if (relative.empty()) return fail(EISDIR);
  std::unique_lock lock(mutex_);
  struct stat st{};
  if (find_visible_entry_layer(relative, st) == Layer::Missing) return fail(ENOENT);
  if (S_ISDIR(st.st_mode)) return fail(EISDIR);
  if (lstat_path(upper_path(relative), st) && ::unlink(upper_path(relative).c_str()) != 0) {
    return fail(last_errno());
  }
  return add_tombstone(relative);
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
  if (lstat_path(upper_path(relative), st) && ::rmdir(upper_path(relative).c_str()) != 0) {
    return fail(last_errno());
  }
  return add_tombstone(relative);
}

Status WorkspaceEngine::rename(std::string_view from_path, std::string_view to_path) {
  const std::string from = normalize_relative(from_path);
  const std::string to = normalize_relative(to_path);
  if (from.empty() || to.empty()) return fail(EBUSY);
  if (from == to) return {};
  if (to.starts_with(from + "/")) return fail(EINVAL);

  std::unique_lock lock(mutex_);
  struct stat st{};
  if (find_visible_entry_layer(from, st) == Layer::Missing) return fail(ENOENT);
  if (find_visible_entry_layer(to, st) != Layer::Missing) {
    if (S_ISDIR(st.st_mode)) return fail(EISDIR);
    if (lstat_path(upper_path(to), st)) ::unlink(upper_path(to).c_str());
    if (auto hidden = add_tombstone(to); !hidden) return hidden;
  }

  if (auto moved = move_visible_entry_to_upper(from, to); !moved) return moved;
  // The destination now holds the moved content, so nothing there stays hidden.
  if (auto dropped = drop_tombstones_under(to); !dropped) return dropped;

  std::error_code ec;
  std::filesystem::remove_all(upper_path(from), ec);
  return add_tombstone(from);
}

Status WorkspaceEngine::symlink(std::string_view target, std::string_view link_path) {
  const std::string relative = normalize_relative(link_path);
  if (relative.empty()) return fail(EEXIST);
  std::unique_lock lock(mutex_);
  struct stat st{};
  if (find_visible_entry_layer(relative, st) != Layer::Missing) return fail(EEXIST);
  if (auto parents = create_missing_upper_parent_directories(relative); !parents) return parents;
  if (::symlink(std::string(target).c_str(), upper_path(relative).c_str()) != 0) {
    return fail(last_errno());
  }
  return {};
}

Status WorkspaceEngine::chmod(std::string_view path, mode_t mode) {
  const std::string relative = normalize_relative(path);
  std::unique_lock lock(mutex_);
  if (auto copied = copy_visible_entry_to_upper(relative); !copied) return copied;
  if (::chmod(upper_path(relative).c_str(), mode & 07777) != 0) return fail(last_errno());
  return {};
}

Status WorkspaceEngine::truncate(std::string_view path, std::uint64_t size) {
  const std::string relative = normalize_relative(path);
  std::unique_lock lock(mutex_);
  struct stat st{};
  const Layer layer = find_visible_entry_layer(relative, st);
  if (layer == Layer::Missing) return fail(ENOENT);
  if (S_ISDIR(st.st_mode)) return fail(EISDIR);
  if (auto copied = copy_visible_entry_to_upper(relative); !copied) return copied;
  if (::truncate(upper_path(relative).c_str(), static_cast<off_t>(size)) != 0) {
    return fail(last_errno());
  }
  return {};
}

Status WorkspaceEngine::utimens(std::string_view path, std::int64_t atime, std::int64_t mtime) {
  const std::string relative = normalize_relative(path);
  std::unique_lock lock(mutex_);
  if (auto copied = copy_visible_entry_to_upper(relative); !copied) return copied;
  const timeval times[2] = {{static_cast<time_t>(atime), 0}, {static_cast<time_t>(mtime), 0}};
  if (::utimes(upper_path(relative).c_str(), times) != 0) return fail(last_errno());
  return {};
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
    if (lstat_path(it->path(), st)) total += static_cast<std::uint64_t>(st.st_size);
  }
  return total;
}

}  // namespace tribios
