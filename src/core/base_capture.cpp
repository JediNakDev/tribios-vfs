#include "core/base_capture.hpp"

#include <sys/stat.h>

#include <chrono>
#include <filesystem>
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

OutcomeVoid copy_directory(const std::filesystem::path& source_root,
                           const std::filesystem::path& base_root, const std::string& relative,
                           dev_t project_device, const CaptureProgressReporter& report_progress,
                           BaseStateCapture& stats) {
  const std::filesystem::path source = relative.empty() ? source_root : source_root / relative;
  std::error_code ec;
  std::filesystem::directory_iterator entries(source, std::filesystem::directory_options::none, ec);
  if (ec) return error("base capture: cannot read " + source.string() + ": " + ec.message());

  for (const auto& entry : entries) {
    const std::string name = entry.path().filename().string();
    if (relative.empty() && (name == kGitDirName || name == kTribiosDirName)) continue;

    struct stat st{};
    if (::lstat(entry.path().c_str(), &st) != 0) continue;
    if (st.st_dev != project_device) continue;  // nested mount

    const std::string child = join_relative(relative, name);
    const std::filesystem::path target = base_root / child;

    if (S_ISLNK(st.st_mode)) {
      std::filesystem::create_symlink(std::filesystem::read_symlink(entry.path(), ec), target, ec);
      if (ec) return error("base capture: cannot recreate symlink " + child);
    } else if (S_ISDIR(st.st_mode)) {
      std::filesystem::create_directories(target, ec);
      if (ec) return error("base capture: cannot create " + target.string());
      ::chmod(target.c_str(), st.st_mode & 07777);
      auto copied =
          copy_directory(source_root, base_root, child, project_device, report_progress, stats);
      if (!copied) return copied;
    } else if (S_ISREG(st.st_mode)) {
      std::filesystem::copy_file(entry.path(), target,
                                 std::filesystem::copy_options::overwrite_existing, ec);
      if (ec) return error("base capture: cannot copy " + child + ": " + ec.message());
      ::chmod(target.c_str(), st.st_mode & 07777);
      stats.bytes += st.st_size;
    } else {
      continue;  // special file
    }
    stats.entry_count++;
    // Capture is the slowest step of Project configuration on macOS, so it
    // reports often enough to look alive without flooding the terminal.
    if (report_progress && stats.entry_count % 2000 == 0) {
      report_progress(stats.entry_count, stats.bytes);
    }
  }
  return {};
}

}  // namespace

Outcome<BaseStateCapture> copy_workspace_contents(const std::filesystem::path& project_root,
                                                  const std::filesystem::path& destination,
                                                  const CaptureProgressReporter& report_progress) {
  struct stat project_st{};
  if (::lstat(project_root.c_str(), &project_st) != 0 || !S_ISDIR(project_st.st_mode)) {
    return error("base capture: " + project_root.string() + " is not a directory");
  }
  std::error_code ec;
  std::filesystem::create_directories(destination, ec);
  if (ec) return error("base capture: cannot create " + destination.string());

  const auto started = std::chrono::steady_clock::now();
  BaseStateCapture stats;
  auto copied = copy_directory(project_root, destination, "", project_st.st_dev, report_progress,
                               stats);
  if (!copied) return std::unexpected(copied.error());
  if (report_progress) report_progress(stats.entry_count, stats.bytes);
  stats.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - started)
                          .count();
  // Nothing writes to the Base state again and the Project source is never read
  // after this point, so later Project changes cannot reach existing
  // Workspaces.
  return stats;
}

}  // namespace tribios
