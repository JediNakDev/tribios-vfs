#include "core/base_capture.hpp"
#include "core/capture_policy.hpp"

#include <sys/stat.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>

#include "../unit/temporary_directory.hpp"

using tribios::CapturePolicy;

namespace {

struct PolicyProject {
  TemporaryDirectory root{"tribios-policy"};
  std::filesystem::path project = root.path() / "project";
  std::filesystem::path base = root.path() / "base";

  PolicyProject() {
    write_file_creating_parents(project / "src/main.cpp", "int main() { return 0; }\n");
    write_file_creating_parents(project / "secret.env", "TOKEN=hunter2\n");
    write_file_creating_parents(project / "cache/blob.bin", "0123456789");
    write_file_creating_parents(project / "cache/nested/more.bin", "abcde");
    write_file_creating_parents(project / "build/out.o", "object");
    write_file_creating_parents(project / "build/keep.txt", "kept");
    write_file_creating_parents(project / ".git/config", "[core]\n");
    std::filesystem::create_directories(project / "real-directory");
    std::filesystem::create_symlink("real-directory", project / "link-to-directory");
  }

  void write_policy(const std::string& source) {
    write_file_creating_parents(project / tribios::kCapturePolicyFileName, source);
  }

  CapturePolicy load_policy() {
    auto loaded = CapturePolicy::load_from_project(project);
    REQUIRE(loaded.has_value());
    return std::move(*loaded);
  }

  bool captured(const std::string& relative) const {
    return std::filesystem::exists(std::filesystem::symlink_status(base / relative));
  }
};

}  // namespace

TEST_CASE("a missing .tribiosignore yields an empty policy that keeps capture-all") {
  PolicyProject fixture;
  const CapturePolicy policy = fixture.load_policy();
  CHECK_FALSE(policy.present());
  CHECK_FALSE(policy.has_patterns());
  CHECK_FALSE(policy.decide("secret.env", false).excluded);
}

TEST_CASE("a .tribiosignore that is not a regular file is a capture error") {
  PolicyProject fixture;
  std::filesystem::create_directories(fixture.project / tribios::kCapturePolicyFileName);
  CHECK_FALSE(CapturePolicy::load_from_project(fixture.project).has_value());
}

TEST_CASE("capture leaves out excluded files and directories and counts them") {
  PolicyProject fixture;
  fixture.write_policy("secret.env\ncache/\n");
  const CapturePolicy policy = fixture.load_policy();
  auto stats = tribios::capture_base_state(fixture.project, fixture.base, policy);
  REQUIRE(stats.has_value());

  CHECK(fixture.captured("src/main.cpp"));
  CHECK_FALSE(fixture.captured("secret.env"));
  CHECK_FALSE(fixture.captured("cache"));

  // secret.env, cache, cache/blob.bin, cache/nested, cache/nested/more.bin.
  CHECK(stats->excluded_entry_count == 5);
  CHECK(stats->excluded_bytes == 14 + 10 + 5);
  REQUIRE(stats->exclusions.size() == 2);
  const auto cache =
      std::find_if(stats->exclusions.begin(), stats->exclusions.end(),
                   [](const auto& exclusion) { return exclusion.relative == "cache"; });
  REQUIRE(cache != stats->exclusions.end());
  CHECK(cache->directory);
  CHECK(cache->reason == ".tribiosignore:2: cache/");
}

TEST_CASE("a negation cannot re-include content beneath an excluded directory") {
  PolicyProject fixture;
  fixture.write_policy("build/\n!build/keep.txt\n");
  REQUIRE(tribios::capture_base_state(fixture.project, fixture.base, fixture.load_policy()));
  CHECK_FALSE(fixture.captured("build"));
  CHECK_FALSE(fixture.captured("build/keep.txt"));
}

TEST_CASE("excluding a directory's contents lets a negation keep one descendant") {
  PolicyProject fixture;
  fixture.write_policy("build/*\n!build/keep.txt\n");
  REQUIRE(tribios::capture_base_state(fixture.project, fixture.base, fixture.load_policy()));
  CHECK(fixture.captured("build/keep.txt"));
  CHECK_FALSE(fixture.captured("build/out.o"));
}

TEST_CASE("the policy file is captured even when a pattern matches it") {
  PolicyProject fixture;
  fixture.write_policy("*\n!src/\n!src/**\n");
  REQUIRE(tribios::capture_base_state(fixture.project, fixture.base, fixture.load_policy()));
  CHECK(read_whole_file(fixture.base / tribios::kCapturePolicyFileName) == "*\n!src/\n!src/**\n");
  CHECK(fixture.captured("src/main.cpp"));
  CHECK_FALSE(fixture.captured("secret.env"));
}

TEST_CASE("the captured policy file holds the bytes that were compiled") {
  PolicyProject fixture;
  fixture.write_policy("secret.env\n");
  const CapturePolicy policy = fixture.load_policy();
  // An edit after the policy was read cannot change what shaped this capture.
  fixture.write_policy("src/\n");
  REQUIRE(tribios::capture_base_state(fixture.project, fixture.base, policy));
  CHECK(read_whole_file(fixture.base / tribios::kCapturePolicyFileName) == "secret.env\n");
  CHECK(fixture.captured("src/main.cpp"));
  CHECK_FALSE(fixture.captured("secret.env"));
}

