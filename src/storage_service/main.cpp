#ifdef __linux__

#include <arpa/inet.h>
#include <linux/btrfs.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

struct OperationResult {
  bool ok = false;
  std::string message;
};

bool is_descendant(const fs::path& parent, const fs::path& child) {
  auto parent_part = parent.begin();
  auto child_part = child.begin();
  for (; parent_part != parent.end(); ++parent_part, ++child_part) {
    if (child_part == child.end() || *parent_part != *child_part) return false;
  }
  return child_part != child.end();
}

bool validate_owned_project_paths(const fs::path& project_argument,
                                  const fs::path& tribios_argument,
                                  const std::vector<fs::path>& operation_paths,
                                  uid_t caller_uid) {
  std::error_code ec;
  const auto project = fs::canonical(project_argument, ec);
  if (ec) return false;
  const auto tribios = fs::canonical(tribios_argument, ec);
  if (ec || tribios != project / ".tribios") return false;
  struct stat project_status {};
  if (::lstat(project.c_str(), &project_status) != 0 || project_status.st_uid != caller_uid) {
    return false;
  }
  for (const auto& operation_path : operation_paths) {
    const auto resolved = fs::weakly_canonical(operation_path, ec);
    if (ec || !is_descendant(tribios, resolved)) return false;
  }
  return true;
}

bool validate_workspace_target(const fs::path& workspace_root_argument,
                               const fs::path& target_argument, uid_t caller_uid) {
  std::error_code ec;
  const auto workspace_root = fs::canonical(workspace_root_argument, ec);
  if (ec) return false;
  const auto target = fs::weakly_canonical(target_argument, ec);
  if (ec || !is_descendant(workspace_root, target)) return false;
  struct stat root_status {};
  struct stat target_status {};
  return ::lstat(workspace_root.c_str(), &root_status) == 0 &&
         ::lstat(target.c_str(), &target_status) == 0 && root_status.st_uid == caller_uid &&
         target_status.st_uid == caller_uid && S_ISDIR(target_status.st_mode);
}

OperationResult system_error(const std::string& operation) {
  return {false, operation + ": " + std::strerror(errno)};
}

OperationResult delete_btrfs_subvolume(const fs::path& path) {
  const int parent = ::open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
  if (parent < 0) return system_error("cannot open the Btrfs subvolume parent");
  btrfs_ioctl_vol_args arguments{};
  const std::string name = path.filename().string();
  if (name.size() >= sizeof(arguments.name)) {
    ::close(parent);
    return {false, "Btrfs subvolume name is too long"};
  }
  std::memcpy(arguments.name, name.c_str(), name.size() + 1);
  const int deleted = ::ioctl(parent, BTRFS_IOC_SNAP_DESTROY, &arguments);
  const int saved_errno = errno;
  ::close(parent);
  errno = saved_errno;
  if (deleted != 0 && errno != ENOENT) return system_error("cannot delete subvolume");
  return {true, {}};
}

OperationResult perform_operation(const std::vector<std::string>& arguments, uid_t caller_uid) {
  if (arguments.size() < 4) return {false, "invalid storage-service request"};
  const std::string& operation = arguments[0];
  const fs::path project = arguments[1];
  const fs::path tribios = arguments[2];

  if (operation == "unmount" && arguments.size() == 5) {
    const fs::path workspace_root = arguments[3];
    const fs::path target = arguments[4];
    if (!validate_owned_project_paths(project, tribios, {}, caller_uid) ||
        !validate_workspace_target(workspace_root, target, caller_uid)) {
      return {false, "storage-service path validation failed"};
    }
    if (::umount2(target.c_str(), 0) != 0 && errno != EINVAL && errno != ENOENT) {
      return system_error("cannot unmount Workspace");
    }
    return {true, {}};
  }
  if (operation == "btrfs-delete" && arguments.size() == 4) {
    const fs::path target = arguments[3];
    if (!validate_owned_project_paths(project, tribios, {target}, caller_uid)) {
      return {false, "storage-service path validation failed"};
    }
    return delete_btrfs_subvolume(target);
  }
  if (operation == "overlay-mount" && arguments.size() == 8) {
    const fs::path workspace_root = arguments[3];
    const fs::path lower = arguments[4];
    const fs::path upper = arguments[5];
    const fs::path work = arguments[6];
    const fs::path target = arguments[7];
    if (!validate_owned_project_paths(project, tribios, {lower, upper, work}, caller_uid) ||
        !validate_workspace_target(workspace_root, target, caller_uid)) {
      return {false, "storage-service path validation failed"};
    }
    std::error_code ec;
    if (fs::directory_iterator(target, ec) != fs::directory_iterator{} || ec) {
      return {false, "the Workspace mount path is not empty"};
    }
    const std::string options = "lowerdir=" + lower.string() + ",upperdir=" + upper.string() +
                                ",workdir=" + work.string();
    if (::mount("overlay", target.c_str(), "overlay", 0, options.c_str()) != 0) {
      return system_error("cannot mount OverlayFS Workspace");
    }
    return {true, {}};
  }
  return {false, "invalid storage-service request"};
}

