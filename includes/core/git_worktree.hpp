#pragma once

#include <string>

#include "core/error.hpp"
#include "core/paths.hpp"

namespace tribios {

bool is_git_project(const fs::path& project_root);

// Registers a Workspace as a Git linked worktree: its own branch, HEAD and
// index over the Project's shared objects and refs, with no second checkout.
// Returns the contents of the `.git` file the Workspace must expose.
Outcome<std::string> register_linked_worktree(const fs::path& project_root,
                                              const std::string& branch,
                                              const fs::path& workspace_path,
                                              const fs::path& staging_dir);

// Drops the worktree administrative state, keeping the branch and its commits.
void unregister_linked_worktree(const fs::path& project_root, const std::string& name);

}  // namespace tribios
