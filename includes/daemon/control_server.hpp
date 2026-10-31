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
  ControlServer(ProjectManager& manager, std::filesystem::path socket_path)
      : manager_(manager), socket_path_(std::move(socket_path)) {}

  OutcomeVoid serve();  // runs until a shutdown request arrives
  void stop();
  void remove_socket();

  // Reported by `info` so callers know whether a mounted path exists.
  void set_mount_active(bool active) { mount_active_.store(active); }

  // Runs on a shutdown request before the server agrees to stop. A failure
  // refuses the shutdown and is reported to the caller, and serving continues.
  void set_shutdown_unmount(std::function<OutcomeVoid()> unmount) {
    shutdown_unmount_ = std::move(unmount);
  }

 private:
  std::vector<std::string> dispatch_control_request(const std::vector<std::string>& request);

  ProjectManager& manager_;
  std::filesystem::path socket_path_;
  std::atomic<bool> running_{true};
  std::atomic<bool> mount_active_{false};
  std::function<OutcomeVoid()> shutdown_unmount_;
  int listen_fd_ = -1;
};

}  // namespace tribios