bool read_all(int socket, void* data, std::size_t size) {
  auto* bytes = static_cast<char*>(data);
  while (size > 0) {
    const ssize_t received = ::read(socket, bytes, size);
    if (received <= 0) return false;
    bytes += received;
    size -= static_cast<std::size_t>(received);
  }
  return true;
}

bool write_all(int socket, const void* data, std::size_t size) {
  const auto* bytes = static_cast<const char*>(data);
  while (size > 0) {
    const ssize_t written = ::write(socket, bytes, size);
    if (written <= 0) return false;
    bytes += written;
    size -= static_cast<std::size_t>(written);
  }
  return true;
}

std::vector<std::string> read_request(int socket) {
  std::uint32_t count = 0;
  if (!read_all(socket, &count, sizeof(count))) return {};
  count = ntohl(count);
  if (count == 0 || count > 16) return {};
  std::vector<std::string> arguments;
  for (std::uint32_t index = 0; index < count; ++index) {
    std::uint32_t length = 0;
    if (!read_all(socket, &length, sizeof(length))) return {};
    length = ntohl(length);
    if (length > 1024 * 1024) return {};
    std::string argument(length, '\0');
    if (!read_all(socket, argument.data(), argument.size())) return {};
    arguments.push_back(std::move(argument));
  }
  return arguments;
}

void write_response(int socket, const OperationResult& result) {
  const std::uint32_t status = htonl(result.ok ? 0U : 1U);
  const std::uint32_t length = htonl(static_cast<std::uint32_t>(result.message.size()));
  write_all(socket, &status, sizeof(status));
  write_all(socket, &length, sizeof(length));
  write_all(socket, result.message.data(), result.message.size());
}

}  // namespace

int main(int argc, char** argv) {
  fs::path socket_path = "/run/tribios/storage.sock";
  if (argc == 3 && std::string_view(argv[1]) == "--socket") {
    socket_path = argv[2];
  } else if (argc != 1) {
    std::cerr << "usage: tribios_storage_service [--socket <path>]\n";
    return 2;
  }
  if (::geteuid() != 0) {
    std::cerr << "tribios_storage_service must run as root\n";
    return 1;
  }
  std::error_code ec;
  fs::create_directories(socket_path.parent_path(), ec);
  if (ec) {
    std::cerr << "cannot create storage-service socket directory: " << ec.message() << "\n";
    return 1;
  }
  ::unlink(socket_path.c_str());
  const int server = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (server < 0) return 1;
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const std::string path = socket_path.string();
  if (path.size() >= sizeof(address.sun_path)) return 1;
  std::copy(path.begin(), path.end(), address.sun_path);
  if (::bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
      ::chmod(socket_path.c_str(), 0666) != 0 || ::listen(server, 64) != 0) {
    std::cerr << "cannot start the storage service: " << std::strerror(errno) << "\n";
    return 1;
  }
  while (true) {
    const int client = ::accept(server, nullptr, nullptr);
    if (client < 0) continue;
    ucred credentials{};
    socklen_t credentials_size = sizeof(credentials);
    if (::getsockopt(client, SOL_SOCKET, SO_PEERCRED, &credentials, &credentials_size) != 0) {
      write_response(client, {false, "cannot authenticate the storage-service caller"});
      ::close(client);
      continue;
    }
    const auto arguments = read_request(client);
    const auto result = arguments.empty()
                            ? OperationResult{false, "invalid storage-service request"}
                            : perform_operation(arguments, credentials.uid);
    write_response(client, result);
    ::close(client);
  }
}

#endif
