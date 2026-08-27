#include "core/workspace_storage.hpp"

#include <sys/stat.h>
#include <sys/statvfs.h>
#ifdef __linux__
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/un.h>
#endif
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <system_error>

#include "core/paths.hpp"
#include "core/proc.hpp"
#include "storage_internal.hpp"

namespace tribios {
namespace {

constexpr std::uint64_t kMinimumGrowthAllowanceBytes = 16ULL * 1024 * 1024 * 1024;

// Fixed per platform so a Project configured today and a Project configured
// after an upgrade choose the same backend from the same host.
std::vector<std::string> backend_preference_order() {
#ifdef __APPLE__
  return {kApfsShadowBackend};
#elif defined(__linux__)
  return {kBtrfsSnapshotBackend, kOverlayFsBackend};
#else
  return {};
#endif
}

OutcomeVoid estimate_directory(const std::filesystem::path& source_root,
                               const std::string& relative, dev_t project_device,
                               WorkspaceContentsEstimate& estimate) {
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

    if (S_ISDIR(st.st_mode)) {
      auto counted = estimate_directory(source_root, join_relative(relative, name), project_device,
                                        estimate);
      if (!counted) return counted;
    } else if (S_ISREG(st.st_mode)) {
      estimate.bytes += st.st_size;
    } else if (!S_ISLNK(st.st_mode)) {
      continue;  // special file
    }
    estimate.entry_count++;
  }
  return {};
}

}  // namespace

bool path_is_mount_point(const std::filesystem::path& path) {
  struct stat here{};
  struct stat parent{};
  if (::lstat(path.c_str(), &here) != 0) return false;
  if (!S_ISDIR(here.st_mode)) return false;
  if (::lstat(path.parent_path().c_str(), &parent) != 0) return false;
  return here.st_dev != parent.st_dev;
}

Outcome<WorkspaceStorageStatus> read_filesystem_capacity(const std::filesystem::path& path) {
  struct statvfs info{};
  if (::statvfs(path.c_str(), &info) != 0) {
    return error("cannot read writable capacity of " + path.string() + ": " +
                 std::generic_category().message(errno));
  }
  WorkspaceStorageStatus status;
  status.attached = true;
  const auto block = static_cast<std::uint64_t>(info.f_frsize);
  status.writable_capacity_bytes = static_cast<std::uint64_t>(info.f_blocks) * block;
  status.writable_remaining_bytes = static_cast<std::uint64_t>(info.f_bavail) * block;
  return status;
}

OutcomeVoid remove_empty_workspace_path(const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::exists(std::filesystem::symlink_status(path, ec))) return {};
  std::filesystem::remove(path, ec);
  if (ec) return error("cannot remove the Workspace path " + path.string() + ": " + ec.message());
  return {};
}

std::filesystem::path storage_service_socket_path() {
  if (const char* from_env = std::getenv("TRIBIOS_STORAGE_SERVICE_SOCKET");
      from_env != nullptr) {
    return from_env;
  }
  return std::filesystem::path(TRIBIOS_STORAGE_SERVICE_DEFAULT_SOCKET);
}

