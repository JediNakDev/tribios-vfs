#include <fcntl.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <sys/mount.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/base_capture.hpp"
#include "core/paths.hpp"
#include "core/project_manager.hpp"
#include "daemon/control_client.hpp"

extern char** environ;

namespace {

using tribios::error;
namespace fs = std::filesystem;
using tribios::Outcome;
using tribios::OutcomeVoid;

constexpr const char* kUsage =
    R"(tribios - THROWAWAY PROTOTYPE (see docs/prototype/README.md)

usage:
  tribios configure <project> [--mount <path>] [--force]
  tribios info [--project <path>]
  tribios daemon start [--project <path>] [--no-mount]
  tribios daemon stop|status [--project <path>]
  tribios workspace create <name> [--branch <branch>] [--project <path>]
  tribios workspace list [--project <path>]
  tribios workspace remove <name> [--project <path>]
  tribios workspace wait-reclaim [--project <path>]
  tribios fs <verb> <workspace> <args...> [--project <path>]

`tribios fs` drives the Workspace engine directly. On macOS the same behavior is
reachable through the mounted Workspace path; the verbs exist so the test and
benchmark harness can exercise identical semantics without a FUSE backend.
)";

struct Options {
  std::vector<std::string> positional;
  std::string project;
  std::string mount;
  std::string branch;
  bool force = false;
  bool no_mount = false;
};

Options parse_command_line(int argc, char** argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--project" && i + 1 < argc) {
      options.project = argv[++i];
    } else if (argument == "--mount" && i + 1 < argc) {
      options.mount = argv[++i];
    } else if (argument == "--branch" && i + 1 < argc) {
      options.branch = argv[++i];
    } else if (argument == "--force") {
      options.force = true;
    } else if (argument == "--no-mount") {
      options.no_mount = true;
    } else {
      options.positional.push_back(argument);
    }
  }
  return options;
}

// An explicit flag, TRIBIOS_PROJECT, or the nearest configured ancestor.
Outcome<std::filesystem::path> resolve_configured_project_root(const Options& options) {
  if (!options.project.empty()) return std::filesystem::absolute(options.project);
  if (const char* from_env = std::getenv("TRIBIOS_PROJECT"); from_env != nullptr) {
    return std::filesystem::absolute(from_env);
  }
  std::error_code ec;
  for (std::filesystem::path current = std::filesystem::current_path(ec); !current.empty();
       current = current.parent_path()) {
    if (std::filesystem::exists(current / tribios::kTribiosDirName / "meta.db", ec)) return current;
    if (current == current.root_path()) break;
  }
  return error("no configured project found: pass --project or run `tribios configure`");
}

std::filesystem::path executable_directory(const char* argv0) {
  char buffer[4096];
  std::uint32_t size = sizeof(buffer);
  if (_NSGetExecutablePath(buffer, &size) == 0) {
    return std::filesystem::weakly_canonical(std::filesystem::path(buffer)).parent_path();
  }
  return std::filesystem::absolute(std::filesystem::path(argv0)).parent_path();
}

int report_error(const std::string& message) {
  std::cerr << "tribios: " << message << "\n";
  return 1;
}

OutcomeVoid wait_for_socket(const std::filesystem::path& socket_path,
                            std::chrono::seconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    auto pinged = tribios::control_request(socket_path, {"ping"});
    if (pinged) return {};
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return error("the daemon did not start within the timeout");
}

OutcomeVoid wait_for_socket_to_close(const std::filesystem::path& socket_path,
                                     std::chrono::seconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (!tribios::control_request(socket_path, {"ping"})) return {};
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return error("the daemon did not stop within the timeout");
}

bool mount_is_active(const std::filesystem::path& mount_point) {
  struct statfs mount_info {};
  return ::statfs(mount_point.c_str(), &mount_info) == 0 &&
         std::string_view(mount_info.f_fstypename) == "macfuse";
}

OutcomeVoid wait_for_mount_to_close(const std::filesystem::path& mount_point,
                                    std::chrono::seconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (!mount_is_active(mount_point)) return {};
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return error("the mount did not stop within the timeout");
}

