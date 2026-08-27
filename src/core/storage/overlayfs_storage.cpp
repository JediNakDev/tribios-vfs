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

// OverlayFS creates a "work" subdirectory inside the work directory, owned by
// root with no permissions, and leaves it behind after unmounting. A recursive
// delete cannot list it, so it has to go first: it is always empty, and
// removing it needs only write permission on the work directory.
void remove_overlay_work_directory(const std::filesystem::path& work, std::error_code& ec) {
  std::filesystem::remove(work / "work", ec);
  std::filesystem::remove(work, ec);
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

class OverlayFsStorage final : public WorkspaceStorage {
 public:
  using WorkspaceStorage::WorkspaceStorage;

  std::string_view backend_identifier() const override { return kOverlayFsBackend; }

  Outcome<BaseStateCapture> capture_base_state(
      const std::filesystem::path& source_root,
      const CaptureProgressReporter& report_progress) override {
    // OverlayFS never writes to the lower tree, so the Base state stays
    // read-only by construction. Stripping its write bits would only travel
    // into every Workspace with the first copy-up and make the copy unwritable.
    return copy_workspace_contents(source_root, configuration_.base_dir, report_progress);
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
    remove_overlay_work_directory(private_path / "work", ec);
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
  const std::string probe_name =
      "probe-overlay-" + std::to_string(static_cast<long long>(::getpid()));
  const auto probe = configuration.private_dir / probe_name;
  const auto lower = probe / "lower";
  const auto upper = probe / "upper";
  const auto work = probe / "work";
  // The storage service only mounts onto a path below the Workspace directory,
  // so the probe has to mount where a real Workspace would.
  const auto target = configuration.workspaces_dir / probe_name;
  std::error_code ec;
  for (const auto& path : {lower, upper, work, target}) {
    std::filesystem::create_directories(path, ec);
  }
  StorageConfiguration probe_configuration = configuration;
  probe_configuration.base_dir = lower;
  const auto mounted = mount_overlay(probe_configuration, upper, work, target);
  const auto unmounted = mounted.ok ? unmount_overlay(probe_configuration, target)
                                    : StorageServiceResult{};
  std::filesystem::remove(target, ec);
  remove_overlay_work_directory(work, ec);
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
