#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "core/error.hpp"

namespace tribios {

bool is_git_project(const std::filesystem::path& project_root);

// What the new Workspace's index needs to describe its Base-state files the
// way the Workspace will report them. See workspace_git_index.hpp.
struct WorkspaceIndexSeed {
  std::filesystem::path tribios_dir;
  std::filesystem::path base_dir;
  std::uint64_t inode_salt = 0;
  std::uint32_t device = 0;
};

// Registers a Workspace as a Git linked worktree: its own branch, HEAD and
// index over the Project's shared objects and refs, with no second checkout.
// Returns the contents of the `.git` file the Workspace must expose.
Outcome<std::string> register_linked_worktree(const std::filesystem::path& project_root,
                                              const std::string& branch,
                                              const std::filesystem::path& workspace_path,
                                              const std::filesystem::path& staging_dir,
                                              const WorkspaceIndexSeed& seed);

// Drops the worktree administrative state through Git, keeping the branch and
// its commits.
OutcomeVoid unregister_linked_worktree(const std::filesystem::path& project_root,
                                       const std::filesystem::path& workspace_path);

// Rolls back an interrupted creation, including the branch that operation
// created.
OutcomeVoid rollback_linked_worktree_creation(const std::filesystem::path& project_root,
                                              const std::string& branch,
                                              const std::filesystem::path& workspace_path,
                                              const std::filesystem::path& staging_dir);

}  // namespace tribios
