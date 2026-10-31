#include "core/capture_policy.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>

using tribios::CapturePolicy;
using tribios::match_capture_glob;

namespace {

CapturePolicy compile_policy(const std::string& source) {
  auto compiled = CapturePolicy::compile(source);
  REQUIRE(compiled.has_value());
  return std::move(*compiled);
}

bool excludes_file(const CapturePolicy& policy, const std::string& relative) {
  return policy.decide(relative, false).excluded;
}

bool excludes_directory(const CapturePolicy& policy, const std::string& relative) {
  return policy.decide(relative, true).excluded;
}

std::string compile_error(const std::string& source) {
  auto compiled = CapturePolicy::compile(source);
  REQUIRE_FALSE(compiled.has_value());
  return compiled.error();
}

}  // namespace

TEST_CASE("wildcards never cross a path separator except as a whole ** segment") {
  CHECK(match_capture_glob("*.log", "debug.log"));
  CHECK_FALSE(match_capture_glob("*.log", "logs/debug.log"));
  CHECK(match_capture_glob("debug?.log", "debug1.log"));
  CHECK_FALSE(match_capture_glob("a?b", "a/b"));
  CHECK(match_capture_glob("**/fixtures", "fixtures"));
  CHECK(match_capture_glob("**/fixtures", "test/unit/fixtures"));
  CHECK(match_capture_glob("docs/**", "docs/a/b.md"));
  CHECK(match_capture_glob("a/**/b", "a/b"));
  CHECK(match_capture_glob("a/**/b", "a/x/y/b"));
  CHECK_FALSE(match_capture_glob("a/**/b", "a/x/c"));
  // Not a whole segment, so ** behaves like *.
  CHECK(match_capture_glob("foo**", "foobar"));
  CHECK_FALSE(match_capture_glob("foo**", "foo/bar"));
}

TEST_CASE("repeated wildcards followed by a failing literal finish in polynomial time") {
  std::string repeated_star_then_a;
  for (int repeat = 0; repeat < 12; ++repeat) repeated_star_then_a += "*a";
  const std::string only_a_characters(50, 'a');
  CHECK_FALSE(match_capture_glob(repeated_star_then_a + "b", only_a_characters));
  CHECK(match_capture_glob(repeated_star_then_a + "b", only_a_characters + "b"));

  std::string repeated_directory_wildcards;
  for (int repeat = 0; repeat < 12; ++repeat) repeated_directory_wildcards += "**/a*/";
  std::string deep_path;
  for (int repeat = 0; repeat < 30; ++repeat) deep_path += "aaa/";
  CHECK_FALSE(match_capture_glob(repeated_directory_wildcards + "b", deep_path + "aaa"));
  CHECK(match_capture_glob(repeated_directory_wildcards + "b", deep_path + "b"));
}

TEST_CASE("character classes support ranges, negation and a leading literal bracket") {
  CHECK(match_capture_glob("file[0-9].txt", "file7.txt"));
  CHECK_FALSE(match_capture_glob("file[0-9].txt", "filex.txt"));
  CHECK(match_capture_glob("file[!0-9].txt", "filex.txt"));
  CHECK(match_capture_glob("file[^0-9].txt", "filex.txt"));
  CHECK(match_capture_glob("[]]", "]"));
  CHECK(match_capture_glob("[a-]", "-"));
  CHECK_FALSE(match_capture_glob("[a-c]", "/"));
}

TEST_CASE("names without a slash match at any depth and slashes anchor to the root") {
  const CapturePolicy policy = compile_policy("secret.env\n/root-only.txt\nconfig/local.json\n");
  CHECK(excludes_file(policy, "secret.env"));
  CHECK(excludes_file(policy, "nested/deeper/secret.env"));
  CHECK(excludes_file(policy, "root-only.txt"));
  CHECK_FALSE(excludes_file(policy, "nested/root-only.txt"));
  CHECK(excludes_file(policy, "config/local.json"));
  CHECK_FALSE(excludes_file(policy, "app/config/local.json"));
}

TEST_CASE("a trailing slash matches directories only") {
  const CapturePolicy policy = compile_policy("cache/\n");
  CHECK(excludes_directory(policy, "cache"));
  CHECK(excludes_directory(policy, "src/cache"));
  CHECK_FALSE(excludes_file(policy, "cache"));
}

TEST_CASE("comments, blank lines and trailing whitespace are ignored") {
  const CapturePolicy policy = compile_policy("# a comment\n\n   \nsecret.env   \r\n");
  CHECK(excludes_file(policy, "secret.env"));
  CHECK_FALSE(excludes_file(policy, "# a comment"));
}

TEST_CASE("a backslash escapes comment, negation and trailing-space characters") {
  const CapturePolicy policy = compile_policy("\\#notes.txt\n\\!important\nspace\\ \n");
  CHECK(excludes_file(policy, "#notes.txt"));
  CHECK(excludes_file(policy, "!important"));
  CHECK(excludes_file(policy, "space "));
  CHECK_FALSE(excludes_file(policy, "space"));
}

TEST_CASE("the last matching pattern decides and negation re-includes") {
  const CapturePolicy policy = compile_policy("*.env\n!example.env\n");
  CHECK(excludes_file(policy, "secret.env"));
  CHECK_FALSE(excludes_file(policy, "example.env"));

  const CapturePolicy reversed = compile_policy("!example.env\n*.env\n");
  CHECK(excludes_file(reversed, "example.env"));

  const auto decision = policy.decide("example.env", false);
  CHECK(policy.describe_pattern(decision) == ".tribiosignore:2: !example.env");
}

TEST_CASE("matching is byte-exact and case-sensitive on every host") {
  const CapturePolicy policy = compile_policy("Secret.env\n");
  CHECK(excludes_file(policy, "Secret.env"));
  CHECK_FALSE(excludes_file(policy, "secret.env"));
  CHECK_FALSE(excludes_file(policy, "SECRET.ENV"));
}

TEST_CASE("malformed policies are rejected with the offending line") {
  CHECK(compile_error("ok.txt\n[abc\n") == ".tribiosignore:2: unterminated character class");
  CHECK(compile_error("trailing\\") == ".tribiosignore:1: pattern ends with an unfinished escape");
  CHECK(compile_error("!\n") == ".tribiosignore:1: pattern names no path");
  CHECK(compile_error("/\n") == ".tribiosignore:1: pattern names no path");
  CHECK(compile_error("a//b\n") == ".tribiosignore:1: pattern contains an empty path segment");
  CHECK(compile_error(std::string("a\0b\n", 4)) == ".tribiosignore: contains a NUL byte");
  CHECK(compile_error("caf\xC3\n") == ".tribiosignore: is not valid UTF-8");
  CHECK(compile_error("\xC0\xAF\n") == ".tribiosignore: is not valid UTF-8");
  CHECK(CapturePolicy::compile("caf\xC3\xA9.txt\n").has_value());
}
