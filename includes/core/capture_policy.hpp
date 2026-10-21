#pragma once

#include <sys/types.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/error.hpp"

namespace tribios {

inline constexpr const char* kCapturePolicyFileName = ".tribiosignore";

extern const char* const kCapturePolicyWarning;

struct CapturePattern {
  std::string text;  // the source line, for diagnostics
  int line = 0;
  std::string glob;
  bool negated = false;
  bool directory_only = false;
  bool anchored = false;  // matches the whole Project-relative path, not just the name
};

struct CaptureDecision {
  bool excluded = false;
  int pattern_index = -1;  // the last matching pattern, or -1 when none matched
};

// The Capture exclusions from a Project's root `.tribiosignore`, compiled once
// per capture. Patterns use gitignore-style syntax and the last matching
// pattern decides; docs/tribiosignore.md documents the grammar and every
// difference from Git.
class CapturePolicy {
 public:
  // Reads and validates `<project_root>/.tribiosignore`. A missing file yields
  // an empty policy that excludes nothing.
  static Outcome<CapturePolicy> load_from_project(const std::filesystem::path& project_root);
  static Outcome<CapturePolicy> compile(std::string source);

  // Decides one Project-relative path on its own. Callers walking a tree must
  // not descend into an excluded directory, which is what stops a negated
  // pattern from re-including content beneath it.
  CaptureDecision decide(std::string_view relative, bool is_directory) const;

  // "<file>:<line>: <pattern>" for a decision that a pattern made.
  std::string describe_pattern(const CaptureDecision& decision) const;

  bool present() const { return present_; }
  mode_t source_mode() const { return source_mode_; }
  bool has_patterns() const { return !patterns_.empty(); }
  // The exact bytes that were compiled. Capture writes these, rather than
  // re-reading the file, so a Base state always holds the policy that shaped it.
  const std::string& source() const { return source_; }

 private:
  bool present_ = false;
  mode_t source_mode_ = 0600;
  std::string source_;
  std::vector<CapturePattern> patterns_;
};

struct CaptureExplanation {
  bool excluded = false;
  std::string reason;
};

// Explains whether a capture would include one Project-relative path, applying
// Tribios' fixed rules and the excluded-parent traversal rule as well as the
// policy's patterns.
Outcome<CaptureExplanation> explain_capture_decision(const std::filesystem::path& project_root,
                                                     const CapturePolicy& policy,
                                                     std::string_view relative);

// Wildcard matching over a Project-relative path: `*` and `?` never match `/`,
// and `**` spans whole directories when it forms a complete path segment.
bool match_capture_glob(std::string_view glob, std::string_view text);

}  // namespace tribios
