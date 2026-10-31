#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/error.hpp"

namespace tribios {

// The stat data Git caches per index entry so that it can trust a file without
// reading it. Git truncates every field to 32 bits.
struct GitIndexStat {
  std::uint32_t ctime_seconds = 0;
  std::uint32_t ctime_nanoseconds = 0;
  std::uint32_t mtime_seconds = 0;
  std::uint32_t mtime_nanoseconds = 0;
  std::uint32_t device = 0;
  std::uint32_t inode = 0;
  std::uint32_t uid = 0;
  std::uint32_t gid = 0;
  std::uint32_t size = 0;

  bool operator==(const GitIndexStat&) const = default;
};

struct GitIndexEntry {
  std::string path;
  std::uint32_t mode = 0;
  std::size_t offset = 0;  // where the entry starts in the index file
};

// A SHA-1 Git index file, format version 2, 3 or 4, whose entries can have
// their stat data rewritten in place. Paths, object ids, flags and extensions
// are never touched, so every offset recorded in the file stays valid.
struct GitIndexFile {
  std::string bytes;
  std::vector<GitIndexEntry> entries;
};

// Rejects split indexes: their entries live partly in a shared index file.
Outcome<GitIndexFile> parse_git_index(std::string bytes);
GitIndexStat read_git_index_stat(const GitIndexFile& index, const GitIndexEntry& entry);
void write_git_index_stat(GitIndexFile& index, const GitIndexEntry& entry,
                          const GitIndexStat& stat);
// Recomputes the trailing checksum, which Git verifies in `git fsck`.
void seal_git_index(GitIndexFile& index);

}  // namespace tribios
