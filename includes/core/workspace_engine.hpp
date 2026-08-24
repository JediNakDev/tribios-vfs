#pragma once

#include <sys/stat.h>

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

#include "core/error.hpp"
#include "core/metadata_store.hpp"

namespace tribios {

struct Attr {
  mode_t mode = 0;
  std::uint64_t size = 0;
  std::uint64_t nlink = 1;
  std::int64_t mtime = 0;
  std::int64_t atime = 0;
  std::int64_t ctime = 0;
  bool from_upper = false;
};

struct DirEntry {
  std::string name;
  mode_t mode = 0;
};

// All filesystem behavior of one Workspace: an immutable Base tree plus a
// sparse upper tree of this Workspace's own files, with tombstones marking
// Base-state paths it removed. The FUSE callbacks are a thin adapter over
// this class.
class WorkspaceEngine {
 public:
  WorkspaceEngine(std::string workspace, std::filesystem::path base_dir,
                  std::filesystem::path upper_dir, MetadataStore& store);

  Result<Attr> getattr(std::string_view path);
  Result<std::vector<DirEntry>> readdir(std::string_view path);
  Result<std::string> readlink(std::string_view path);
  Result<std::string> read_file(std::string_view path, std::uint64_t size, std::uint64_t offset);
  Result<std::size_t> write_file(std::string_view path, std::string_view data,
                                 std::uint64_t offset);

  Result<int> open_handle(std::string_view path, int flags);
  Result<std::size_t> read_handle(int fd, char* buffer, std::size_t size, std::uint64_t offset);
  Result<std::size_t> write_handle(int fd, const char* buffer, std::size_t size,
                                   std::uint64_t offset);
  void close_handle(int fd);

  Status create(std::string_view path, mode_t mode);
  Status mkdir(std::string_view path, mode_t mode);
  Status unlink(std::string_view path);
  Status rmdir(std::string_view path);
  Status rename(std::string_view from, std::string_view to);
  Status symlink(std::string_view target, std::string_view link_path);
  Status chmod(std::string_view path, mode_t mode);
  Status truncate(std::string_view path, std::uint64_t size);
  Status utimens(std::string_view path, std::int64_t atime, std::int64_t mtime);

  std::uint64_t upper_bytes() const;

 private:
  enum class Layer { Missing, Upper, Base };

  // Every helper below assumes the caller holds mutex_.
  std::filesystem::path upper_path(std::string_view relative) const;
  std::filesystem::path base_path(std::string_view relative) const;
  bool base_is_visible(const std::string& relative) const;
  Layer find_visible_entry_layer(const std::string& relative, struct stat& out) const;
  void merge_visible_directory_entries(const std::string& relative,
                                       std::map<std::string, DirEntry>& entries) const;
  Status create_missing_upper_parent_directories(const std::string& relative);
  Status copy_visible_entry_to_upper(const std::string& relative);
  Status move_visible_entry_to_upper(const std::string& from, const std::string& to);
  Status add_tombstone(const std::string& relative);
  Status drop_tombstones_under(const std::string& relative);

  std::string workspace_;
  std::filesystem::path base_dir_;
  std::filesystem::path upper_dir_;
  MetadataStore& store_;
  mutable std::shared_mutex mutex_;
  std::set<std::string> tombstones_;
};

}  // namespace tribios