StorageServiceResult request_privileged_storage_operation(
    const std::vector<std::string>& arguments) {
#ifndef __linux__
  (void)arguments;
  return {false, "privileged Workspace storage operations are available only on Linux"};
#else
  const auto socket_path = storage_service_socket_path();
  const int socket = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (socket < 0) return {false, "cannot create a storage-service connection"};
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const std::string path = socket_path.string();
  if (path.size() >= sizeof(address.sun_path)) {
    ::close(socket);
    return {false, "the storage-service socket path is too long"};
  }
  std::copy(path.begin(), path.end(), address.sun_path);
  if (::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(socket);
    return {false, "cannot reach the Tribios storage service at " + path +
                       "; run `sudo tribios install-privileges` after installing Tribios"};
  }
  auto write_all = [&](const void* data, std::size_t size) {
    const auto* bytes = static_cast<const char*>(data);
    while (size > 0) {
      const ssize_t written = ::write(socket, bytes, size);
      if (written <= 0) return false;
      bytes += written;
      size -= static_cast<std::size_t>(written);
    }
    return true;
  };
  const std::uint32_t count = htonl(static_cast<std::uint32_t>(arguments.size()));
  if (!write_all(&count, sizeof(count))) {
    ::close(socket);
    return {false, "cannot write a storage-service request"};
  }
  for (const auto& argument : arguments) {
    const std::uint32_t length = htonl(static_cast<std::uint32_t>(argument.size()));
    if (!write_all(&length, sizeof(length)) || !write_all(argument.data(), argument.size())) {
      ::close(socket);
      return {false, "cannot write a storage-service request"};
    }
  }
  auto read_all = [&](void* data, std::size_t size) {
    auto* bytes = static_cast<char*>(data);
    while (size > 0) {
      const ssize_t received = ::read(socket, bytes, size);
      if (received <= 0) return false;
      bytes += received;
      size -= static_cast<std::size_t>(received);
    }
    return true;
  };
  std::uint32_t status = 0;
  std::uint32_t message_length = 0;
  if (!read_all(&status, sizeof(status)) || !read_all(&message_length, sizeof(message_length))) {
    ::close(socket);
    return {false, "the storage service closed an incomplete response"};
  }
  status = ntohl(status);
  message_length = ntohl(message_length);
  if (message_length > 1024 * 1024) {
    ::close(socket);
    return {false, "the storage service returned an oversized response"};
  }
  std::string message(message_length, '\0');
  const bool read = read_all(message.data(), message.size());
  ::close(socket);
  if (!read) return {false, "the storage service closed an incomplete response"};
  return {status == 0, std::move(message)};
#endif
}

Outcome<WorkspaceContentsEstimate> estimate_workspace_contents(
    const std::filesystem::path& project_root) {
  struct stat project_st{};
  if (::lstat(project_root.c_str(), &project_st) != 0 || !S_ISDIR(project_st.st_mode)) {
    return error("base capture: " + project_root.string() + " is not a directory");
  }
  WorkspaceContentsEstimate estimate;
  if (auto counted = estimate_directory(project_root, "", project_st.st_dev, estimate); !counted) {
    return std::unexpected(counted.error());
  }
  return estimate;
}

std::uint64_t default_growth_allowance_bytes(std::uint64_t base_state_bytes) {
  return std::max(kMinimumGrowthAllowanceBytes, base_state_bytes * 4);
}

std::vector<StorageCapability> probe_workspace_storage_capabilities(
    const StorageConfiguration& configuration) {
  std::vector<StorageCapability> capabilities;
  for (const auto& backend : backend_preference_order()) {
#ifdef __APPLE__
    if (backend == kApfsShadowBackend) {
      capabilities.push_back(probe_apfs_shadow_capability(configuration));
    }
#endif
#ifdef __linux__
    if (backend == kBtrfsSnapshotBackend) {
      capabilities.push_back(probe_btrfs_snapshot_capability(configuration));
    } else if (backend == kOverlayFsBackend) {
      capabilities.push_back(probe_overlayfs_capability(configuration));
    }
#endif
  }
  return capabilities;
}

Outcome<std::string> choose_supported_backend(const std::vector<StorageCapability>& capabilities) {
  for (const auto& backend : backend_preference_order()) {
    for (const auto& capability : capabilities) {
      if (capability.backend == backend && capability.supported) return capability.backend;
    }
  }
  std::string reasons;
  for (const auto& capability : capabilities) {
    if (!reasons.empty()) reasons += "; ";
    reasons += capability.backend + " is unavailable: " + capability.missing_capability;
  }
  if (reasons.empty()) reasons = "this operating system has no supported Workspace storage backend";
  return error("no supported Workspace storage backend: " + reasons);
}

Outcome<std::unique_ptr<WorkspaceStorage>> open_workspace_storage(
    const std::string& backend, StorageConfiguration configuration) {
#ifdef __APPLE__
  if (backend == kApfsShadowBackend) return make_apfs_shadow_storage(std::move(configuration));
#endif
#ifdef __linux__
  if (backend == kBtrfsSnapshotBackend) {
    return make_btrfs_snapshot_storage(std::move(configuration));
  }
  if (backend == kOverlayFsBackend) return make_overlayfs_storage(std::move(configuration));
#endif
  return error("this build cannot open Workspace storage backend " + backend +
               " on this operating system");
}

}  // namespace tribios
