#include "core/git_index.hpp"

#include <limits>
#include <optional>

#include "core/sha1.hpp"

namespace tribios {
namespace {

// gitformat-index(5): every entry starts with ten 32-bit big-endian fields,
// then the object id, then 16 bits of flags, then the path.
constexpr std::size_t kHeaderSize = 12;
constexpr std::size_t kChecksumSize = 20;
constexpr std::size_t kModeOffset = 24;
constexpr std::size_t kFlagsOffset = 60;
constexpr std::size_t kPathOffset = 62;
constexpr std::uint16_t kExtendedFlag = 0x4000;

std::uint32_t read_be32(const std::string& bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (std::size_t i = 0; i < 4; ++i) {
    value = (value << 8) | static_cast<unsigned char>(bytes[offset + i]);
  }
  return value;
}

void write_be32(std::string& bytes, std::size_t offset, std::uint32_t value) {
  for (std::size_t i = 0; i < 4; ++i) {
    bytes[offset + i] = static_cast<char>((value >> (24 - 8 * i)) & 0xff);
  }
}

// Version 4 prefixes each path with the number of bytes to drop from the end of
// the previous path, as Git's offset varint.
std::optional<std::uint64_t> read_path_strip_length(const std::string& bytes, std::size_t& cursor,
                                                    std::size_t end) {
  if (cursor >= end) return std::nullopt;
  unsigned char byte = static_cast<unsigned char>(bytes[cursor++]);
  std::uint64_t value = byte & 0x7f;
  while ((byte & 0x80) != 0) {
    if (cursor >= end || value >= (std::numeric_limits<std::uint64_t>::max() >> 7)) {
      return std::nullopt;
    }
    byte = static_cast<unsigned char>(bytes[cursor++]);
    value = ((value + 1) << 7) | (byte & 0x7f);
  }
  return value;
}

}  // namespace

Outcome<GitIndexFile> parse_git_index(std::string bytes) {
  if (bytes.size() < kHeaderSize + kChecksumSize || bytes.compare(0, 4, "DIRC") != 0) {
    return error("not a Git index file");
  }
  const std::uint32_t version = read_be32(bytes, 4);
  if (version < 2 || version > 4) {
    return error("unsupported Git index version " + std::to_string(version));
  }
  const std::uint32_t entry_count = read_be32(bytes, 8);
  const std::size_t end = bytes.size() - kChecksumSize;

  GitIndexFile index;
  index.entries.reserve(entry_count);
  std::size_t offset = kHeaderSize;
  std::string previous_path;
  for (std::uint32_t i = 0; i < entry_count; ++i) {
    if (offset + kPathOffset > end) return error("truncated Git index entry");
    const std::uint32_t flags = (static_cast<unsigned char>(bytes[offset + kFlagsOffset]) << 8) |
                                static_cast<unsigned char>(bytes[offset + kFlagsOffset + 1]);
    std::size_t path_start = offset + kPathOffset;
    if ((flags & kExtendedFlag) != 0) {
      if (version < 3) return error("extended Git index flags in a version 2 index");
      path_start += 2;
    }

    std::string path;
    std::size_t next_entry = 0;
    if (version == 4) {
      std::size_t cursor = path_start;
      const auto strip = read_path_strip_length(bytes, cursor, end);
      if (!strip || *strip > previous_path.size()) return error("corrupt Git index path prefix");
      const std::size_t terminator = bytes.find('\0', cursor);
      if (terminator == std::string::npos || terminator >= end) {
        return error("unterminated Git index path");
      }
      path = previous_path.substr(0, previous_path.size() - *strip) +
             bytes.substr(cursor, terminator - cursor);
      next_entry = terminator + 1;
    } else {
      const std::size_t terminator = bytes.find('\0', path_start);
      if (terminator == std::string::npos || terminator >= end) {
        return error("unterminated Git index path");
      }
      path = bytes.substr(path_start, terminator - path_start);
      // Entries are NUL-padded to a multiple of eight bytes, with at least one NUL.
      next_entry = offset + ((path_start - offset + path.size() + 8) & ~std::size_t{7});
      if (next_entry > end) return error("truncated Git index entry");
    }
    index.entries.push_back(GitIndexEntry{path, read_be32(bytes, offset + kModeOffset), offset});
    previous_path = std::move(path);
    offset = next_entry;
  }

  while (offset + 8 <= end) {
    const std::string signature = bytes.substr(offset, 4);
    const std::uint32_t size = read_be32(bytes, offset + 4);
    if (signature == "link") return error("split Git indexes are not supported");
    if (size > end - offset - 8) return error("truncated Git index extension " + signature);
    offset += 8 + size;
  }
  if (offset != end) return error("trailing bytes after the Git index extensions");

  index.bytes = std::move(bytes);
  return index;
}

GitIndexStat read_git_index_stat(const GitIndexFile& index, const GitIndexEntry& entry) {
  const std::string& bytes = index.bytes;
  const std::size_t at = entry.offset;
  return GitIndexStat{read_be32(bytes, at),      read_be32(bytes, at + 4),
                      read_be32(bytes, at + 8),  read_be32(bytes, at + 12),
                      read_be32(bytes, at + 16), read_be32(bytes, at + 20),
                      read_be32(bytes, at + 28), read_be32(bytes, at + 32),
                      read_be32(bytes, at + 36)};
}

void write_git_index_stat(GitIndexFile& index, const GitIndexEntry& entry,
                          const GitIndexStat& stat) {
  std::string& bytes = index.bytes;
  const std::size_t at = entry.offset;
  write_be32(bytes, at, stat.ctime_seconds);
  write_be32(bytes, at + 4, stat.ctime_nanoseconds);
  write_be32(bytes, at + 8, stat.mtime_seconds);
  write_be32(bytes, at + 12, stat.mtime_nanoseconds);
  write_be32(bytes, at + 16, stat.device);
  write_be32(bytes, at + 20, stat.inode);
  write_be32(bytes, at + 28, stat.uid);
  write_be32(bytes, at + 32, stat.gid);
  write_be32(bytes, at + 36, stat.size);
}

void seal_git_index(GitIndexFile& index) {
  const std::size_t content_size = index.bytes.size() - kChecksumSize;
  const auto digest = sha1_digest(std::string_view(index.bytes).substr(0, content_size));
  index.bytes.replace(content_size, kChecksumSize,
                      std::string(reinterpret_cast<const char*>(digest.data()), digest.size()));
}

}  // namespace tribios