TEST_CASE("negations cannot bring back Tribios-excluded content") {
  PolicyProject fixture;
  fixture.write_policy("!.git\n!.git/**\n");
  REQUIRE(tribios::capture_base_state(fixture.project, fixture.base, fixture.load_policy()));
  CHECK_FALSE(fixture.captured(".git"));
}

TEST_CASE("symlinks match by their own path and are never followed") {
  PolicyProject fixture;
  // A directory-only pattern does not match a symlink to a directory.
  fixture.write_policy("link-to-directory/\n");
  REQUIRE(tribios::capture_base_state(fixture.project, fixture.base, fixture.load_policy()));
  CHECK(std::filesystem::is_symlink(fixture.base / "link-to-directory"));

  PolicyProject by_name;
  by_name.write_policy("link-to-directory\n");
  REQUIRE(tribios::capture_base_state(by_name.project, by_name.base, by_name.load_policy()));
  CHECK_FALSE(by_name.captured("link-to-directory"));
  CHECK(by_name.captured("real-directory"));
}

TEST_CASE("a dry run makes the same decisions as a capture and writes nothing") {
  PolicyProject fixture;
  fixture.write_policy("secret.env\ncache/\nbuild/*\n!build/keep.txt\n");
  const CapturePolicy policy = fixture.load_policy();
  auto planned = tribios::plan_base_state_capture(fixture.project, policy);
  REQUIRE(planned.has_value());
  CHECK_FALSE(std::filesystem::exists(fixture.base));

  auto captured = tribios::capture_base_state(fixture.project, fixture.base, policy);
  REQUIRE(captured.has_value());
  CHECK(planned->entry_count == captured->entry_count);
  CHECK(planned->bytes == captured->bytes);
  CHECK(planned->excluded_entry_count == captured->excluded_entry_count);
  CHECK(planned->excluded_bytes == captured->excluded_bytes);
  REQUIRE(planned->exclusions.size() == captured->exclusions.size());
  for (std::size_t index = 0; index < planned->exclusions.size(); ++index) {
    CHECK(planned->exclusions[index].relative == captured->exclusions[index].relative);
    CHECK(planned->exclusions[index].reason == captured->exclusions[index].reason);
  }
}

TEST_CASE("an explanation names the deciding pattern, its line and any excluded parent") {
  PolicyProject fixture;
  fixture.write_policy("# caches\ncache/\n*.env\n!example.env\n");
  const CapturePolicy policy = fixture.load_policy();
  const auto explain = [&](const std::string& relative) {
    auto explained = tribios::explain_capture_decision(fixture.project, policy, relative);
    REQUIRE(explained.has_value());
    return *explained;
  };

  const auto secret = explain("secret.env");
  CHECK(secret.excluded);
  CHECK(secret.reason == ".tribiosignore:3: *.env");

  const auto example = explain("example.env");
  CHECK_FALSE(example.excluded);
  CHECK(example.reason == ".tribiosignore:4: !example.env");

  const auto nested = explain("cache/nested/more.bin");
  CHECK(nested.excluded);
  CHECK(nested.reason == ".tribiosignore:2: cache/, matched parent directory cache");

  const auto source = explain("src/main.cpp");
  CHECK_FALSE(source.excluded);
  CHECK(source.reason == "no pattern matches");

  CHECK(explain(".git/config").excluded);
  CHECK_FALSE(explain(".tribiosignore").excluded);
}

TEST_CASE("a loaded capture policy survives deletion or replacement of its source") {
  PolicyProject fixture;
  const auto policy_path = fixture.project / tribios::kCapturePolicyFileName;
  fixture.write_policy("secret.env\n");
  REQUIRE(::chmod(policy_path.c_str(), 0640) == 0);
  const CapturePolicy policy = fixture.load_policy();

  SECTION("deleted policy") {
    std::filesystem::remove(policy_path);
  }
  SECTION("replaced with a directory") {
    std::filesystem::remove(policy_path);
    std::filesystem::create_directory(policy_path);
  }
  SECTION("replaced with a symlink") {
    std::filesystem::remove(policy_path);
    std::filesystem::create_symlink("src/main.cpp", policy_path);
  }

  const auto planned = tribios::plan_base_state_capture(fixture.project, policy);
  REQUIRE(planned.has_value());
  const auto captured = tribios::capture_base_state(fixture.project, fixture.base, policy);
  REQUIRE(captured.has_value());
  CHECK(read_whole_file(fixture.base / tribios::kCapturePolicyFileName) == "secret.env\n");
  CHECK_FALSE(fixture.captured("secret.env"));
  CHECK(fixture.captured("src/main.cpp"));
  CHECK(planned->entry_count == captured->entry_count);
  CHECK(planned->bytes == captured->bytes);
  struct stat st{};
  REQUIRE(::lstat((fixture.base / tribios::kCapturePolicyFileName).c_str(), &st) == 0);
  CHECK(S_ISREG(st.st_mode));
  CHECK((st.st_mode & 07777) == 0640);
}
