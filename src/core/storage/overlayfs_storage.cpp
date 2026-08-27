#ifdef __linux__

#include "storage_internal.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <string>

#include "core/base_capture.hpp"

namespace tribios {
namespace {

std::filesystem::path overlay_private_path(const StorageConfiguration& configuration,
                                           const std::string& name) {
  return configuration.private_dir / "overlay" / name;
}

StorageServiceResult mount_overlay(const StorageConfiguration& configuration,
                                   const std::filesystem::path& upper,
                                   const std::filesystem::path& work,
                                   const std::filesystem::path& target) {
  return request_privileged_storage_operation(
      {"overlay-mount", configuration.project_root.string(), configuration.tribios_dir.string(),
       configuration.workspaces_dir.string(), configuration.base_dir.string(), upper.string(),
       work.string(), target.string()});
}

StorageServiceResult unmount_overlay(const StorageConfiguration& configuration,
                                     const std::filesystem::path& target) {
  return request_privileged_storage_operation(
      {"unmount", configuration.project_root.string(), configuration.tribios_dir.string(),
       configuration.workspaces_dir.string(), target.string()});
}

OutcomeVoid make_tree_immutable(const std::filesystem::path& root) {
  std::error_code ec;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec)) {
    if (ec) return error("cannot inspect the OverlayFS Base state: " + ec.message());
    const auto status = entry.symlink_status(ec);
    if (ec || std::filesystem::is_symlink(status)) continue;
    const auto permissions = status.permissions();
    std::filesystem::permissions(entry.path(), permissions & ~std::filesystem::perms::owner_write &
                                                   ~std::filesystem::perms::group_write &
                                                   ~std::filesystem::perms::others_write,
                                 ec);
    if (ec) return error("cannot make the OverlayFS Base state immutable: " + ec.message());
  }
  std::filesystem::permissions(root, std::filesystem::perms::owner_read |
                                         std::filesystem::perms::owner_exec |
                                         std::filesystem::perms::group_read |
                                         std::filesystem::perms::group_exec |
                                         std::filesystem::perms::others_read |
                                         std::filesystem::perms::others_exec,
                               ec);
  if (ec) return error("cannot make the OverlayFS Base state immutable: " + ec.message());
  return {};
}

class OverlayFsStorage final : public WorkspaceStorage {
 public:
  using WorkspaceStorage::WorkspaceStorage;

  std::string_view backend_identifier() const override { return kOverlayFsBackend; }

  Outcome<BaseStateCapture> capture_base_state(
      const std::filesystem::path& source_root,
      const CaptureProgressReporter& report_progress) override {
    auto captured =
        copy_workspace_contents(source_root, configuration_.base_dir, report_progress);
    if (!captured) return std::unexpected(captured.error());
    if (auto immutable = make_tree_immutable(configuration_.base_dir); !immutable) {
      return std::unexpected(immutable.error());
    }
    return captured;
  }

  Outcome<std::string> create_workspace(const std::string& name) override {
    const auto private_path = overlay_private_path(configuration_, name);
    const auto upper = private_path / "upper";
    const auto work = private_path / "work";
    std::error_code ec;
    std::filesystem::create_directories(upper, ec);
    std::filesystem::create_directories(work, ec);
    std::filesystem::create_directories(workspace_path(name), ec);
    if (ec) return error("cannot prepare OverlayFS Workspace storage: " + ec.message());
    const auto mounted = mount_overlay(configuration_, upper, work, workspace_path(name));
    if (!mounted.ok) return error("cannot mount the OverlayFS Workspace: " + mounted.output);
    return std::filesystem::relative(private_path, configuration_.tribios_dir).string();
  }

  OutcomeVoid attach_workspace(const std::string& name, const std::string& locator) override {
    if (path_is_mount_point(workspace_path(name))) return {};
    const auto private_path = locator.empty() ? overlay_private_path(configuration_, name)
                                              : configuration_.tribios_dir / locator;
    std::error_code ec;
    std::filesystem::create_directories(workspace_path(name), ec);
    if (ec) return error("cannot create the OverlayFS Workspace path: " + ec.message());
    const auto mounted = mount_overlay(configuration_, private_path / "upper",
                                       private_path / "work", workspace_path(name));
    if (!mounted.ok) return error("cannot mount the OverlayFS Workspace: " + mounted.output);
    return {};
  }

  OutcomeVoid detach_workspace(const std::string& name, const std::string&) override {
    if (!path_is_mount_point(workspace_path(name))) return {};
    const auto unmounted = unmount_overlay(configuration_, workspace_path(name));
    if (!unmounted.ok) return error("cannot unmount the OverlayFS Workspace: " + unmounted.output);
    return remove_empty_workspace_path(workspace_path(name));
  }

  OutcomeVoid reclaim_workspace(const std::string& name, const std::string& locator) override {
    if (auto detached = detach_workspace(name, locator); !detached) return detached;
    const auto private_path = locator.empty() ? overlay_private_path(configuration_, name)
                                              : configuration_.tribios_dir / locator;
    std::error_code ec;
    std::filesystem::remove_all(private_path, ec);
    if (ec) return error("cannot reclaim OverlayFS Workspace storage: " + ec.message());
    return {};
  }

  Outcome<WorkspaceStorageStatus> inspect_workspace(const std::string& name,
                                                    const std::string&) override {
    if (!path_is_mount_point(workspace_path(name))) return WorkspaceStorageStatus{};
    return read_filesystem_capacity(workspace_path(name));
  }
};

}  // namespace

StorageCapability probe_overlayfs_capability(const StorageConfiguration& configuration) {
  StorageCapability capability{kOverlayFsBackend, false, {}};
  const auto probe = configuration.private_dir /
                     ("probe-overlay-" + std::to_string(static_cast<long long>(::getpid())));
  const auto lower = probe / "lower";
  const auto upper = probe / "upper";
  const auto work = probe / "work";
  const auto target = probe / "target";
  std::error_code ec;
  for (const auto& path : {lower, upper, work, target}) {
    std::filesystem::create_directories(path, ec);
  }
  StorageConfiguration probe_configuration = configuration;
  probe_configuration.base_dir = lower;
  const auto mounted = mount_overlay(probe_configuration, upper, work, target);
  const auto unmounted = mounted.ok ? unmount_overlay(probe_configuration, target)
                                    : StorageServiceResult{};
  std::filesystem::remove_all(probe, ec);
  if (!mounted.ok) {
    capability.missing_capability =
        "host-namespace OverlayFS mounting is unavailable: " + mounted.output;
  } else if (!unmounted.ok) {
    capability.missing_capability = "OverlayFS unmounting is unavailable: " + unmounted.output;
  } else if (ec) {
    capability.missing_capability = "cannot remove OverlayFS probe storage: " + ec.message();
  } else {
    capability.supported = true;
  }
  return capability;
}

std::unique_ptr<WorkspaceStorage> make_overlayfs_storage(StorageConfiguration configuration) {
  return std::make_unique<OverlayFsStorage>(std::move(configuration));
}

}  // namespace tribios

#endif
