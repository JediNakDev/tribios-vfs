#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>

#include "core/error.hpp"
#include "core/workspace_storage.hpp"

namespace tribios {

// Copies the Project's current regular files, directories and symlinks into
// `destination`, regardless of Git tracking or ignore rules. Git and Tribios
// metadata, special files and nested mounts are left out; symlinks are copied
// as symlinks, never followed.
//
// The destination is whatever the selected storage adapter prepared for its
// Base state: a mounted sparse image on macOS, a writable Btrfs subvolume, or a
// plain lower directory. Which entries belong to the Workspace contents is a
// Project rule, so it lives here rather than in three adapters.
Outcome<BaseStateCapture> copy_workspace_contents(const std::filesystem::path& project_root,
                                                  const std::filesystem::path& destination,
                                                  const CaptureProgressReporter& report_progress);

extern const char* const kSecretsWarning;

}  // namespace tribios
