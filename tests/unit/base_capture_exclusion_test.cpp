// Covers core/base_capture.hpp: which Project content enters a Base state and
// which content is excluded. CONTEXT.md defines a Capture exclusion as a
// Project-relative rule that is independent of Git ignore rules, so the tests
// below assert both what is left out and what is deliberately kept.
//
// There is no standalone exclusion predicate to call: the rules live inside the
// directory walk in src/core/base_capture.cpp, so the tests drive the public
// capture_base_state entry point over a temporary Project tree. No Git
// repository and no daemon are involved.

#include "core/base_capture.hpp"

#include <sys/stat.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include "temporary_directory.hpp"

using tribios::capture_base_state;

namespace {

// A Project tree that exercises every rule the capture walk applies.
struct CapturedProject {
  TemporaryDirectory root{"tribios-capture"};
  std::filesystem::path project = root.path() / "project";
  std::filesystem::path base = root.path() / "base";

  CapturedProject() {
    write_file_creating_parents(project / "src/main.cpp", "int main() { return 0; }\n");
    write_file_creating_parents(project / ".gitignore", "build/\nsecret.env\n");

    // Ignored by the .gitignore above; a Base state captures it anyway.
    write_file_creating_parents(project / "secret.env", "TOKEN=hunter2\n");
    write_file_creating_parents(project / "build/artifact.o", "object");

    // Git and Tribios metadata at the Project root.
    write_file_creating_parents(project / ".git/config", "[core]\n");
    write_file_creating_parents(project / ".tribios/meta.db", "sqlite");

    // The same names below the Project root are ordinary content.
    write_file_creating_parents(project / "vendor/dep/.git/config", "[core]\n");
    write_file_creating_parents(project / "vendor/dep/.tribios/notes", "notes");

    std::filesystem::create_symlink("src/main.cpp", project / "link-to-file");
    std::filesystem::create_symlink("src", project / "link-to-directory");
    std::filesystem::create_symlink("nowhere", project / "dangling-link");
    ::mkfifo((project / "control.fifo").c_str(), 0644);
    ::chmod((project / "src/main.cpp").c_str(), 0640);
  }
};

}  // namespace

TEST_CASE("capture_base_state copies regular files with their contents and permission bits") {
  CapturedProject fixture;
  auto stats = capture_base_state(fixture.project, fixture.base);
  REQUIRE(stats.has_value());

  REQUIRE(std::filesystem::is_regular_file(fixture.base / "src/main.cpp"));
  CHECK(read_whole_file(fixture.base / "src/main.cpp") == "int main() { return 0; }\n");

  struct stat st{};
  REQUIRE(::lstat((fixture.base / "src/main.cpp").c_str(), &st) == 0);
  CHECK((st.st_mode & 07777) == 0640);
}

TEST_CASE("the Capture exclusion for Git and Tribios metadata applies only at the Project root") {
  CapturedProject fixture;
  REQUIRE(capture_base_state(fixture.project, fixture.base).has_value());

  CHECK_FALSE(std::filesystem::exists(fixture.base / ".git"));
  CHECK_FALSE(std::filesystem::exists(fixture.base / ".tribios"));

  // A vendored dependency's own .git directory is Workspace content, not
  // administrative metadata of this Project, so the rule does not reach it.
  CHECK(std::filesystem::is_regular_file(fixture.base / "vendor/dep/.git/config"));
  CHECK(std::filesystem::is_regular_file(fixture.base / "vendor/dep/.tribios/notes"));
}

TEST_CASE("Capture exclusions are independent of Git ignore rules") {
  CapturedProject fixture;
  REQUIRE(capture_base_state(fixture.project, fixture.base).has_value());

  // Nothing reads .gitignore, so an ignored file and an ignored directory are
  // captured like any other content. kSecretsWarning is the documented
  // consequence of exactly this.
  CHECK(std::filesystem::is_regular_file(fixture.base / ".gitignore"));
  CHECK(read_whole_file(fixture.base / "secret.env") == "TOKEN=hunter2\n");
  CHECK(read_whole_file(fixture.base / "build/artifact.o") == "object");
  CHECK(std::string(tribios::kSecretsWarning).find("secrets") != std::string::npos);
}

