// Covers core/git_index.hpp and core/sha1.hpp: reading a Git index file,
// rewriting entry stat data in place, and resealing its checksum. The index
// files are built byte by byte from gitformat-index(5), so no Git is involved.

#include "core/git_index.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

#include "core/sha1.hpp"

using tribios::GitIndexStat;
using tribios::parse_git_index;
using tribios::read_git_index_stat;
using tribios::seal_git_index;
using tribios::sha1_digest;
using tribios::write_git_index_stat;

namespace {

std::string sha1_hex(std::string_view data) {
  static const char* digits = "0123456789abcdef";
  std::string hex;
  for (const std::uint8_t byte : sha1_digest(data)) {
    hex.push_back(digits[byte >> 4]);
    hex.push_back(digits[byte & 0xf]);
  }
  return hex;
}

std::string be32(std::uint32_t value) {
  return {static_cast<char>(value >> 24), static_cast<char>(value >> 16),
          static_cast<char>(value >> 8), static_cast<char>(value)};
}

std::string be16(std::uint16_t value) {
  return {static_cast<char>(value >> 8), static_cast<char>(value)};
}

struct EntrySpec {
  std::string path;
  std::uint32_t mode = 0100644;
  // Stat field i holds stat_seed + i, so every field is distinguishable.
  std::uint32_t stat_seed = 0;
  bool extended = false;
};

GitIndexStat expected_stat(std::uint32_t seed) {
  return GitIndexStat{seed, seed + 1, seed + 2, seed + 3, seed + 4,
                      seed + 5, seed + 7, seed + 8, seed + 9};
}

std::string build_index(std::uint32_t version, const std::vector<EntrySpec>& entries,
                        const std::string& extensions = {}) {
  std::string out = "DIRC" + be32(version) + be32(static_cast<std::uint32_t>(entries.size()));
  std::string previous_path;
  for (const EntrySpec& spec : entries) {
    std::string entry;
    for (std::uint32_t field = 0; field < 10; ++field) {
      entry += be32(field == 6 ? spec.mode : spec.stat_seed + field);
    }
    entry += std::string(20, '\x5a');
    entry += be16(static_cast<std::uint16_t>(spec.path.size() | (spec.extended ? 0x4000 : 0)));
    if (spec.extended) entry += be16(0x2000);
    if (version == 4) {
      std::size_t common = 0;
      while (common < previous_path.size() && common < spec.path.size() &&
             previous_path[common] == spec.path[common]) {
        ++common;
      }
      entry.push_back(static_cast<char>(previous_path.size() - common));  // under 128: one byte
      entry += spec.path.substr(common);
      entry.push_back('\0');
    } else {
      entry += spec.path;
      const std::size_t padded = (entry.size() + 8) & ~std::size_t{7};
      entry += std::string(padded - entry.size(), '\0');
    }
    out += entry;
    previous_path = spec.path;
  }
  out += extensions;
  const auto digest = sha1_digest(out);
  out.append(reinterpret_cast<const char*>(digest.data()), digest.size());
  return out;
}

std::string tree_extension() { return "TREE" + be32(6) + std::string("abcdef"); }

}  // namespace

