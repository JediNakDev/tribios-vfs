#pragma once

// Declarations shared by the storage adapters in this directory. Nothing
// outside src/core/storage/ needs them, so they stay private to it.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "core/error.hpp"
#include "core/workspace_storage.hpp"

namespace tribios {

// True when `path` is the root of a mounted filesystem, which is how every
// adapter answers "is this Workspace attached" without parsing a mount table.
bool path_is_mount_point(const std::filesystem::path& path);

Outcome<WorkspaceStorageStatus> read_filesystem_capacity(const std::filesystem::path& path);

// Removes a directory that a detach or reclamation left empty. An already
// absent directory is success; a non-empty one is an error, because that means
// the caller detached something that still holds data.
OutcomeVoid remove_empty_workspace_path(const std::filesystem::path& path);

// Sends one narrowly scoped operation to the installed Linux storage service.
struct StorageServiceResult {
  bool ok = false;
  std::string output;
};
StorageServiceResult request_privileged_storage_operation(
    const std::vector<std::string>& arguments);
std::filesystem::path storage_service_socket_path();

#ifdef __APPLE__
std::unique_ptr<WorkspaceStorage> make_apfs_shadow_storage(StorageConfiguration configuration);
StorageCapability probe_apfs_shadow_capability(const StorageConfiguration& configuration);
#endif

#ifdef __linux__
std::unique_ptr<WorkspaceStorage> make_btrfs_snapshot_storage(StorageConfiguration configuration);
StorageCapability probe_btrfs_snapshot_capability(const StorageConfiguration& configuration);

std::unique_ptr<WorkspaceStorage> make_overlayfs_storage(StorageConfiguration configuration);
StorageCapability probe_overlayfs_capability(const StorageConfiguration& configuration);
#endif

}  // namespace tribios
