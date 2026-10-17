#include "core/base_capture.hpp"

#include <sys/stat.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>

#include "core/paths.hpp"

namespace tribios {

const char* const kSecretsWarning =
    "warning: the Base state captures ignored and untracked files, which may "
    "include secrets\n"
    "warning: every local process that can reach a Workspace can read them; "
    "Workspace isolation\n"
    "warning: is a correctness boundary, not a security boundary";

namespace {

struct CaptureWalk {
  const std::filesystem::path& source_root;
  const std::filesystem::path* base_root;  // null for a dry run
  const CapturePolicy& policy;
  dev_t project_device;
  CaptureStats& stats;
};

bool is_capturable_type(mode_t mode) {
  return S_ISREG(mode) || S_ISDIR(mode) || S_ISLNK(mode);
}

// Counts what an excluded entry would have contributed, under the same type and
// nested-mount rules the capture applies. An unreadable excluded directory
// counts as itself only: excluding it is how a user keeps capture away from it.
void count_excluded_tree(const std::filesystem::path& path, const struct stat& st,
                         dev_t project_device, CaptureStats& stats) {
  stats.excluded_entry_count++;
  if (S_ISREG(st.st_mode)) stats.excluded_bytes += st.st_size;
  if (!S_ISDIR(st.st_mode)) return;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(path, ec)) {
    struct stat child {};
    if (::lstat(entry.path().c_str(), &child) != 0) continue;
    if (child.st_dev != project_device || !is_capturable_type(child.st_mode)) continue;
    count_excluded_tree(entry.path(), child, project_device, stats);
  }
}

OutcomeVoid write_compiled_policy(const CaptureWalk& walk, const std::filesystem::path& target,
                                  mode_t mode) {
  if (walk.base_root == nullptr) return {};
  std::ofstream out(target, std::ios::binary | std::ios::trunc);
  out << walk.policy.source();
  out.close();
  if (!out) return error("base capture: cannot write " + target.string());
  ::chmod(target.c_str(), mode & 07777);
  return {};
}

OutcomeVoid capture_directory(const CaptureWalk& walk, const std::string& relative) {
  const std::filesystem::path source =
      relative.empty() ? walk.source_root : walk.source_root / relative;
  std::error_code ec;
  std::filesystem::directory_iterator entries(source, std::filesystem::directory_options::none, ec);
  if (ec) return error("base capture: cannot read " + source.string() + ": " + ec.message());

  for (const auto& entry : entries) {
    const std::string name = entry.path().filename().string();
    if (relative.empty() && (name == kGitDirName || name == kTribiosDirName)) continue;

    struct stat st{};
    if (::lstat(entry.path().c_str(), &st) != 0) continue;
    if (st.st_dev != walk.project_device) continue;  // nested mount
    if (!is_capturable_type(st.st_mode)) continue;   // special file

    const std::string child = join_relative(relative, name);
    const std::filesystem::path target =
        walk.base_root == nullptr ? std::filesystem::path{} : *walk.base_root / child;

    // The Base state's policy file is always the exact policy that shaped it. A
    // file that appeared after the policy was read did not shape anything.
    if (relative.empty() && name == kCapturePolicyFileName) {
      if (!walk.policy.present() || !S_ISREG(st.st_mode)) continue;
      if (auto written = write_compiled_policy(walk, target, st.st_mode); !written) {
        return written;
      }
      walk.stats.bytes += static_cast<std::int64_t>(walk.policy.source().size());
      walk.stats.entry_count++;
      continue;
    }

    const auto decided_at = std::chrono::steady_clock::now();
    const CaptureDecision decision = walk.policy.decide(child, S_ISDIR(st.st_mode));
    walk.stats.policy_evaluation_us += std::chrono::duration_cast<std::chrono::microseconds>(
                                           std::chrono::steady_clock::now() - decided_at)
                                           .count();
    if (decision.excluded) {
      count_excluded_tree(entry.path(), st, walk.project_device, walk.stats);
      walk.stats.exclusions.push_back(
          CaptureExclusion{child, S_ISDIR(st.st_mode), walk.policy.describe_pattern(decision)});
      continue;
    }

    if (S_ISLNK(st.st_mode)) {
      if (walk.base_root != nullptr) {
        std::filesystem::create_symlink(std::filesystem::read_symlink(entry.path(), ec), target,
                                        ec);
        if (ec) return error("base capture: cannot recreate symlink " + child);
      }
    } else if (S_ISDIR(st.st_mode)) {
      if (walk.base_root != nullptr) {
        std::filesystem::create_directories(target, ec);
        if (ec) return error("base capture: cannot create " + target.string());
        ::chmod(target.c_str(), st.st_mode & 07777);
      }
      if (auto captured = capture_directory(walk, child); !captured) return captured;
    } else {
      if (walk.base_root != nullptr) {
        std::filesystem::copy_file(entry.path(), target,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) return error("base capture: cannot copy " + child + ": " + ec.message());
        ::chmod(target.c_str(), st.st_mode & 07777);
      }
      walk.stats.bytes += st.st_size;
    }
    walk.stats.entry_count++;
  }
  return {};
}

Outcome<CaptureStats> walk_project(const std::filesystem::path& project_root,
                                   const std::filesystem::path* base_dir,
                                   const CapturePolicy& policy) {
  struct stat project_st{};
  if (::lstat(project_root.c_str(), &project_st) != 0 || !S_ISDIR(project_st.st_mode)) {
    return error("base capture: " + project_root.string() + " is not a directory");
  }
  if (base_dir != nullptr) {
    std::error_code ec;
    std::filesystem::create_directories(*base_dir, ec);
    if (ec) return error("base capture: cannot create " + base_dir->string());
  }

  const auto started = std::chrono::steady_clock::now();
  CaptureStats stats;
  const CaptureWalk walk{project_root, base_dir, policy, project_st.st_dev, stats};
  if (auto captured = capture_directory(walk, ""); !captured) {
    return std::unexpected(captured.error());
  }
  stats.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - started)
                          .count();
  return stats;
}

}  // namespace

Outcome<CaptureStats> capture_base_state(const std::filesystem::path& project_root,
                                         const std::filesystem::path& base_dir,
                                         const CapturePolicy& policy) {
  // Nothing writes to the Base state again and the Project source is never read
  // after this point, so later Project changes cannot reach existing
  // Workspaces.
  return walk_project(project_root, &base_dir, policy);
}

Outcome<CaptureStats> plan_base_state_capture(const std::filesystem::path& project_root,
                                              const CapturePolicy& policy) {
  return walk_project(project_root, nullptr, policy);
}

}  // namespace tribios
