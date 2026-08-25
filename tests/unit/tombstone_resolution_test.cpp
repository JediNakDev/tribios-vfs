// Covers tombstone storage in core/metadata_store.hpp and the resolution rule
// that reads it in core/workspace_engine.hpp. The invariant documented in
// docs/prototype/README.md is that a tombstone keeps hiding the Base-state
// subtree beneath it even after the path is re-created, so a removed directory
// never resurrects its old children.
//
// Everything here runs against a temporary SQLite file and two ordinary
// directories. No daemon, no FUSE mount and no Git repository.

#include "core/metadata_store.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "core/error.hpp"
#include "core/workspace_engine.hpp"
#include "temporary_directory.hpp"

using tribios::MetadataStore;
using tribios::WorkspaceEngine;

namespace {

// One Project's storage: a Base tree, one Workspace upper tree and the metadata
// database that holds the tombstones.
struct WorkspaceFixture {
  TemporaryDirectory root{"tribios-tombstone"};
  std::filesystem::path base_dir = root.path() / "base";
  std::filesystem::path upper_dir = root.path() / "upper/feature-x";
  std::unique_ptr<MetadataStore> store;

  WorkspaceFixture() {
    std::filesystem::create_directories(base_dir);
    std::filesystem::create_directories(upper_dir);
    auto opened = MetadataStore::open_database(root.path() / "meta.db");
    REQUIRE(opened.has_value());
    store = std::move(*opened);
  }

  WorkspaceEngine open_workspace_engine() {
    return WorkspaceEngine("feature-x", base_dir, upper_dir, *store);
  }
};

std::vector<std::string> sorted_tombstones(MetadataStore& store, const std::string& workspace) {
  auto paths = store.load_workspace_tombstones(workspace);
  std::sort(paths.begin(), paths.end());
  return paths;
}

std::vector<std::string> entry_names(const std::vector<tribios::DirEntry>& entries) {
  std::vector<std::string> names;
  for (const auto& entry : entries) names.push_back(entry.name);
  return names;
}

}  // namespace

TEST_CASE("a tombstone on a directory hides the whole Base-state subtree beneath it") {
  WorkspaceFixture fixture;
  write_file_creating_parents(fixture.base_dir / "docs/readme.md", "readme");
  write_file_creating_parents(fixture.base_dir / "docs/deep/nested/note.txt", "note");
  write_file_creating_parents(fixture.base_dir / "docs-elsewhere.txt", "sibling");
  REQUIRE(fixture.store->add_tombstone("feature-x", "docs").has_value());

  auto engine = fixture.open_workspace_engine();
  CHECK(engine.getattr("docs").error() == ENOENT);
  CHECK(engine.getattr("docs/readme.md").error() == ENOENT);
  CHECK(engine.getattr("docs/deep").error() == ENOENT);
  CHECK(engine.getattr("docs/deep/nested/note.txt").error() == ENOENT);
  CHECK(engine.readdir("docs").error() == ENOENT);

  // A sibling whose name merely starts with the tombstoned path is untouched:
  // hiding follows path segments, not string prefixes.
  CHECK(engine.getattr("docs-elsewhere.txt").has_value());

  auto root_entries = fixture.open_workspace_engine().readdir("");
  REQUIRE(root_entries.has_value());
  CHECK(entry_names(*root_entries) == std::vector<std::string>{"docs-elsewhere.txt"});
}

TEST_CASE("re-creating a tombstoned directory does not resurrect its Base-state children") {
  WorkspaceFixture fixture;
  write_file_creating_parents(fixture.base_dir / "docs/readme.md", "readme");
  write_file_creating_parents(fixture.base_dir / "docs/deep/note.txt", "note");
  REQUIRE(fixture.store->add_tombstone("feature-x", "docs").has_value());

  auto engine = fixture.open_workspace_engine();
  REQUIRE(engine.mkdir("docs", 0755).has_value());

  auto attributes = engine.getattr("docs");
  REQUIRE(attributes.has_value());
  CHECK(attributes->from_upper);

  auto entries = engine.readdir("docs");
  REQUIRE(entries.has_value());
  CHECK(entries->empty());
  CHECK(engine.getattr("docs/readme.md").error() == ENOENT);
  CHECK(engine.getattr("docs/deep/note.txt").error() == ENOENT);

  // The children are hidden, not deleted: the Base state is immutable.
  CHECK(std::filesystem::is_regular_file(fixture.base_dir / "docs/readme.md"));

  // A brand new child of the re-created directory is visible and is the only
  // entry, so the old and new contents never mix.
  REQUIRE(engine.create("docs/fresh.md", 0644).has_value());
  auto refreshed = engine.readdir("docs");
  REQUIRE(refreshed.has_value());
  CHECK(entry_names(*refreshed) == std::vector<std::string>{"fresh.md"});
}

TEST_CASE("removing a Base-state file records a tombstone that survives reopening the Workspace") {
  WorkspaceFixture fixture;
  write_file_creating_parents(fixture.base_dir / "keep.txt", "keep");
  write_file_creating_parents(fixture.base_dir / "gone.txt", "gone");

  {
    auto engine = fixture.open_workspace_engine();
    REQUIRE(engine.unlink("gone.txt").has_value());
    CHECK(engine.getattr("gone.txt").error() == ENOENT);
  }
  CHECK(sorted_tombstones(*fixture.store, "feature-x") == std::vector<std::string>{"gone.txt"});

  // A second engine over the same storage loads the tombstones from the store,
  // which is what makes a removal survive a daemon restart.
  auto reopened = fixture.open_workspace_engine();
  CHECK(reopened.getattr("gone.txt").error() == ENOENT);
  CHECK(reopened.getattr("keep.txt").has_value());
}

