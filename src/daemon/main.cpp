#include <csignal>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "core/project_manager.hpp"
#include "daemon/control_server.hpp"
#include "fuse/fuse_adapter.hpp"

namespace {

tribios::ControlServer* glb_control_server = nullptr;
tribios::fs::path glb_mount_point;

void stop_daemon_on_signal(int) {
  if (glb_control_server != nullptr) glb_control_server->stop();
  if (!glb_mount_point.empty()) tribios::request_unmount(glb_mount_point);
}

}  // namespace

int main(int argc, char** argv) {
  std::string project;
  bool mount = true;
  bool debug = false;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--project" && i + 1 < argc) {
      project = argv[++i];
    } else if (argument == "--no-mount") {
      mount = false;
    } else if (argument == "--debug") {
      debug = true;
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

  std::thread mount_thread;
  server.set_mount_active(mount && tribios::mount_supported());
  if (mount && tribios::mount_supported()) {
    glb_mount_point = paths.mount_point;
    mount_thread = std::thread([&] {
      auto mounted = tribios::run_project_mount(**manager, paths.mount_point, debug);
      if (!mounted) std::cerr << "tribios_daemon: " << mounted.error() << "\n";
    });
  } else if (mount) {
    std::cerr << "tribios_daemon: no FUSE backend in this build, serving control only\n";
  }

  std::cerr << "tribios_daemon: serving " << paths.root << " on " << paths.socket << "\n";
  auto served = server.serve();
  if (!glb_mount_point.empty()) tribios::request_unmount(glb_mount_point);
  if (mount_thread.joinable()) mount_thread.join();
  // Workspace data and metadata are preserved across a clean shutdown.
  (*manager)->wait_for_reclamation();
  if (!served) {
    std::cerr << "tribios_daemon: " << served.error() << "\n";
    return 1;
  }
  return 0;
}
