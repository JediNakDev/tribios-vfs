#pragma once

#include <cstdint>
#include <filesystem>

#include "core/error.hpp"

namespace tribios {

struct CaptureStats {
  std::int64_t entry_count = 0;
  std::int64_t bytes = 0;
  std::int64_t duration_ms = 0;
};

// Copies the Project's current regular files, directories and symlinks into
// Tribios storage as one immutable Base state, regardless of Git tracking or
// ignore rules. Git and Tribios metadata, special files and nested mounts are
// left out; symlinks are copied as symlinks, never followed.
Outcome<CaptureStats> capture_base_state(const std::filesystem::path& project_root,
                                         const std::filesystem::path& base_dir);

extern const char* const kSecretsWarning;

}  // namespace tribios
