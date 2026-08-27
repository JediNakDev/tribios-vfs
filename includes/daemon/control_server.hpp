#pragma once

#include <atomic>
#include <filesystem>
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

 private:
  std::vector<std::string> dispatch_control_request(const std::vector<std::string>& request);

  ProjectManager& manager_;
  std::filesystem::path socket_path_;
  std::atomic<bool> running_{true};
  int listen_fd_ = -1;
};

}  // namespace tribios
