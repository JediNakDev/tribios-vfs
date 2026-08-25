// Covers core/paths.hpp: turning any caller-supplied path into a
// Project-relative path, and what that normalization does and does not do about
// paths that try to leave the Workspace.

#include "core/paths.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "core/error.hpp"
#include "core/metadata_store.hpp"
#include "core/workspace_engine.hpp"
#include "temporary_directory.hpp"

using tribios::join_relative;
using tribios::normalize_relative;
using tribios::parent_of;

TEST_CASE("normalize_relative maps every spelling of the view root to the empty string") {
  CHECK(normalize_relative("") == "");
  CHECK(normalize_relative("/") == "");
  CHECK(normalize_relative("///") == "");
  CHECK(normalize_relative(".") == "");
  CHECK(normalize_relative("./") == "");
  CHECK(normalize_relative("/./././") == "");
}

TEST_CASE("normalize_relative strips the leading slash so no result is absolute") {
  CHECK(normalize_relative("/src/main.cpp") == "src/main.cpp");
  CHECK(normalize_relative("////src/main.cpp") == "src/main.cpp");
  CHECK(normalize_relative("src/main.cpp") == "src/main.cpp");
}

TEST_CASE("normalize_relative drops trailing slashes and empty components") {
  CHECK(normalize_relative("src/") == "src");
  CHECK(normalize_relative("/src/nested///") == "src/nested");
  CHECK(normalize_relative("src//nested//main.cpp") == "src/nested/main.cpp");
}

TEST_CASE("normalize_relative drops current-directory segments anywhere in the path") {
  CHECK(normalize_relative("./src/./nested/.") == "src/nested");
  CHECK(normalize_relative("/./src/main.cpp") == "src/main.cpp");
}

TEST_CASE("normalize_relative keeps names that merely begin with or consist of dots") {
  CHECK(normalize_relative(".gitignore") == ".gitignore");
  CHECK(normalize_relative("src/...") == "src/...");
  CHECK(normalize_relative(".tribios/base") == ".tribios/base");
}

TEST_CASE("normalize_relative is idempotent, so re-normalizing a stored path is a no-op") {
  for (const char* path : {"", "/", "./a//b/", "a/../b", "/.git/config", "..."}) {
    const std::string once = normalize_relative(path);
    CHECK(normalize_relative(once) == once);
  }
}

TEST_CASE("normalize_relative resolves a parent segment against the segment before it") {
  CHECK(normalize_relative("src/../main.cpp") == "main.cpp");
  CHECK(normalize_relative("src/nested/../main.cpp") == "src/main.cpp");
  CHECK(normalize_relative("a/./../b") == "b");
  CHECK(normalize_relative("src/nested/..") == "src");
}

// Regression test for issue #10. The parent of the view root is the view root,
// so a ".." with nothing left to pop is dropped and the result still names a
// path inside the Workspace.
TEST_CASE("normalize_relative clamps parent segments that would climb past the view root") {
  CHECK(normalize_relative("..") == "");
  CHECK(normalize_relative("../..") == "");
  CHECK(normalize_relative("../secret.txt") == "secret.txt");
  CHECK(normalize_relative("/../../secret.txt") == "secret.txt");
  CHECK(normalize_relative("src/../../secret.txt") == "secret.txt");
  CHECK(normalize_relative("../../two/upper/secret.txt") == "two/upper/secret.txt");
}

TEST_CASE("parent_of walks one level up and reports the view root as the empty string") {
  CHECK(parent_of("src/nested/main.cpp") == "src/nested");
  CHECK(parent_of("src/nested") == "src");
  CHECK(parent_of("src") == "");
  CHECK(parent_of("") == "");
}

TEST_CASE("join_relative adds a separator only below the view root") {
  CHECK(join_relative("", "src") == "src");
  CHECK(join_relative("src", "main.cpp") == "src/main.cpp");
  CHECK(join_relative("src/nested", "main.cpp") == "src/nested/main.cpp");
}

TEST_CASE("parent_of and join_relative round-trip every path back to itself") {
  for (const char* path : {"src/nested/main.cpp", "src/main.cpp", "main.cpp"}) {
    const std::string relative = path;
    const std::string parent = parent_of(relative);
    const std::string name = relative.substr(parent.empty() ? 0 : parent.size() + 1);
    CHECK(join_relative(parent, name) == relative);
  }
}

// Regression test for issue #10. The control socket reaches WorkspaceEngine
// without kernel path resolution in front of it, see
// dispatch_filesystem_request in src/daemon/control_server.cpp, so a ".." in a
// caller-supplied path used to name a file outside the Workspace. A mounted
// FUSE client was never affected: the kernel resolves ".." before the request
// is delivered.
TEST_CASE("WorkspaceEngine cannot reach a file outside the Workspace through parent segments") {
  TemporaryDirectory workspace_root("tribios-traversal");
  const auto base_dir = workspace_root.path() / "base";
  const auto upper_dir = workspace_root.path() / "upper";
  std::filesystem::create_directories(base_dir);
  std::filesystem::create_directories(upper_dir);
  write_file_creating_parents(workspace_root.path() / "outside-the-workspace.txt", "private");

  auto store = tribios::MetadataStore::open_database(workspace_root.path() / "meta.db");
  REQUIRE(store.has_value());
  tribios::WorkspaceEngine engine("ws", base_dir, upper_dir, **store);

  auto escaped = engine.getattr("../outside-the-workspace.txt");
  REQUIRE_FALSE(escaped.has_value());
  CHECK(escaped.error() == ENOENT);

  auto contents = engine.read_file("../outside-the-workspace.txt", 64, 0);
  REQUIRE_FALSE(contents.has_value());
  CHECK(contents.error() == ENOENT);
}
