#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "core/capture_policy.hpp"
#include "core/error.hpp"

namespace tribios {

// One path the capture policy left out. An excluded directory is listed once
// and its contents count toward the excluded totals.
struct CaptureExclusion {
  std::string relative;
  bool directory = false;
  std::string reason;
};

struct CaptureStats {
  std::int64_t entry_count = 0;
  std::int64_t bytes = 0;
  std::int64_t duration_ms = 0;
  std::int64_t excluded_entry_count = 0;
  std::int64_t excluded_bytes = 0;
  std::int64_t policy_evaluation_us = 0;
  std::vector<CaptureExclusion> exclusions;
};

// Copies the Project's current regular files, directories and symlinks into
// Tribios storage as one immutable Base state, regardless of Git tracking or
// ignore rules, minus what `policy` excludes. Git and Tribios metadata,
// special files and nested mounts are always left out; symlinks are copied as
// symlinks, never followed.
Outcome<CaptureStats> capture_base_state(const std::filesystem::path& project_root,
                                         const std::filesystem::path& base_dir,
                                         const CapturePolicy& policy);

// Makes exactly the decisions capture_base_state would and writes nothing.
Outcome<CaptureStats> plan_base_state_capture(const std::filesystem::path& project_root,
                                              const CapturePolicy& policy);

extern const char* const kSecretsWarning;

}  // namespace tribios
