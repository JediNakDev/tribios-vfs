#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#ifdef __APPLE__
#include <sys/mount.h>
#endif
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

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

extern char** environ;

namespace {

using tribios::error;
namespace fs = std::filesystem;
using tribios::Outcome;
using tribios::OutcomeVoid;

constexpr const char* kUsage =
    R"(tribios - persistent isolated development Workspaces

usage:
  tribios configure <project> [--mount <path>] [--force]
  tribios info [--project <path>]
  tribios daemon start [--project <path>] [--no-mount]
  tribios daemon stop|status [--project <path>]
  tribios workspace create <name> [--branch <branch>] [--project <path>]
  tribios workspace list [--project <path>]
  tribios workspace remove <name> [--project <path>]
  tribios workspace wait-reclaim [--project <path>]
  tribios recovery inspect [--project <path>]
  tribios fs <verb> <workspace> <args...> [--project <path>]
  tribios version

`tribios fs` drives the Workspace engine directly. On macOS and Linux the same
behavior is reachable through the mounted Workspace path; the verbs exist so
the test and benchmark harness can exercise identical semantics without a FUSE
backend.
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
#ifdef __APPLE__
  std::uint32_t size = sizeof(buffer);
  if (_NSGetExecutablePath(buffer, &size) == 0) {
    return std::filesystem::weakly_canonical(std::filesystem::path(buffer)).parent_path();
  }
#else
  const ssize_t n = ::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
  if (n > 0) {
    buffer[n] = '\0';
    return std::filesystem::path(buffer).parent_path();
  }
#endif
  return std::filesystem::absolute(std::filesystem::path(argv0)).parent_path();
}

// The daemon sits beside the CLI in a build tree and under libexec in an
// installed tree. Both are resolved relative to the running executable so an
// installation stays relocatable, which Homebrew's cellar and DESTDIR staging
// both require.
std::filesystem::path resolve_daemon_path(const char* argv0) {
  if (const char* from_env = std::getenv("TRIBIOS_DAEMON"); from_env != nullptr) {
    return from_env;
  }
  const auto directory = executable_directory(argv0);
  const auto beside_cli = directory / "tribios_daemon";
  if (std::filesystem::exists(beside_cli)) return beside_cli;
  const std::filesystem::path libexec_directory{TRIBIOS_DAEMON_LIBEXEC_DIR};
  const auto installed = libexec_directory.is_absolute()
                             ? libexec_directory / "tribios_daemon"
                             : directory.parent_path() / libexec_directory / "tribios_daemon";
  if (std::filesystem::exists(installed)) return installed;
  return beside_cli;
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

  const auto daemon_path = resolve_daemon_path(argv0);
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

int command_recovery(const Options& options) {
  if (options.positional.size() < 2 || options.positional[1] != "inspect") {
    return report_error("recovery needs inspect");
  }
  auto project = resolve_configured_project_root(options);
  if (!project) return report_error(project.error());
  const auto paths = tribios::ProjectPaths::from_project_root(*project);
  auto store = tribios::MetadataStore::open_database_read_only(paths.database);
  if (!store) return report_error(store.error());
  auto operations = (*store)->load_recovery_operations();
  if (!operations) return report_error(operations.error());
  auto diagnostics = (*store)->load_recovery_diagnostics();
  if (!diagnostics) return report_error(diagnostics.error());

  std::cout << "metadata format: 1\n";
  std::cout << "pending operations: " << operations->size() << "\n";
  for (const auto& operation : *operations) {
    std::cout << "operation " << operation.id << " Workspace " << operation.workspace << " "
              << operation.kind << " " << operation.path;
    if (!operation.target.empty()) std::cout << " -> " << operation.target;
    std::cout << "\n";
  }
  std::cout << "recovery diagnostics: " << diagnostics->size() << "\n";
  for (const auto& diagnostic : *diagnostics) {
    std::cout << "R" << diagnostic.id << " " << diagnostic.message << "\n";
  }
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
  if (command == "recovery") return command_recovery(options);
  if (command == "info") return command_info(options);
  if (command == "fs") return command_fs(options);
  if (command == "upper-bytes") return command_upper_bytes(options);
  if (command == "version" || command == "--version") {
    std::cout << "tribios " << TRIBIOS_VERSION << "\n";
    return 0;
  }
  if (command == "help" || command == "--help") {
    std::cout << kUsage;
    return 0;
  }
  std::cerr << kUsage;
  return 2;
}
