#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/error.hpp"

namespace tribios {

// The lifecycle seam between the Workspace domain model and the native
// filesystem mechanism that backs it. Once a Workspace is attached, Git and
// developer tools reach kernel filesystem objects directly, so nothing here
// describes a read, write, metadata lookup, directory listing, rename, deletion
// or copy-up. Adding one would put Tribios back on the per-operation path this
// design exists to leave.

inline constexpr const char* kApfsShadowBackend = "apfs-shadow";
inline constexpr const char* kBtrfsSnapshotBackend = "btrfs-snapshot";
inline constexpr const char* kOverlayFsBackend = "overlayfs";

// The layout of backing-store objects a backend writes. It is persisted beside
// the backend identifier so an upgrade cannot reinterpret existing storage.
inline constexpr int kStorageFormatVersion = 1;

struct StorageCapability {
  std::string backend;
  bool supported = false;
  // Names the capability that is missing, in the imperative form an operator
  // can act on. Empty when supported.
  std::string missing_capability;
};

struct BaseStateCapture {
  std::int64_t entry_count = 0;
  std::int64_t bytes = 0;
  std::int64_t duration_ms = 0;
};

// What a stat-only walk of the Project source expects a capture to cost. The
// macOS image volume is sized from this before any bytes are copied, because an
// attached sparse image cannot grow after its Base state is captured.
struct WorkspaceContentsEstimate {
  std::int64_t entry_count = 0;
  std::int64_t bytes = 0;
};

struct WorkspaceStorageStatus {
  bool attached = false;
  std::uint64_t writable_capacity_bytes = 0;
  std::uint64_t writable_remaining_bytes = 0;
};

// Everything an adapter needs to place its objects. `workspaces_dir` holds the
// Workspace paths themselves; `private_dir` holds the shadow files, removed
// subvolumes and upper trees that never appear in a Workspace path.
struct StorageConfiguration {
  std::filesystem::path project_root;
  std::filesystem::path tribios_dir;
  std::filesystem::path base_dir;
  std::filesystem::path workspaces_dir;
  std::filesystem::path private_dir;
  std::uint64_t growth_allowance_bytes = 0;
};

using CaptureProgressReporter = std::function<void(std::int64_t entries, std::int64_t bytes)>;

class WorkspaceStorage {
 public:
  explicit WorkspaceStorage(StorageConfiguration configuration)
      : configuration_(std::move(configuration)) {}
  virtual ~WorkspaceStorage() = default;

  virtual std::string_view backend_identifier() const = 0;

  // Captures the Project's Workspace contents once into this backend's
  // immutable Base state. Reports progress because it is user-visible during
  // Project configuration and is the slowest step on macOS.
  virtual Outcome<BaseStateCapture> capture_base_state(
      const std::filesystem::path& source_root, const CaptureProgressReporter& report_progress) = 0;

  // Creates this Workspace's backing store and attaches it at its Workspace
  // path. Returns the locator that recovery needs to find the backing store
  // again.
  virtual Outcome<std::string> create_workspace(const std::string& name) = 0;

  // Brings an existing Workspace back to its Workspace path after a machine
  // restart. Succeeds when the Workspace is already attached there.
  virtual OutcomeVoid attach_workspace(const std::string& name, const std::string& locator) = 0;

  // Makes the Workspace path stop resolving. Succeeds when it already does not.
  virtual OutcomeVoid detach_workspace(const std::string& name, const std::string& locator) = 0;

  // Frees the backing store. Succeeds when it is already freed.
  virtual OutcomeVoid reclaim_workspace(const std::string& name, const std::string& locator) = 0;

  virtual Outcome<WorkspaceStorageStatus> inspect_workspace(const std::string& name,
                                                            const std::string& locator) = 0;

  std::filesystem::path workspace_path(const std::string& name) const {
    return configuration_.workspaces_dir / name;
  }
  const StorageConfiguration& storage_configuration() const { return configuration_; }

 protected:
  StorageConfiguration configuration_;
};

// Walks the Project source the way a capture would, without copying anything.
Outcome<WorkspaceContentsEstimate> estimate_workspace_contents(
    const std::filesystem::path& project_root);

// The larger of four times the expected Base state and 16 GiB. A sparse image
// charges roughly three percent of unused volume capacity once and nothing per
// Workspace, so the allowance is set generously rather than tightly.
std::uint64_t default_growth_allowance_bytes(std::uint64_t base_state_bytes);

// Runs each backend this build supports through both halves of its contract:
// the creating half and the destroying half. A backend that can create a
// Workspace it can never remove is reported unsupported.
std::vector<StorageCapability> probe_workspace_storage_capabilities(
    const StorageConfiguration& configuration);

// The first supported backend in the platform's fixed preference order.
Outcome<std::string> choose_supported_backend(const std::vector<StorageCapability>& capabilities);

Outcome<std::unique_ptr<WorkspaceStorage>> open_workspace_storage(const std::string& backend,
                                                                  StorageConfiguration configuration);

}  // namespace tribios