int command_configure(const Options& options) {
  if (options.positional.size() < 2) return report_error("configure needs a project directory");
  const std::filesystem::path root = std::filesystem::absolute(options.positional[1]);
  auto captured = tribios::ProjectManager::configure(root, options.mount, options.force);
  if (!captured) return report_error(captured.error());

  const auto paths = tribios::ProjectPaths::from_project_root(root);
  std::cout << "configured " << paths.root << "\n"
            << "base state: " << captured->entry_count << " entries, " << captured->bytes
            << " bytes, captured in " << captured->duration_ms << " ms\n"
            << "mount point: "
            << (options.mount.empty() ? paths.mount_point.string()
                                      : std::filesystem::absolute(options.mount).string())
            << "\n";
  std::cerr << tribios::kSecretsWarning << "\n";
  return 0;
}

int command_daemon(const Options& options, const char* argv0) {
  if (options.positional.size() < 2) return report_error("daemon needs start, stop or status");
  const std::string action = options.positional[1];
  auto project = resolve_configured_project_root(options);
  if (!project) return report_error(project.error());
  const auto paths = tribios::ProjectPaths::from_project_root(*project);

  if (action == "status") {
    auto pinged = tribios::control_request(paths.socket, {"ping"});
    std::cout << (pinged ? "running" : "stopped") << "\n";
    return pinged ? 0 : 1;
  }
  if (action == "stop") {
    auto stopped = tribios::control_request(paths.socket, {"shutdown"});
    if (!stopped) return report_error(stopped.error());
    auto closed = wait_for_socket_to_close(paths.socket, std::chrono::seconds(15));
    if (!closed) return report_error(closed.error());
    auto unmounted = wait_for_mount_to_close(paths.mount_point, std::chrono::seconds(15));
    if (!unmounted) return report_error(unmounted.error());
    std::cout << "stopped\n";
    return 0;
  }
  if (action != "start") return report_error("unknown daemon action: " + action);

  if (tribios::control_request(paths.socket, {"ping"})) {
    std::cout << "already running\n";
    return 0;
  }

  std::filesystem::path daemon_path;
  if (const char* from_env = std::getenv("TRIBIOS_DAEMON"); from_env != nullptr) {
    daemon_path = from_env;
  } else {
    daemon_path = executable_directory(argv0) / "tribios_daemon";
  }
  std::vector<std::string> arguments{daemon_path.string(), "--project", paths.root.string()};
  if (options.no_mount) arguments.push_back("--no-mount");
  std::vector<char*> raw;
  for (auto& argument : arguments) raw.push_back(const_cast<char*>(argument.c_str()));
  raw.push_back(nullptr);

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, paths.log.c_str(),
                                   O_WRONLY | O_CREAT | O_APPEND, 0644);
  posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
  pid_t pid = 0;
  const int spawned =
      posix_spawn(&pid, daemon_path.c_str(), &actions, nullptr, raw.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (spawned != 0) return report_error("cannot start " + daemon_path.string());

  auto ready = wait_for_socket(paths.socket, std::chrono::seconds(15));
  if (!ready) return report_error(ready.error() + "; see " + paths.log.string());
  std::cout << "started (pid " << pid << ")\n";
  return 0;
}

