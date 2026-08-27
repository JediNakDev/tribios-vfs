#ifdef __linux__

#include "storage_internal.hpp"

#include <linux/magic.h>
#include <sys/statfs.h>
#include <unistd.h>

#include <filesystem>
#include <string>

#include "core/base_capture.hpp"
#include "core/proc.hpp"

namespace tribios {
namespace {

OutcomeVoid run_btrfs(const std::vector<std::string>& arguments, std::string_view operation) {
  std::vector<std::string> command{"btrfs"};
  command.insert(command.end(), arguments.begin(), arguments.end());
  const auto result = run_process_and_capture_output(command);
  if (!result.ok()) return error(std::string(operation) + ": " + result.output);
  return {};
}

OutcomeVoid delete_subvolume(const StorageConfiguration& configuration,
                             const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return {};
  if (auto deleted = run_btrfs({"subvolume", "delete", path.string()},
                               "cannot delete the Btrfs subvolume");
      deleted) {
    return {};
  }
  const auto helper = request_privileged_storage_operation(
      {"btrfs-delete", configuration.project_root.string(), configuration.tribios_dir.string(),
       path.string()});
  if (!helper.ok) return error("cannot delete the Btrfs subvolume: " + helper.output);
  return {};
}

std::filesystem::path reclaimed_path(const StorageConfiguration& configuration,
                                     const std::string& name) {
  return configuration.private_dir / "reclaim" / name;
}

class BtrfsSnapshotStorage final : public WorkspaceStorage {
 public:
  using WorkspaceStorage::WorkspaceStorage;

  std::string_view backend_identifier() const override { return kBtrfsSnapshotBackend; }

  Outcome<BaseStateCapture> capture_base_state(
      const std::filesystem::path& source_root,
      const CaptureProgressReporter& report_progress) override {
    if (auto created = run_btrfs({"subvolume", "create", configuration_.base_dir.string()},
                                  "cannot create the Btrfs Base-state subvolume");
        !created) {
      return std::unexpected(created.error());
    }
    auto captured =
        copy_workspace_contents(source_root, configuration_.base_dir, report_progress);
    if (!captured) return std::unexpected(captured.error());
    if (auto immutable =
            run_btrfs({"property", "set", "-ts", configuration_.base_dir.string(), "ro", "true"},
                      "cannot make the Btrfs Base state read-only");
        !immutable) {
      return std::unexpected(immutable.error());
    }
    return captured;
  }

  Outcome<std::string> create_workspace(const std::string& name) override {
    if (auto created = run_btrfs({"subvolume", "snapshot", configuration_.base_dir.string(),
                                   workspace_path(name).string()},
                                  "cannot create the Btrfs Workspace snapshot");
        !created) {
      return std::unexpected(created.error());
    }
    return name;
  }

  OutcomeVoid attach_workspace(const std::string& name, const std::string&) override {
    std::error_code ec;
    if (std::filesystem::is_directory(workspace_path(name), ec)) return {};
    const auto reclaimed = reclaimed_path(configuration_, name);
    if (!std::filesystem::is_directory(reclaimed, ec)) {
      return error("the Btrfs Workspace snapshot is missing");
    }
    std::filesystem::rename(reclaimed, workspace_path(name), ec);
    if (ec) return error("cannot restore the Btrfs Workspace path: " + ec.message());
    return {};
  }

  OutcomeVoid detach_workspace(const std::string& name, const std::string&) override {
    std::error_code ec;
    if (!std::filesystem::exists(workspace_path(name), ec)) return {};
    const auto reclaimed = reclaimed_path(configuration_, name);
    std::filesystem::create_directories(reclaimed.parent_path(), ec);
    std::filesystem::rename(workspace_path(name), reclaimed, ec);
    if (ec) return error("cannot detach the Btrfs Workspace path: " + ec.message());
    return {};
  }

  OutcomeVoid reclaim_workspace(const std::string& name, const std::string&) override {
    const auto reclaimed = reclaimed_path(configuration_, name);
    std::error_code ec;
    const auto target = std::filesystem::exists(reclaimed, ec) ? reclaimed : workspace_path(name);
    return delete_subvolume(configuration_, target);
  }

  Outcome<WorkspaceStorageStatus> inspect_workspace(const std::string& name,
                                                    const std::string&) override {
    std::error_code ec;
    if (!std::filesystem::is_directory(workspace_path(name), ec)) {
      return WorkspaceStorageStatus{};
    }
    return read_filesystem_capacity(workspace_path(name));
  }
};

}  // namespace

StorageCapability probe_btrfs_snapshot_capability(const StorageConfiguration& configuration) {
  StorageCapability capability{kBtrfsSnapshotBackend, false, {}};
  struct statfs filesystem {};
  if (::statfs(configuration.private_dir.c_str(), &filesystem) != 0 ||
      static_cast<unsigned long>(filesystem.f_type) != BTRFS_SUPER_MAGIC) {
    capability.missing_capability = "Project storage is not on Btrfs";
    return capability;
  }
  const auto probe = configuration.private_dir /
                     ("probe-btrfs-" + std::to_string(static_cast<long long>(::getpid())));
  const auto snapshot = probe.string() + "-snapshot";
  auto created = run_btrfs({"subvolume", "create", probe.string()},
                            "snapshot creation is unavailable");
  auto snapped = created ? run_btrfs({"subvolume", "snapshot", probe.string(), snapshot},
                                     "snapshot creation is unavailable")
                         : created;
  auto snapshot_deleted =
      snapped ? delete_subvolume(configuration, snapshot) : snapped;
  auto base_deleted = created ? delete_subvolume(configuration, probe) : created;
  if (!created) {
    capability.missing_capability = created.error();
  } else if (!snapped) {
    capability.missing_capability = snapped.error();
  } else if (!snapshot_deleted || !base_deleted) {
    capability.missing_capability =
        !snapshot_deleted ? snapshot_deleted.error() : base_deleted.error();
  } else {
    capability.supported = true;
  }
  return capability;
}

std::unique_ptr<WorkspaceStorage> make_btrfs_snapshot_storage(
    StorageConfiguration configuration) {
  return std::make_unique<BtrfsSnapshotStorage>(std::move(configuration));
}

}  // namespace tribios

#endif
