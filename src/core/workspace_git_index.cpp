#include "core/workspace_git_index.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "core/git_index.hpp"
#include "core/paths.hpp"
#include "core/proc.hpp"
#include "core/recovery.hpp"
#include "core/workspace_engine.hpp"

namespace tribios {
namespace {

constexpr const char* kBaseGitIndexName = "base-git-index";
constexpr const char* kBaseGitCommitName = "base-git-commit";
constexpr std::size_t kSha1HexLength = 40;

// Settings that would tie an index to a work tree or spread it over several
// files. The Base-state index is copied into every Workspace, so it must be one
// self-contained file.
const std::vector<std::string> kSelfContainedIndexConfig = {
    "-c", "core.fsmonitor=false", "-c", "core.untrackedCache=false", "-c", "core.splitIndex=false"};

std::vector<std::string> concat(std::vector<std::string> head, const std::vector<std::string>& tail) {
  head.insert(head.end(), tail.begin(), tail.end());
  return head;
}

std::string trim_trailing_whitespace(std::string text) {
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.pop_back();
  return text;
}

std::string read_whole_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Only SHA-1 repositories are seeded; git_index.hpp does not parse SHA-256
// indexes, and those Workspaces fall back to an unseeded index.
std::string resolve_sha1_head_commit(const std::filesystem::path& git_dir) {
  auto head = run_process_and_capture_output(
      {"git", "--git-dir", git_dir.string(), "rev-parse", "--verify", "--quiet", "HEAD^{commit}"});
  if (!head.ok()) return {};
  std::string commit = trim_trailing_whitespace(head.output);
  return commit.size() == kSha1HexLength ? commit : std::string{};
}

#ifdef __APPLE__
long ctime_nanoseconds(const struct stat& st) { return st.st_ctimespec.tv_nsec; }
long mtime_nanoseconds(const struct stat& st) { return st.st_mtimespec.tv_nsec; }
#else
long ctime_nanoseconds(const struct stat& st) { return st.st_ctim.tv_nsec; }
long mtime_nanoseconds(const struct stat& st) { return st.st_mtim.tv_nsec; }
#endif

bool is_seedable_mode(std::uint32_t mode) {
  const std::uint32_t type = mode & S_IFMT;
  return type == S_IFREG || type == S_IFLNK;
}

// The Base-state index holds the Base files' own stat data wherever Git
// verified their content. Each such entry is rewritten to what the Workspace
// reports for the same file, which differs only in identity and precision: a
// salted inode, the mount's device, the daemon's owner, whole-second times.
OutcomeVoid translate_verified_entries(const std::filesystem::path& index_path,
                                       const std::filesystem::path& base_dir,
                                       std::uint64_t inode_salt, std::uint32_t device) {
  auto index = parse_git_index(read_whole_file(index_path));
  if (!index) return std::unexpected(index.error());

  const auto owner = static_cast<std::uint32_t>(::getuid());
  const auto group = static_cast<std::uint32_t>(::getgid());
  for (const GitIndexEntry& entry : index->entries) {
    if (!is_seedable_mode(entry.mode)) continue;
    struct stat st{};
    GitIndexStat workspace_stat;  // empty unless verified: Git then rereads the file
    if (::lstat((base_dir / entry.path).c_str(), &st) == 0) {
      const GitIndexStat base_stat{static_cast<std::uint32_t>(st.st_ctime),
                                   static_cast<std::uint32_t>(ctime_nanoseconds(st)),
                                   static_cast<std::uint32_t>(st.st_mtime),
                                   static_cast<std::uint32_t>(mtime_nanoseconds(st)),
                                   0,
                                   static_cast<std::uint32_t>(st.st_ino),
                                   static_cast<std::uint32_t>(st.st_uid),
                                   static_cast<std::uint32_t>(st.st_gid),
                                   static_cast<std::uint32_t>(st.st_size)};
      GitIndexStat recorded = read_git_index_stat(*index, entry);
      // The device number of a removable volume changes between mounts, and
      // Git ignores it, so it does not decide whether the entry is verified.
      recorded.device = 0;
      if (recorded == base_stat) {
        workspace_stat = GitIndexStat{
            static_cast<std::uint32_t>(st.st_ctime),
            0,
            static_cast<std::uint32_t>(st.st_mtime),
            0,
            device,
            static_cast<std::uint32_t>(
                salted_workspace_inode(inode_salt, static_cast<std::uint64_t>(st.st_ino))),
            owner,
            group,
            static_cast<std::uint32_t>(st.st_size)};
      }
    }
    write_git_index_stat(*index, entry, workspace_stat);
  }
  seal_git_index(*index);

  const std::filesystem::path staged = index_path.string() + ".tribios";
  {
    std::ofstream out(staged, std::ios::binary | std::ios::trunc);
    out << index->bytes;
    if (!out) return error("cannot write " + staged.string());
  }
  std::error_code ec;
  std::filesystem::rename(staged, index_path, ec);
  if (ec) return error("cannot publish " + index_path.string() + ": " + ec.message());
  return {};
}

OutcomeVoid seed_workspace_git_index(const std::filesystem::path& admin_dir,
                                     const std::filesystem::path& tribios_dir,
                                     const std::filesystem::path& base_dir,
                                     const std::string& head_commit, std::uint64_t inode_salt,
                                     std::uint32_t device) {
  const std::filesystem::path base_index = tribios_dir / kBaseGitIndexName;
  const std::string base_commit =
      trim_trailing_whitespace(read_whole_file(tribios_dir / kBaseGitCommitName));
  std::error_code ec;
  if (base_commit.size() != kSha1HexLength || !std::filesystem::is_regular_file(base_index, ec)) {
    return error("this Project has no Base-state Git index");
  }

  const std::filesystem::path index_path = admin_dir / "index";
  std::filesystem::copy_file(base_index, index_path,
                             std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) return error("cannot copy the Base-state Git index: " + ec.message());

  // A two-tree merge carries an entry's stat data forward only when its blob
  // is the same at both commits, so a file changed since configure is hashed.
  // `-i` keeps Git from checking a work tree this command does not have.
  if (head_commit != base_commit) {
    auto merged = run_process_and_capture_output(concat(
        concat({"git"}, kSelfContainedIndexConfig),
        {"--git-dir", admin_dir.string(), "read-tree", "-m", "-i", base_commit, head_commit}));
    if (!merged.ok()) return error("git read-tree -m failed: " + merged.output);
  }
  return translate_verified_entries(index_path, base_dir, inode_salt, device);
}

}  // namespace

OutcomeVoid capture_base_git_index(const std::filesystem::path& project_root,
                                   const std::filesystem::path& base_dir,
                                   const std::filesystem::path& tribios_dir) {
  const std::filesystem::path git_dir = project_root / kGitDirName;
  const std::string commit = resolve_sha1_head_commit(git_dir);
  if (commit.empty()) return {};

  // Git distrusts an entry whose mtime is not older, in whole seconds, than the
  // index it was read from, and empties its stat data on the next write. Every
  // Base-state file was written before this call, so writing the index in a
  // later second keeps them all trusted. The margin covers kernels whose file
  // timestamps lag the wall clock by a scheduler tick.
  std::this_thread::sleep_until(
      std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()) +
      std::chrono::milliseconds(1050));