TEST_CASE("symlinks are captured as symlinks and are never followed") {
  CapturedProject fixture;
  REQUIRE(capture_base_state(fixture.project, fixture.base).has_value());

  REQUIRE(std::filesystem::is_symlink(fixture.base / "link-to-file"));
  CHECK(std::filesystem::read_symlink(fixture.base / "link-to-file") == "src/main.cpp");

  // A symlink to a directory is copied as a link; the walk does not descend
  // through it and so does not duplicate the target's contents.
  REQUIRE(std::filesystem::is_symlink(fixture.base / "link-to-directory"));
  CHECK(std::filesystem::read_symlink(fixture.base / "link-to-directory") == "src");
  CHECK_FALSE(std::filesystem::is_directory(
      std::filesystem::symlink_status(fixture.base / "link-to-directory")));

  // A dangling link is captured verbatim rather than dropped or resolved.
  REQUIRE(std::filesystem::is_symlink(fixture.base / "dangling-link"));
  CHECK(std::filesystem::read_symlink(fixture.base / "dangling-link") == "nowhere");
}

TEST_CASE("special files are excluded from the Base state") {
  CapturedProject fixture;
  REQUIRE(capture_base_state(fixture.project, fixture.base).has_value());

  REQUIRE(std::filesystem::exists(std::filesystem::symlink_status(fixture.project / "control.fifo")));
  CHECK_FALSE(std::filesystem::exists(std::filesystem::symlink_status(fixture.base / "control.fifo")));
}

TEST_CASE("CaptureStats counts captured entries and sums only regular-file bytes") {
  CapturedProject fixture;
  auto stats = capture_base_state(fixture.project, fixture.base);
  REQUIRE(stats.has_value());

  // src, src/main.cpp, .gitignore, secret.env, build, build/artifact.o, vendor,
  // vendor/dep, vendor/dep/.git, vendor/dep/.git/config, vendor/dep/.tribios,
  // vendor/dep/.tribios/notes, link-to-file, link-to-directory, dangling-link.
  // The excluded .git, .tribios and control.fifo are not counted.
  CHECK(stats->entry_count == 15);

  std::int64_t regular_file_bytes = 0;
  for (const char* relative : {"src/main.cpp", ".gitignore", "secret.env", "build/artifact.o",
                               "vendor/dep/.git/config", "vendor/dep/.tribios/notes"}) {
    regular_file_bytes += static_cast<std::int64_t>(
        std::filesystem::file_size(fixture.project / relative));
  }
  CHECK(stats->bytes == regular_file_bytes);
  CHECK(stats->duration_ms >= 0);
}

TEST_CASE("capture_base_state creates the Base directory and captures an empty Project") {
  TemporaryDirectory root("tribios-capture-empty");
  const auto project = root.path() / "project";
  std::filesystem::create_directories(project);
  const auto base = root.path() / "nested/base";

  auto stats = capture_base_state(project, base);
  REQUIRE(stats.has_value());
  CHECK(stats->entry_count == 0);
  CHECK(stats->bytes == 0);
  CHECK(std::filesystem::is_directory(base));
}

TEST_CASE("capture_base_state reports an error when the Project root is not a directory") {
  TemporaryDirectory root("tribios-capture-error");
  write_file_creating_parents(root.path() / "not-a-directory", "x");

  auto missing = capture_base_state(root.path() / "does-not-exist", root.path() / "base");
  REQUIRE_FALSE(missing.has_value());
  CHECK(missing.error().find("is not a directory") != std::string::npos);

  auto regular_file = capture_base_state(root.path() / "not-a-directory", root.path() / "base");
  REQUIRE_FALSE(regular_file.has_value());
  CHECK(regular_file.error().find("is not a directory") != std::string::npos);
}