TEST_CASE("sha1_digest matches the FIPS 180 test vectors", "[git_index]") {
  CHECK(sha1_hex("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  CHECK(sha1_hex("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
  // 56 bytes: the length no longer fits in the first padded block.
  CHECK(sha1_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
  CHECK(sha1_hex(std::string(1000000, 'a')) == "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

TEST_CASE("a version 2 index exposes each entry's path, mode and stat data", "[git_index]") {
  const auto index = parse_git_index(build_index(
      2, {{"README.md", 0100644, 100}, {"bin/run", 0100755, 200}, {"link", 0120000, 300}},
      tree_extension()));
  REQUIRE(index);
  REQUIRE(index->entries.size() == 3);
  CHECK(index->entries[0].path == "README.md");
  CHECK(index->entries[1].path == "bin/run");
  CHECK(index->entries[1].mode == 0100755);
  CHECK(index->entries[2].mode == 0120000);
  CHECK(read_git_index_stat(*index, index->entries[0]) == expected_stat(100));
  CHECK(read_git_index_stat(*index, index->entries[2]) == expected_stat(300));
}

TEST_CASE("rewriting stat data changes nothing else and reseals the file", "[git_index]") {
  const std::string original =
      build_index(2, {{"a.txt", 0100644, 10}, {"b.txt", 0100644, 20}}, tree_extension());
  auto index = parse_git_index(original);
  REQUIRE(index);

  const GitIndexStat replacement{1, 0, 2, 0, 3, 4, 5, 6, 7};
  write_git_index_stat(*index, index->entries[1], replacement);
  seal_git_index(*index);

  const std::string& rewritten = index->bytes;
  REQUIRE(rewritten.size() == original.size());
  const std::size_t content_size = rewritten.size() - 20;
  const auto digest = sha1_digest(std::string_view(rewritten).substr(0, content_size));
  CHECK(rewritten.substr(content_size) ==
        std::string(reinterpret_cast<const char*>(digest.data()), digest.size()));

  // Only the second entry's stat fields moved; its mode, the first entry and
  // the extension are byte-for-byte what they were.
  const auto reparsed = parse_git_index(rewritten);
  REQUIRE(reparsed);
  CHECK(read_git_index_stat(*reparsed, reparsed->entries[0]) == expected_stat(10));
  CHECK(read_git_index_stat(*reparsed, reparsed->entries[1]) == replacement);
  CHECK(reparsed->entries[1].mode == 0100644);
  const std::size_t extension_start = content_size - tree_extension().size();
  CHECK(rewritten.substr(extension_start, tree_extension().size()) == tree_extension());
}

TEST_CASE("a version 3 entry with extended flags has its path two bytes later", "[git_index]") {
  const auto index = parse_git_index(build_index(
      3, {{"intent.txt", 0100644, 40, true}, {"plain.txt", 0100644, 50}}));
  REQUIRE(index);
  REQUIRE(index->entries.size() == 2);
  CHECK(index->entries[0].path == "intent.txt");
  CHECK(index->entries[1].path == "plain.txt");
  CHECK(read_git_index_stat(*index, index->entries[1]) == expected_stat(50));
}

TEST_CASE("a version 4 index rebuilds prefix-compressed paths", "[git_index]") {
  const auto index = parse_git_index(build_index(
      4, {{"src/core/a.cpp", 0100644, 1}, {"src/core/b.cpp", 0100644, 2}, {"src/x", 0100644, 3},
          {"tests/t", 0100644, 4}},
      tree_extension()));
  REQUIRE(index);
  REQUIRE(index->entries.size() == 4);
  CHECK(index->entries[1].path == "src/core/b.cpp");
  CHECK(index->entries[2].path == "src/x");
  CHECK(index->entries[3].path == "tests/t");
  CHECK(read_git_index_stat(*index, index->entries[3]) == expected_stat(4));
}

TEST_CASE("malformed and unsupported indexes are rejected", "[git_index]") {
  const std::string valid = build_index(2, {{"a.txt", 0100644, 1}});

  SECTION("wrong signature") {
    std::string bytes = valid;
    bytes[0] = 'X';
    CHECK_FALSE(parse_git_index(bytes));
  }
  SECTION("unknown version") { CHECK_FALSE(parse_git_index(build_index(5, {}))); }
  SECTION("truncated entry") {
    std::string bytes = valid.substr(0, 40) + valid.substr(valid.size() - 20);
    CHECK_FALSE(parse_git_index(bytes));
  }
  SECTION("extended flags in version 2") {
    CHECK_FALSE(parse_git_index(build_index(2, {{"a.txt", 0100644, 1, true}})));
  }
  SECTION("split index") {
    const std::string link = "link" + be32(20) + std::string(20, '\0');
    CHECK_FALSE(parse_git_index(build_index(2, {{"a.txt", 0100644, 1}}, link)));
  }
  SECTION("extension larger than the file") {
    const std::string oversized = "TREE" + be32(1000) + std::string("ab");
    CHECK_FALSE(parse_git_index(build_index(2, {{"a.txt", 0100644, 1}}, oversized)));
  }
}
