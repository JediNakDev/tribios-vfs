#include <csignal>

#include <filesystem>
#include <iostream>
#include <string>

#include "core/project_manager.hpp"
#include "daemon/control_server.hpp"

namespace {

tribios::ControlServer* glb_control_server = nullptr;

void stop_daemon_on_signal(int) {
  if (glb_control_server != nullptr) glb_control_server->stop();
}

}  // namespace

int main(int argc, char** argv) {
  std::string project;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--project" && i + 1 < argc) {
      project = argv[++i];
    } else {
      std::cerr << "tribios_daemon: unknown argument " << argument << "\n";
      return 2;
    }
  }
  if (project.empty()) {
    std::cerr << "tribios_daemon: --project is required\n";
    return 2;
  }

  auto manager = tribios::ProjectManager::open_configured_project(project);
  if (!manager) {
    std::cerr << "tribios_daemon: " << manager.error() << "\n";
    return 1;
  }
  const auto paths = (*manager)->project_paths();
  tribios::ControlServer server(**manager, paths.socket);
  glb_control_server = &server;
  std::signal(SIGTERM, stop_daemon_on_signal);
  std::signal(SIGINT, stop_daemon_on_signal);
  // A client that dies between sending its request and reading the reply must
  // not take the daemon down with it.
  std::signal(SIGPIPE, SIG_IGN);

  std::cerr << "tribios_daemon: serving " << paths.root << " on " << paths.socket << "\n";
  auto served = server.serve();
  (*manager)->wait_for_reclamation();
  if (!served) {
    std::cerr << "tribios_daemon: " << served.error() << "\n";
    return 1;
  }
  return 0;
}
