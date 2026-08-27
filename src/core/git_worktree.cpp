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

// True when the Project turned on an index feature whose cached data describes
// the Project's own working tree and cannot be reused by a copy of it. The
// value is not always a boolean: `core.fsmonitor` may name a hook, so anything
// set and not explicitly false counts as enabled.
bool project_enabled(const std::filesystem::path& project_root, const std::string& key) {
  auto value = run_process_and_capture_output(
      {"git", "-C", project_root.string(), "config", "--get", key});
  if (!value.ok()) return false;
  const auto end = value.output.find_last_not_of(" \t\r\n");
  const std::string set = end == std::string::npos ? "" : value.output.substr(0, end + 1);
  return !set.empty() && set != "false";
}

// The Project's own index already carries a cached size and modification time
// for every tracked file, and the Base state copied those files with their
// modification times intact. Copying the index therefore hands the Workspace a
// warm stat cache, where `read-tree` would leave every entry zeroed and force a
// full re-hash. It also makes a Workspace open with the same staged and
// modified state the Project had, which is what copying a working tree means.
OutcomeVoid seed_workspace_index(const std::filesystem::path& project_root,
                                 const std::filesystem::path& admin_dir) {
  std::error_code ec;
  const std::filesystem::path project_index = project_root / kGitDirName / "index";
  // A split index stores most entries in a separate shared file that the
  // Workspace would resolve to its own administrative directory, and a
  // filesystem monitor reports on the Project's path rather than the
  // Workspace's. Neither survives copying, so fall back rather than seed.
  const bool copyable =
      !project_enabled(project_root, "core.splitIndex") &&
      !project_enabled(project_root, "core.fsmonitor");
  if (copyable && std::filesystem::is_regular_file(project_index, ec)) {
    std::filesystem::copy_file(project_index, admin_dir / "index",
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (!ec) return {};
  }
  // A Project that has never staged anything has no index file. Falling back
  // costs the first status a re-hash but always produces a correct index.
  auto populated =
      run_process_and_capture_output({"git", "--git-dir", admin_dir.string(), "read-tree", "HEAD"});
  if (!populated.ok()) return error("git read-tree failed: " + populated.output);
  return {};
}

// Git compares the inode number and creation time of a file against the index
// before trusting the cached hash, and both change when the Base state is
// copied. Excluding them leaves the modification time and size, which the copy
// preserves. The untracked cache is discarded for the same reason the split
// index is: it describes directories in the Project, not the Workspace.
// `--worktree` keeps these relaxations on the Workspace so the Project keeps
// Git's default strictness; it needs one shared extension flag.
OutcomeVoid trust_copied_stat_data(const std::filesystem::path& project_root,
                                   const std::filesystem::path& admin_dir) {
  auto extension = run_process_and_capture_output(
      {"git", "-C", project_root.string(), "config", "extensions.worktreeConfig", "true"});
  if (!extension.ok()) return error("cannot enable worktree config: " + extension.output);
  const std::string settings[][2] = {
      {"core.checkStat", "minimal"},
      {"core.trustctime", "false"},
      {"core.untrackedCache", "false"},
  };
  for (const auto& setting : settings) {
    auto set = run_process_and_capture_output({"git", "--git-dir", admin_dir.string(), "config",
                                               "--worktree", setting[0], setting[1]});
    if (!set.ok()) return error("cannot set " + setting[0] + " on the Workspace: " + set.output);
  }
  return {};
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
  // every tracked file as staged-deleted. An unborn HEAD has nothing to fill it
  // with.
  if (run_process_and_capture_output(
          {"git", "--git-dir", admin_dir.string(), "rev-parse", "--verify", "HEAD"})
          .ok()) {
    auto populated = seed_workspace_index(project_root, admin_dir);
    if (!populated) return std::unexpected(populated.error());
  }

  // A Workspace holds copies, so every file has a fresh inode and creation
  // time. Without this Git treats the seeded index as stale and re-hashes the
  // whole tree on the first status.
  auto configured = trust_copied_stat_data(project_root, admin_dir);
  if (!configured) return std::unexpected(configured.error());

  // A detached Workspace has no directory at the recorded gitdir path, which
  // makes `git worktree prune` delete its branch, HEAD and index. Only a lock
  // holds prune off, and manual prune ignores the usual grace period.
  auto locked = run_process_and_capture_output(
      {"git", "-C", project_root.string(), "worktree", "lock", workspace_path.string()});
  if (!locked.ok() && locked.output.find("already locked") == std::string::npos) {
    return error("git worktree lock failed: " + locked.output);
  }

  for (const char* name : {"HEAD", "commondir", "gitdir", "index", "locked", "config.worktree"}) {
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
  // Creation locks the worktree against prune, and `remove` refuses a locked
  // worktree even with `--force`. Unlocking a worktree that is not locked, or
  // not registered at all, is not an error worth stopping teardown for.
  run_process_and_capture_output(
      {"git", "-C", project_root.string(), "worktree", "unlock", workspace_path.string()});
  auto removed = run_process_and_capture_output(
      {"git", "-C", project_root.string(), "worktree", "remove", "--force",
       workspace_path.string()});
  if (!removed.ok() && removed.output.find("is not a working tree") == std::string::npos &&
      removed.output.find("is not a working tree directory") == std::string::npos) {
    // A detached Workspace is an empty directory whose ".git" file lived in the
    // Workspace storage, and `git worktree remove` refuses to validate a
    // working tree it cannot find. Dropping the empty directory and pruning
    // unregisters it. Every other Workspace is still locked, so prune leaves
    // them registered.
    std::error_code detached_ec;
    if (std::filesystem::exists(workspace_path / kGitDirName, detached_ec)) {
      return error("git worktree remove failed: " + removed.output);
    }
    std::filesystem::remove(workspace_path, detached_ec);
    if (detached_ec) {
      return error("cannot remove the detached Workspace path: " + detached_ec.message());
    }
    auto pruned = run_process_and_capture_output(
        {"git", "-C", project_root.string(), "worktree", "prune"});
    if (!pruned.ok()) return error("git worktree prune failed: " + pruned.output);
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
