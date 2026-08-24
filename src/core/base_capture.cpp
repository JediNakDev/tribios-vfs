#include "core/base_capture.hpp"

#include <sys/stat.h>

#include <chrono>
#include <system_error>

namespace tribios {

const char* const kSecretsWarning =
    "warning: the Base state captures ignored and untracked files, which may include secrets\n"
    "warning: every local process that can reach a Workspace can read them; Workspace isolation\n"
    "warning: is a correctness boundary, not a security boundary";

namespace {

OutcomeVoid capture_directory(const fs::path& source_root, const fs::path& base_root,
                              const std::string& relative, dev_t project_device,
                              CaptureStats& stats) {
  const fs::path source = relative.empty() ? source_root : source_root / relative;
  std::error_code ec;
  fs::directory_iterator entries(source, fs::directory_options::none, ec);
  if (ec) return error("base capture: cannot read " + source.string() + ": " + ec.message());

  for (const auto& entry : entries) {
    const std::string name = entry.path().filename().string();
    if (relative.empty() && (name == kGitDirName || name == kTribiosDirName)) continue;

    struct stat st {};
    if (::lstat(entry.path().c_str(), &st) != 0) continue;
    if (st.st_dev != project_device) continue;  // nested mount

    const std::string child = join_relative(relative, name);
    const fs::path target = base_root / child;

    if (S_ISLNK(st.st_mode)) {
      fs::create_symlink(fs::read_symlink(entry.path(), ec), target, ec);
      if (ec) return error("base capture: cannot recreate symlink " + child);
    } else if (S_ISDIR(st.st_mode)) {
      fs::create_directories(target, ec);
      if (ec) return error("base capture: cannot create " + target.string());
      ::chmod(target.c_str(), st.st_mode & 07777);
      auto captured = capture_directory(source_root, base_root, child, project_device, stats);
      if (!captured) return captured;
    } else if (S_ISREG(st.st_mode)) {
      fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing, ec);
      if (ec) return error("base capture: cannot copy " + child + ": " + ec.message());
      ::chmod(target.c_str(), st.st_mode & 07777);
      stats.bytes += st.st_size;
    } else {
      continue;  // special file
    }
    stats.entry_count++;
  }
  return {};
}

}  // namespace

Outcome<CaptureStats> capture_base_state(const fs::path& project_root, const fs::path& base_dir) {
  struct stat project_st {};
  if (::lstat(project_root.c_str(), &project_st) != 0 || !S_ISDIR(project_st.st_mode)) {
    return error("base capture: " + project_root.string() + " is not a directory");
  }
  std::error_code ec;
  fs::create_directories(base_dir, ec);
  if (ec) return error("base capture: cannot create " + base_dir.string());

  const auto started = std::chrono::steady_clock::now();
  CaptureStats stats;
  auto captured = capture_directory(project_root, base_dir, "", project_st.st_dev, stats);
  if (!captured) return std::unexpected(captured.error());
  stats.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - started)
                          .count();
  // Nothing writes to the Base state again and the Project source is never read
  // after this point, so later Project changes cannot reach existing Workspaces.
  return stats;
}

}  // namespace tribios