TEST_CASE("emptying and removing a Base-state directory tombstones the directory itself") {
  WorkspaceFixture fixture;
  write_file_creating_parents(fixture.base_dir / "logs/first.log", "one");
  write_file_creating_parents(fixture.base_dir / "logs/second.log", "two");

  auto engine = fixture.open_workspace_engine();
  CHECK(engine.rmdir("logs").error() == ENOTEMPTY);
  REQUIRE(engine.unlink("logs/first.log").has_value());
  REQUIRE(engine.unlink("logs/second.log").has_value());
  REQUIRE(engine.rmdir("logs").has_value());

  CHECK(sorted_tombstones(*fixture.store, "feature-x") ==
        std::vector<std::string>{"logs", "logs/first.log", "logs/second.log"});
  CHECK(engine.getattr("logs").error() == ENOENT);

  REQUIRE(engine.mkdir("logs", 0755).has_value());
  auto entries = engine.readdir("logs");
  REQUIRE(entries.has_value());
  CHECK(entries->empty());
}

TEST_CASE("a rename onto a hidden path drops the tombstones under the destination") {
  WorkspaceFixture fixture;
  write_file_creating_parents(fixture.base_dir / "keep.txt", "keep");
  write_file_creating_parents(fixture.base_dir / "gone.txt", "gone");

  auto engine = fixture.open_workspace_engine();
  REQUIRE(engine.unlink("gone.txt").has_value());
  REQUIRE(engine.rename("keep.txt", "gone.txt").has_value());

  auto contents = engine.read_file("gone.txt", 64, 0);
  REQUIRE(contents.has_value());
  CHECK(*contents == "keep");
  CHECK(engine.getattr("keep.txt").error() == ENOENT);
  CHECK(sorted_tombstones(*fixture.store, "feature-x") == std::vector<std::string>{"keep.txt"});
}

TEST_CASE("tombstones belong to one Workspace and never hide a sibling Workspace's Base state") {
  WorkspaceFixture fixture;
  write_file_creating_parents(fixture.base_dir / "shared.txt", "shared");
  const auto sibling_upper_dir = fixture.root.path() / "upper/feature-y";
  std::filesystem::create_directories(sibling_upper_dir);

  auto engine = fixture.open_workspace_engine();
  REQUIRE(engine.unlink("shared.txt").has_value());

  WorkspaceEngine sibling("feature-y", fixture.base_dir, sibling_upper_dir, *fixture.store);
  CHECK(sibling.getattr("shared.txt").has_value());
  CHECK(sorted_tombstones(*fixture.store, "feature-y").empty());
}

TEST_CASE("add_tombstone is idempotent for the same Workspace and path") {
  WorkspaceFixture fixture;
  REQUIRE(fixture.store->add_tombstone("feature-x", "docs/readme.md").has_value());
  REQUIRE(fixture.store->add_tombstone("feature-x", "docs/readme.md").has_value());
  CHECK(sorted_tombstones(*fixture.store, "feature-x") ==
        std::vector<std::string>{"docs/readme.md"});
}

TEST_CASE("remove_tombstones_under removes a path and its descendants, not its name prefixes") {
  WorkspaceFixture fixture;
  for (const char* path : {"docs", "docs/readme.md", "docs/deep/note.txt", "docs-elsewhere.txt",
                           "docs2", "other"}) {
    REQUIRE(fixture.store->add_tombstone("feature-x", path).has_value());
  }
  REQUIRE(fixture.store->remove_tombstones_under("feature-x", "docs").has_value());

  CHECK(sorted_tombstones(*fixture.store, "feature-x") ==
        std::vector<std::string>{"docs-elsewhere.txt", "docs2", "other"});
}

// Regression test for issue #11. The SQL matches a literal prefix, so it agrees
// with the in-memory prefix match in WorkspaceEngine::drop_tombstones_under. An
// unescaped LIKE pattern would read "_" and "%" as wildcards and take "a-b/c"
// and "axb/c" with it, leaving the store and the cache to diverge after a
// rename involving such a path.
TEST_CASE("remove_tombstones_under treats LIKE wildcards in a path as literal characters") {
  WorkspaceFixture fixture;
  for (const char* path : {"a_b/c", "a-b/c", "axb/c", "unrelated"}) {
    REQUIRE(fixture.store->add_tombstone("feature-x", path).has_value());
  }
  REQUIRE(fixture.store->remove_tombstones_under("feature-x", "a_b").has_value());

  CHECK(sorted_tombstones(*fixture.store, "feature-x") ==
        std::vector<std::string>{"a-b/c", "axb/c", "unrelated"});
}

TEST_CASE("remove_tombstones_under treats a percent sign in a path as a literal character") {
  WorkspaceFixture fixture;
  for (const char* path : {"a%/c", "ab/c", "unrelated"}) {
    REQUIRE(fixture.store->add_tombstone("feature-x", path).has_value());
  }
  REQUIRE(fixture.store->remove_tombstones_under("feature-x", "a%").has_value());

  CHECK(sorted_tombstones(*fixture.store, "feature-x") ==
        std::vector<std::string>{"ab/c", "unrelated"});
}

TEST_CASE("clear_tombstones empties one Workspace and leaves the others alone") {
  WorkspaceFixture fixture;
  REQUIRE(fixture.store->add_tombstone("feature-x", "docs").has_value());
  REQUIRE(fixture.store->add_tombstone("feature-y", "docs").has_value());
  REQUIRE(fixture.store->clear_tombstones("feature-x").has_value());

  CHECK(sorted_tombstones(*fixture.store, "feature-x").empty());
  CHECK(sorted_tombstones(*fixture.store, "feature-y") == std::vector<std::string>{"docs"});
}