  const std::filesystem::path index_path = tribios_dir / kBaseGitIndexName;
  const std::vector<std::string> git = concat(
      concat({"env", "GIT_INDEX_FILE=" + index_path.string(), "git"}, kSelfContainedIndexConfig),
      {"--git-dir", git_dir.string(), "--work-tree", base_dir.string()});
  auto read = run_process_and_capture_output(concat(git, {"read-tree", commit}));
  if (!read.ok()) return error("git read-tree of the Base state failed: " + read.output);
  // `-q` keeps going past Base-state files that differ from HEAD; they keep
  // empty stat data.
  auto refreshed = run_process_and_capture_output(concat(git, {"update-index", "-q", "--refresh"}));
  if (!refreshed.ok()) return error("git update-index of the Base state failed: " + refreshed.output);

  const std::filesystem::path commit_path = tribios_dir / kBaseGitCommitName;
  {
    std::ofstream out(commit_path, std::ios::trunc);
    out << commit << "\n";
    if (!out) return error("cannot write " + commit_path.string());
  }
  if (sync_file_data(index_path) != 0 || sync_file_data(commit_path) != 0) {
    return error("cannot flush the Base-state Git index");
  }
  return {};
}

OutcomeVoid populate_workspace_git_index(const std::filesystem::path& admin_dir,
                                         const std::filesystem::path& tribios_dir,
                                         const std::filesystem::path& base_dir,
                                         std::uint64_t inode_salt, std::uint32_t device) {
  const std::string head_commit = resolve_sha1_head_commit(admin_dir);
  if (!head_commit.empty() &&
      seed_workspace_git_index(admin_dir, tribios_dir, base_dir, head_commit, inode_salt, device)) {
    return {};
  }

  // Unseeded: a Project configured before the Base-state index existed, a
  // SHA-256 repository, or a seeding failure. An unborn HEAD has nothing to read.
  if (!run_process_and_capture_output(
           {"git", "--git-dir", admin_dir.string(), "rev-parse", "--verify", "HEAD"})
           .ok()) {
    return {};
  }
  auto populated =
      run_process_and_capture_output({"git", "--git-dir", admin_dir.string(), "read-tree", "HEAD"});
  if (!populated.ok()) return error("git read-tree failed: " + populated.output);
  return {};
}

}  // namespace tribios
