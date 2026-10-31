#pragma once

#include <cstdint>
#include <filesystem>

#include "core/error.hpp"

namespace tribios {

// Hashes the Base state against HEAD once, at configure, into a private index.
// Git records stat data only for the Base-state files whose content matches the
// committed blob, so that index is the record of which files a Workspace can
// trust without rereading. An unborn HEAD records nothing.
OutcomeVoid capture_base_git_index(const std::filesystem::path& project_root,
                                   const std::filesystem::path& base_dir,
                                   const std::filesystem::path& tribios_dir);

// Fills a new Workspace's index, `<admin_dir>/index`, from HEAD. Entries that
// the Base-state index verified carry the stat data the Workspace reports for
// those files, so Git's first refresh does not reread every unchanged file
// through the mount. Everything else gets empty stat data, as `git read-tree`
// leaves it, and Git hashes it on first use.
OutcomeVoid populate_workspace_git_index(const std::filesystem::path& admin_dir,
                                         const std::filesystem::path& tribios_dir,
                                         const std::filesystem::path& base_dir,
                                         std::uint64_t inode_salt, std::uint32_t device);

}  // namespace tribios
