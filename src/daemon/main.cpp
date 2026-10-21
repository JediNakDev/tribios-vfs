#include <sys/stat.h>
#ifdef __APPLE__
#include <sys/mount.h>
#endif

#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

#include "core/project_manager.hpp"
#include "daemon/control_server.hpp"
#include "fuse/fuse_adapter.hpp"

namespace {

tribios::ControlServer* glb_control_server = nullptr;
std::filesystem::path glb_mount_point;

bool wait_for_mount(const std::filesystem::path& mount_point,
                    std::chrono::seconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
#ifdef __APPLE__
  struct statfs mount_info {};
#else
  struct stat mount_stat {};
  struct stat parent_stat {};
#endif
  while (std::chrono::steady_clock::now() < deadline) {
#ifdef __APPLE__
    if (::statfs(mount_point.c_str(), &mount_info) == 0 &&
        std::string_view(mount_info.f_fstypename) == "macfuse") {
      return true;
    }
#else
    if (::stat(mount_point.c_str(), &mount_stat) == 0 &&
        ::stat(mount_point.parent_path().c_str(), &parent_stat) == 0 &&
        mount_stat.st_dev != parent_stat.st_dev) {
      return true;
    }
#endif
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

bool mount_is_active(const std::filesystem::path& mount_point) {
#ifdef __APPLE__
  struct statfs mount_info {};
  return ::statfs(mount_point.c_str(), &mount_info) == 0 &&
         std::string_view(mount_info.f_fstypename) == "macfuse";
#else
  struct stat mount_stat {};
  struct stat parent_stat {};
  return ::stat(mount_point.c_str(), &mount_stat) == 0 &&
         ::stat(mount_point.parent_path().c_str(), &parent_stat) == 0 &&
         mount_stat.st_dev != parent_stat.st_dev;
#endif
}

// Unmounting runs a helper process and may wait for a busy mount, so it is
// left to main once the server stops.
void stop_daemon_on_signal(int) {
  if (glb_control_server != nullptr) glb_control_server->stop();
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

  const auto startup_paths = tribios::ProjectPaths::from_project_root(project);
  if (mount && mount_is_active(startup_paths.mount_point)) {
    if (auto unmounted = tribios::unmount_project_view(startup_paths.mount_point); !unmounted) {
      std::cerr << "tribios_daemon: cannot remove the stale Project mount before recovery: "
                << unmounted.error() << "\n";
      return 1;
    }
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
  if (mount && tribios::mount_supported()) {
    glb_mount_point = paths.mount_point;
    server.set_shutdown_unmount([&]() -> tribios::OutcomeVoid {
      if (!mount_is_active(paths.mount_point)) return {};
      return tribios::unmount_project_view(paths.mount_point);
    });
    mount_thread = std::thread([&] {
      auto mounted = tribios::run_project_mount(**manager, paths.mount_point, debug);
      if (!mounted) std::cerr << "tribios_daemon: " << mounted.error() << "\n";
    });
    const bool mounted = wait_for_mount(paths.mount_point, std::chrono::seconds(10));
    server.set_mount_active(mounted);
    if (!mounted) {
      std::cerr << "tribios_daemon: mount did not become ready within the timeout\n";
    }
  } else if (mount) {
    std::cerr << "tribios_daemon: " << tribios::mount_unavailable_reason()
              << "; serving control only\n";
  }

  std::cerr << "tribios_daemon: serving " << paths.root << " on " << paths.socket << "\n";
  auto served = server.serve();
  // A shutdown request has already unmounted. A signal stops the server
  // without that step, and no caller is left to refuse, so wait until the
  // mount can be released.
  while (!glb_mount_point.empty() && mount_is_active(glb_mount_point)) {
    auto unmounted = tribios::unmount_project_view(glb_mount_point);
    if (unmounted) break;
    std::cerr << "tribios_daemon: " << unmounted.error() << "\n";
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  if (mount_thread.joinable()) mount_thread.join();
  // Workspace data and metadata are preserved across a clean shutdown.
  (*manager)->wait_for_reclamation();
  // Socket disappearance is the CLI's clean-shutdown completion signal.
  // Keep it present until reclamation releases every SQLite transaction.
  server.remove_socket();
  if (!served) {
    std::cerr << "tribios_daemon: " << served.error() << "\n";
    return 1;
  }
  return 0;
}