int command_workspace(const Options& options) {
  if (options.positional.size() < 2) {
    return report_error("workspace needs create, list or remove");
  }
  auto project = resolve_configured_project_root(options);
  if (!project) return report_error(project.error());
  const auto paths = tribios::ProjectPaths::from_project_root(*project);
  const std::string action = options.positional[1];

  if (action == "create") {
    if (options.positional.size() < 3) return report_error("workspace create needs a name");
    auto created = tribios::control_request(paths.socket,
                                            {"ws.create", options.positional[2], options.branch});
    if (!created) return report_error(created.error());
    std::cout << "created " << created->at(0) << " on branch " << created->at(1) << " in "
              << created->at(2) << " us at " << created->at(3) << "\n";
    return 0;
  }
  if (action == "remove") {
    if (options.positional.size() < 3) return report_error("workspace remove needs a name");
    auto removed = tribios::control_request(paths.socket, {"ws.remove", options.positional[2]});
    if (!removed) return report_error(removed.error());
    std::cout << "removed " << removed->at(0) << " logically in " << removed->at(1)
              << " us; physical reclamation continues in the background\n";
    return 0;
  }
  if (action == "wait-reclaim") {
    auto waited = tribios::control_request(paths.socket, {"ws.wait-reclaim"});
    if (!waited) return report_error(waited.error());
    return 0;
  }
  if (action == "list") {
    auto listed = tribios::control_request(paths.socket, {"ws.list"});
    if (!listed) return report_error(listed.error());
    std::cout << "NAME\tSTATE\tBRANCH\tCREATE_US\tLOGICAL_REMOVE_US\tRECLAIM_US\n";
    for (const auto& line : *listed) std::cout << line << "\n";
    return 0;
  }
  return report_error("unknown workspace action: " + action);
}

int command_info(const Options& options) {
  auto project = resolve_configured_project_root(options);
  if (!project) return report_error(project.error());
  const auto paths = tribios::ProjectPaths::from_project_root(*project);
  auto info = tribios::control_request(paths.socket, {"info"});
  if (!info) {
    // The daemon may be stopped; the metadata store still answers.
    auto manager = tribios::ProjectManager::open_configured_project(*project);
    if (!manager) return report_error(manager.error());
    const auto& record = (*manager)->project_record();
    std::cout << "root: " << record.root << "\nmount: " << record.mount_point
              << "\nkind: git\nbase entries: " << record.base_entry_count
              << "\nbase bytes: " << record.base_bytes
              << "\nmount backend: unknown\ndaemon: stopped\n";
    return 0;
  }
  std::cout << "root: " << info->at(0) << "\nmount: " << info->at(1) << "\nkind: " << info->at(2)
            << "\nbase entries: " << info->at(3) << "\nbase bytes: " << info->at(4)
            << "\nbase capture ms: " << info->at(5) << "\nmount backend: " << info->at(6)
            << "\ndaemon: running\n";
  return 0;
}

int command_fs(const Options& options) {
  if (options.positional.size() < 3) return report_error("fs needs a verb and a workspace");
  auto project = resolve_configured_project_root(options);
  if (!project) return report_error(project.error());
  const auto paths = tribios::ProjectPaths::from_project_root(*project);
  std::vector<std::string> request{"fs." + options.positional[1]};
  for (std::size_t i = 2; i < options.positional.size(); ++i) {
    request.push_back(options.positional[i]);
  }
  auto replied = tribios::control_request(paths.socket, request);
  if (!replied) return report_error(replied.error());
  for (const auto& field : *replied) std::cout << field << "\n";
  return 0;
}

int command_upper_bytes(const Options& options) {
  if (options.positional.size() < 2) return report_error("upper-bytes needs a workspace");
  auto project = resolve_configured_project_root(options);
  if (!project) return report_error(project.error());
  const auto paths = tribios::ProjectPaths::from_project_root(*project);
  auto replied = tribios::control_request(paths.socket, {"stats.upper", options.positional[1]});
  if (!replied) return report_error(replied.error());
  std::cout << replied->at(0) << "\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const Options options = parse_command_line(argc, argv);
  if (options.positional.empty()) {
    std::cout << kUsage;
    return 2;
  }
  const std::string& command = options.positional[0];
  if (command == "configure") return command_configure(options);
  if (command == "daemon") return command_daemon(options, argv[0]);
  if (command == "workspace") return command_workspace(options);
  if (command == "info") return command_info(options);
  if (command == "fs") return command_fs(options);
  if (command == "upper-bytes") return command_upper_bytes(options);
  if (command == "help" || command == "--help") {
    std::cout << kUsage;
    return 0;
  }
  std::cerr << kUsage;
  return 2;
}
