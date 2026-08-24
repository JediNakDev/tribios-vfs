#include "core/git_worktree.hpp"

#include <fstream>
#include <sstream>
#include <system_error>

#include "core/proc.hpp"

namespace tribios {
namespace {

std::string read_first_line(const fs::path& path) {
  std::ifstream file(path);
  std::string line;
  std::getline(file, line);
  return line;
}

}  // namespace

bool is_git_project(const fs::path& project_root) {
  std::error_code ec;
  return fs::exists(project_root / kGitDirName, ec);
}

Outcome<std::string> register_linked_worktree(const fs::path& project_root,
                                              const std::string& branch,
                                              const fs::path& workspace_path,
                                              const fs::path& staging_dir) {
  std::error_code ec;
  fs::remove_all(staging_dir, ec);
  fs::create_directories(staging_dir.parent_path(), ec);

  // `--no-checkout` keeps Git from materializing a second working tree; Tribios
  // supplies the visible files from the Base state and the upper tree.
  auto added = run_process_and_capture_output(
      {"git", "-C", project_root.string(), "worktree", "add", "--no-checkout", "-b", branch,
       staging_dir.string()});
  if (!added.ok()) return error("git worktree add failed: " + added.output);

  // Git wrote "gitdir: <admin dir>" into the staging directory's .git file.
  const std::string git_file = read_first_line(staging_dir / kGitDirName);
  const auto marker = git_file.find("gitdir:");
  if (marker == std::string::npos) return error("unexpected .git file in the linked worktree");
  const fs::path admin_dir = git_file.substr(marker + 8);

  // Point the administrative state at the mounted Workspace, where Git will
  // find the worktree from now on, and drop the staging directory.
  std::ofstream(admin_dir / "gitdir") << (workspace_path / kGitDirName).string() << "\n";
  fs::remove_all(staging_dir, ec);

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

  return "gitdir: " + admin_dir.string() + "\n";
}

void unregister_linked_worktree(const fs::path& project_root, const std::string& name) {
  std::error_code ec;
  fs::remove_all(project_root / kGitDirName / "worktrees" / name, ec);
  run_process_and_capture_output({"git", "-C", project_root.string(), "worktree", "prune"});
}

}  // namespace tribios
