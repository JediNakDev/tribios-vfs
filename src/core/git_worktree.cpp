#include "core/git_worktree.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

#include "core/paths.hpp"
#include "core/proc.hpp"
#include "core/recovery.hpp"

namespace tribios {
namespace {

std::string read_first_line(const std::filesystem::path& path) {
  std::ifstream file(path);
  std::string line;
  std::getline(file, line);
  return line;
}

}  // namespace

bool is_git_project(const std::filesystem::path& project_root) {
  std::error_code ec;
  return std::filesystem::exists(project_root / kGitDirName, ec);
}

Outcome<std::string> register_linked_worktree(const std::filesystem::path& project_root,
                                              const std::string& branch,
                                              const std::filesystem::path& workspace_path,
                                              const std::filesystem::path& staging_dir) {
  std::error_code ec;
  std::filesystem::remove_all(staging_dir, ec);
  std::filesystem::create_directories(staging_dir.parent_path(), ec);

  // `--no-checkout` keeps Git from materializing a second working tree; Tribios
  // supplies the visible files from the Base state and the upper tree.
  auto added =
      run_process_and_capture_output({"git", "-C", project_root.string(), "worktree", "add",
                                      "--no-checkout", "-b", branch, staging_dir.string()});
  if (!added.ok()) return error("git worktree add failed: " + added.output);

  // Git wrote "gitdir: <admin dir>" into the staging directory's .git file.
  const std::string git_file = read_first_line(staging_dir / kGitDirName);
  const auto marker = git_file.find("gitdir:");
  if (marker == std::string::npos) return error("unexpected .git file in the linked worktree");
  const std::filesystem::path admin_dir = git_file.substr(marker + 8);

  // Point the administrative state at the mounted Workspace, where Git will
  // find the worktree from now on, and drop the staging directory.
  {
    std::ofstream gitdir(admin_dir / "gitdir", std::ios::trunc);
    gitdir << (workspace_path / kGitDirName).string() << "\n";
    if (!gitdir) return error("cannot update the linked-worktree gitdir");
  }
  std::filesystem::remove_all(staging_dir, ec);

  // `--no-checkout` also leaves the index empty, which would make Git report
  // every tracked file as staged-deleted. Fill the index from HEAD; the files
  // themselves come from the Base state. An unborn HEAD has nothing to read.
  if (run_process_and_capture_output(
          {"git", "--git-dir", admin_dir.string(), "rev-parse", "--verify", "HEAD"})
          .ok()) {
    auto populated = run_process_and_capture_output(
        {"git", "--git-dir", admin_dir.string(), "read-tree", "HEAD"});
    if (!populated.ok()) return error("git read-tree failed: " + populated.output);
  }

  for (const char* name : {"HEAD", "commondir", "gitdir", "index"}) {
    const std::filesystem::path file = admin_dir / name;
    if (std::filesystem::is_regular_file(file, ec) && sync_file_data(file) != 0) {
      return error("cannot flush Git linked-worktree file " + file.string());
    }
  }
  if (sync_directory(admin_dir) != 0 || sync_parent_directory(admin_dir) != 0) {
    return error("cannot flush Git linked-worktree administrative directory");
  }
  const std::filesystem::path branch_ref = project_root / kGitDirName / "refs" / "heads" / branch;
  if (std::filesystem::is_regular_file(branch_ref, ec) &&
      (sync_file_data(branch_ref) != 0 || sync_parent_directory(branch_ref) != 0)) {
    return error("cannot flush Git branch " + branch);
  }

  return "gitdir: " + admin_dir.string() + "\n";
}

OutcomeVoid unregister_linked_worktree(const std::filesystem::path& project_root,
                                       const std::filesystem::path& workspace_path) {
  auto removed = run_process_and_capture_output(
      {"git", "-C", project_root.string(), "worktree", "remove", "--force",
       workspace_path.string()});
  if (!removed.ok() && removed.output.find("is not a working tree") == std::string::npos &&
      removed.output.find("is not a working tree directory") == std::string::npos) {
    return error("git worktree remove failed: " + removed.output);
  }
  const std::filesystem::path worktrees = project_root / kGitDirName / "worktrees";
  std::error_code ec;
  if (std::filesystem::is_directory(worktrees, ec) && sync_directory(worktrees) != 0) {
    return error("cannot flush Git linked-worktree registry");
  }
  if (sync_directory(project_root / kGitDirName) != 0) {
    return error("cannot flush Git administrative directory");
  }
  return {};
}

OutcomeVoid rollback_linked_worktree_creation(const std::filesystem::path& project_root,
                                              const std::string& branch,
                                              const std::filesystem::path& workspace_path,
                                              const std::filesystem::path& staging_dir) {
  auto remove_workspace = unregister_linked_worktree(project_root, workspace_path);
  auto remove_staging = unregister_linked_worktree(project_root, staging_dir);
  if (!remove_workspace && !remove_staging) return remove_workspace;
  auto deleted = run_process_and_capture_output(
      {"git", "-C", project_root.string(), "branch", "-D", branch});
  if (!deleted.ok() && deleted.output.find("not found") == std::string::npos) {
    return error("git branch rollback failed: " + deleted.output);
  }
  return {};
}

}  // namespace tribios
