#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "core/error.hpp"
#include "core/project_manager.hpp"

namespace tribios {

// The daemon's Unix-domain control interface, spoken only by the CLI.
class ControlServer {
 public:
  // Told about each Workspace path a request changed without going through the
  // mount, so the mounted view can drop what the kernel cached for it. An empty
  // path is the Workspace root. `name_unlinked` means the path no longer names
  // what it named before the request.
  using ExternalChangeHandler = std::function<void(
      const std::string& workspace, const std::string& relative_path, bool name_unlinked)>;

  ControlServer(ProjectManager& manager, std::filesystem::path socket_path)
      : manager_(manager), socket_path_(std::move(socket_path)) {}

  OutcomeVoid serve();  // runs until a shutdown request arrives
  void stop();
  void remove_socket();

  // Reported by `info` so callers know whether a mounted path exists.
  void set_mount_active(bool active) { mount_active_.store(active); }

  void set_external_change_handler(ExternalChangeHandler handler) {
    external_change_handler_ = std::move(handler);
  }

 private:
  std::vector<std::string> dispatch_control_request(const std::vector<std::string>& request);

  ProjectManager& manager_;
  std::filesystem::path socket_path_;
  std::atomic<bool> running_{true};
  std::atomic<bool> mount_active_{false};
  ExternalChangeHandler external_change_handler_;
  int listen_fd_ = -1;
};

}  // namespace tribios
