#ifdef __APPLE__

#include "storage_internal.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <string>
#include <system_error>

#include "core/base_capture.hpp"
#include "core/proc.hpp"

namespace tribios {
namespace {

constexpr std::uint64_t kProbeImageBytes = 64ULL * 1024 * 1024;
constexpr std::uint64_t kImageSizeAlignmentBytes = 1024 * 1024;

std::filesystem::path base_image_path(const StorageConfiguration& configuration) {
  return configuration.private_dir / "base.sparseimage";
}

std::filesystem::path shadow_path(const StorageConfiguration& configuration,
                                  const std::string& name) {
  return configuration.private_dir / "shadows" / (name + ".shadow");
}

OutcomeVoid run_hdiutil(const std::vector<std::string>& arguments, std::string_view operation) {
  std::vector<std::string> command{"hdiutil"};
  command.insert(command.end(), arguments.begin(), arguments.end());
  const auto result = run_process_and_capture_output(command);
  if (!result.ok()) return error(std::string(operation) + ": " + result.output);
  return {};
}

OutcomeVoid create_sparse_apfs_image(const std::filesystem::path& image,
                                     std::uint64_t capacity_bytes) {
  return run_hdiutil({"create", "-size", std::to_string(capacity_bytes) + "b", "-type",
                      "SPARSE", "-fs", "Case-sensitive APFS", "-volname", "Tribios Base",
                      "-nospotlight", image.string()},
                     "cannot create the APFS Base-state image");
}

OutcomeVoid attach_image(const std::filesystem::path& image,
                         const std::filesystem::path& mount_point,
                         const std::filesystem::path& shadow = {}) {
  std::vector<std::string> arguments{"attach", image.string(), "-mountpoint",
                                     mount_point.string(), "-nobrowse", "-noautoopen"};
  if (!shadow.empty()) {
    arguments.push_back("-shadow");
    arguments.push_back(shadow.string());
  }
  return run_hdiutil(arguments, "cannot attach the APFS Workspace image");
}

OutcomeVoid detach_image(const std::filesystem::path& mount_point) {
  if (!path_is_mount_point(mount_point)) return {};
  return run_hdiutil({"detach", mount_point.string()}, "cannot detach the APFS Workspace image");
}

class ApfsShadowStorage final : public WorkspaceStorage {
 public:
  using WorkspaceStorage::WorkspaceStorage;

  std::string_view backend_identifier() const override { return kApfsShadowBackend; }

  Outcome<BaseStateCapture> capture_base_state(
      const std::filesystem::path& source_root,
      const CaptureProgressReporter& report_progress) override {
    const auto estimate = estimate_workspace_contents(source_root);
    if (!estimate) return std::unexpected(estimate.error());
    const std::uint64_t requested_capacity = static_cast<std::uint64_t>(estimate->bytes) +
                                             configuration_.growth_allowance_bytes;
    const std::uint64_t capacity =
        ((requested_capacity + kImageSizeAlignmentBytes - 1) / kImageSizeAlignmentBytes) *
        kImageSizeAlignmentBytes;
    const auto image = base_image_path(configuration_);
    std::error_code ec;
    std::filesystem::create_directories(configuration_.private_dir, ec);
    std::filesystem::create_directories(configuration_.base_dir, ec);
    if (ec) return error("cannot prepare APFS Base-state storage: " + ec.message());
    if (auto created = create_sparse_apfs_image(image, capacity); !created) {
      return std::unexpected(created.error());
    }
    if (auto attached = attach_image(image, configuration_.base_dir); !attached) {
      return std::unexpected(attached.error());
    }

    auto captured = copy_workspace_contents(source_root, configuration_.base_dir, report_progress);
    const auto detached = detach_image(configuration_.base_dir);
    if (!captured) return std::unexpected(captured.error());
    if (!detached) return std::unexpected(detached.error());
    if (::chmod(image.c_str(), 0444) != 0) {
      return error("cannot make the APFS Base-state image immutable");
    }
    return captured;
  }

  Outcome<std::string> create_workspace(const std::string& name) override {
    const auto workspace = workspace_path(name);
    const auto shadow = shadow_path(configuration_, name);
    std::error_code ec;
    std::filesystem::create_directories(workspace, ec);
    std::filesystem::create_directories(shadow.parent_path(), ec);
    if (ec) return error("cannot prepare APFS Workspace storage: " + ec.message());
    if (auto attached = attach_image(base_image_path(configuration_), workspace, shadow);
        !attached) {
      std::filesystem::remove(workspace, ec);
      return std::unexpected(attached.error());
    }
    return std::filesystem::relative(shadow, configuration_.tribios_dir).string();
  }

  OutcomeVoid attach_workspace(const std::string& name, const std::string& locator) override {
    const auto workspace = workspace_path(name);
    if (path_is_mount_point(workspace)) return {};
    std::error_code ec;
    std::filesystem::create_directories(workspace, ec);
    if (ec) return error("cannot create the APFS Workspace path: " + ec.message());
    return attach_image(base_image_path(configuration_), workspace,
                        configuration_.tribios_dir / locator);
  }

  OutcomeVoid detach_workspace(const std::string& name, const std::string&) override {
    return detach_image(workspace_path(name));
  }

  OutcomeVoid reclaim_workspace(const std::string& name, const std::string& locator) override {
    if (auto detached = detach_workspace(name, locator); !detached) return detached;
    std::error_code ec;
    const auto shadow = locator.empty() ? shadow_path(configuration_, name)
                                        : configuration_.tribios_dir / locator;
    std::filesystem::remove(shadow, ec);
    if (ec) return error("cannot remove the APFS Workspace shadow: " + ec.message());
    return remove_empty_workspace_path(workspace_path(name));
  }

  Outcome<WorkspaceStorageStatus> inspect_workspace(const std::string& name,
                                                    const std::string&) override {
    if (!path_is_mount_point(workspace_path(name))) return WorkspaceStorageStatus{};
    return read_filesystem_capacity(workspace_path(name));
  }
};

}  // namespace

StorageCapability probe_apfs_shadow_capability(const StorageConfiguration& configuration) {
  StorageCapability capability{kApfsShadowBackend, false, {}};
  const auto probe_root = configuration.private_dir /
                          ("probe-apfs-" + std::to_string(static_cast<long long>(::getpid())));
  const auto image = probe_root / "probe.sparseimage";
  const auto mount_point = probe_root / "mount";
  std::error_code ec;
  std::filesystem::create_directories(mount_point, ec);
  if (ec) {
    capability.missing_capability = "cannot create APFS capability-probe storage: " + ec.message();
    return capability;
  }
  auto created = create_sparse_apfs_image(image, kProbeImageBytes);
  auto attached = created ? attach_image(image, mount_point) : created;
  auto detached = attached ? detach_image(mount_point) : attached;
  std::filesystem::remove_all(probe_root, ec);
  if (!created) {
    capability.missing_capability = created.error();
  } else if (!attached) {
    capability.missing_capability = attached.error();
  } else if (!detached) {
    capability.missing_capability = detached.error();
  } else if (ec) {
    capability.missing_capability = "cannot remove APFS capability-probe storage: " + ec.message();
  } else {
    capability.supported = true;
  }
  return capability;
}

std::unique_ptr<WorkspaceStorage> make_apfs_shadow_storage(
    StorageConfiguration configuration) {
  return std::make_unique<ApfsShadowStorage>(std::move(configuration));
}

}  // namespace tribios

#endif
