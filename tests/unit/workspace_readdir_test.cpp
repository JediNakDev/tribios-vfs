#include <sys/stat.h>

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "core/metadata_store.hpp"
#include "core/workspace_engine.hpp"
#include "temporary_directory.hpp"

namespace {

struct WorkspaceFixture {
  TemporaryDirectory root{"tribios-readdir"};
  std::filesystem::path base_dir = root.path() / "base";
  std::filesystem::path upper_dir = root.path() / "upper/feature-x";
  std::unique_ptr<tribios::MetadataStore> store;

  WorkspaceFixture() {
    std::filesystem::create_directories(base_dir);
    std::filesystem::create_directories(upper_dir);
    auto opened = tribios::MetadataStore::open_database(root.path() / "meta.db");
    REQUIRE(opened.has_value());
    store = std::move(*opened);
  }

  tribios::WorkspaceEngine open_engine() {
    return tribios::WorkspaceEngine("feature-x", base_dir, upper_dir, *store);
  }
};

const tribios::DirEntry& entry_named(const std::vector<tribios::DirEntry>& entries,
                                     const std::string& name) {
  for (const auto& entry : entries) {
    if (entry.name == name) return entry;
  }
  FAIL("missing directory entry " << name);
  return entries.front();
}

std::vector<std::string> entry_names(const std::vector<tribios::DirEntry>& entries) {
  std::vector<std::string> names;
  for (const auto& entry : entries) names.push_back(entry.name);
  return names;
}

}  // namespace

TEST_CASE("readdir preserves merged entry types and stable Workspace inode numbers") {
  WorkspaceFixture fixture;
  write_file_creating_parents(fixture.base_dir / "regular.txt", "base file");
  std::filesystem::create_directory(fixture.base_dir / "directory");
  std::filesystem::create_symlink("directory", fixture.base_dir / "link-to-directory");
  std::filesystem::create_directory(fixture.base_dir / "replaced");
  write_file_creating_parents(fixture.base_dir / "tombstoned.txt", "hidden");
  write_file_creating_parents(fixture.upper_dir / "replaced", "upper file");
  REQUIRE(fixture.store->add_tombstone("feature-x", "tombstoned.txt").has_value());

  auto engine = fixture.open_engine();
  auto first_read = engine.readdir("");
  REQUIRE(first_read.has_value());
  CHECK(entry_names(*first_read) ==
        std::vector<std::string>{"directory", "link-to-directory", "regular.txt", "replaced"});

  CHECK(S_ISDIR(entry_named(*first_read, "directory").mode));
  CHECK(S_ISLNK(entry_named(*first_read, "link-to-directory").mode));
  CHECK(S_ISREG(entry_named(*first_read, "regular.txt").mode));
  CHECK(S_ISREG(entry_named(*first_read, "replaced").mode));

  for (const auto& entry : *first_read) {
    CHECK(entry.ino != 0);
    auto attributes = engine.getattr(entry.name);
    REQUIRE(attributes.has_value());
    CHECK(entry.ino == attributes->ino);
    CHECK((entry.mode & S_IFMT) == (attributes->mode & S_IFMT));
  }

  auto second_read = engine.readdir("");
  REQUIRE(second_read.has_value());
  REQUIRE(second_read->size() == first_read->size());
  for (std::size_t index = 0; index < first_read->size(); ++index) {
    CHECK((*second_read)[index].name == (*first_read)[index].name);
    CHECK((*second_read)[index].ino == (*first_read)[index].ino);
    CHECK((*second_read)[index].mode == (*first_read)[index].mode);
  }

  auto reopened = fixture.open_engine().readdir("");
  REQUIRE(reopened.has_value());
  REQUIRE(reopened->size() == first_read->size());
  for (std::size_t index = 0; index < first_read->size(); ++index) {
    CHECK((*reopened)[index].ino == (*first_read)[index].ino);
  }
}

TEST_CASE("readdir reflects every Workspace membership mutation after a cached read") {
  WorkspaceFixture fixture;
  write_file_creating_parents(fixture.base_dir / "old.txt", "base file");
  std::filesystem::create_directory(fixture.base_dir / "empty-directory");

  auto engine = fixture.open_engine();
  auto entries = engine.readdir("");
  REQUIRE(entries.has_value());
  CHECK(entry_names(*entries) == std::vector<std::string>{"empty-directory", "old.txt"});

  REQUIRE(engine.create("created.txt", 0644).has_value());
  entries = engine.readdir("");
  REQUIRE(entries.has_value());
  CHECK(entry_names(*entries) ==
        std::vector<std::string>{"created.txt", "empty-directory", "old.txt"});

  REQUIRE(engine.mkdir("new-directory", 0755).has_value());
  entries = engine.readdir("");
  REQUIRE(entries.has_value());
  CHECK(entry_names(*entries) ==
        std::vector<std::string>{"created.txt", "empty-directory", "new-directory", "old.txt"});

  REQUIRE(engine.symlink("created.txt", "created.link").has_value());
  entries = engine.readdir("");
  REQUIRE(entries.has_value());
  CHECK(entry_names(*entries) == std::vector<std::string>{"created.link", "created.txt",
                                                          "empty-directory", "new-directory",
                                                          "old.txt"});

  REQUIRE(engine.rename("created.txt", "renamed.txt").has_value());
  entries = engine.readdir("");
  REQUIRE(entries.has_value());
  CHECK(entry_names(*entries) == std::vector<std::string>{"created.link", "empty-directory",
                                                          "new-directory", "old.txt",
                                                          "renamed.txt"});

  REQUIRE(engine.unlink("old.txt").has_value());
  entries = engine.readdir("");
  REQUIRE(entries.has_value());
  CHECK(entry_names(*entries) == std::vector<std::string>{"created.link", "empty-directory",
                                                          "new-directory", "renamed.txt"});

  REQUIRE(engine.rmdir("empty-directory").has_value());
  entries = engine.readdir("");
  REQUIRE(entries.has_value());
  CHECK(entry_names(*entries) ==
        std::vector<std::string>{"created.link", "new-directory", "renamed.txt"});
}

// Copy-up moves an entry to a new backing file, and the first write beneath a
// Base-only directory materializes that directory in the upper tree. Both
// change the inode a cached parent listing reported.
TEST_CASE("readdir reports the copied-up inode after a cached read") {
  WorkspaceFixture fixture;
  write_file_creating_parents(fixture.base_dir / "edited.txt", "base file");
  write_file_creating_parents(fixture.base_dir / "directory/nested.txt", "nested file");

  auto engine = fixture.open_engine();
  REQUIRE(engine.readdir("").has_value());
  REQUIRE(engine.readdir("directory").has_value());

  REQUIRE(engine.write_file("edited.txt", "upper", 0).has_value());
  REQUIRE(engine.write_file("directory/nested.txt", "upper", 0).has_value());

  const auto require_listing_matches_getattr = [&](const std::string& directory) {
    auto entries = engine.readdir(directory);
    REQUIRE(entries.has_value());
    for (const auto& entry : *entries) {
      const std::string relative = directory.empty() ? entry.name : directory + "/" + entry.name;
      auto attributes = engine.getattr(relative);
      REQUIRE(attributes.has_value());
      CHECK(entry.ino == attributes->ino);
      CHECK(attributes->from_upper);
    }
  };
  require_listing_matches_getattr("");
  require_listing_matches_getattr("directory");
}
