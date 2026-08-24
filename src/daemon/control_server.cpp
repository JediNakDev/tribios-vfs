#include "daemon/control_server.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <optional>
#include <sstream>

#include "daemon/protocol.hpp"

namespace tribios {
namespace {

using ControlReply = std::vector<std::string>;

ControlReply ok_reply(ControlReply payload = {}) {
  payload.insert(payload.begin(), "OK");
  return payload;
}

ControlReply error_reply(const std::string& message) { return {"ERR", message}; }

ControlReply errno_error_reply(int code) {
  return {"ERR", std::string("errno ") + std::to_string(code) + " " + std::strerror(code)};
}

ControlReply filesystem_status_reply(const Status& status) {
  return status ? ok_reply() : errno_error_reply(status.error());
}

std::string format_file_mode(mode_t mode) {
  std::ostringstream out;
  const char* kind = S_ISDIR(mode) ? "dir" : S_ISLNK(mode) ? "symlink" : "file";
  out << kind << " " << std::oct << (mode & 07777);
  return out.str();
}

std::string request_argument(const std::vector<std::string>& request, std::size_t index) {
  return index < request.size() ? request[index] : std::string{};
}

ControlReply dispatch_workspace_request(ProjectManager& manager, const std::string& verb,
                                        const std::vector<std::string>& request) {
  if (verb == "ws.create") {
    auto created =
        manager.create_workspace(request_argument(request, 1), request_argument(request, 2));
    if (!created) return error_reply(created.error());
    return ok_reply({created->name, created->branch, std::to_string(created->create_us),
                     created->path.string()});
  }
  if (verb == "ws.remove") {
    auto removed = manager.remove_workspace(request_argument(request, 1));
    if (!removed) return error_reply(removed.error());
    return ok_reply({removed->name, std::to_string(removed->logical_remove_us)});
  }
  if (verb == "ws.list") {
    ControlReply payload;
    for (const auto& workspace : manager.workspace_records()) {
      std::ostringstream line;
      line << workspace.name << "\t" << workspace_state_name(workspace.state) << "\t"
           << workspace.branch << "\t" << workspace.create_us << "\t"
           << workspace.logical_remove_us << "\t" << workspace.reclaim_us;
      payload.push_back(line.str());
    }
    return ok_reply(payload);
  }
  if (verb == "ws.wait-reclaim") {
    manager.wait_for_reclamation();
    return ok_reply();
  }
  return error_reply("unknown Workspace request: " + verb);
}

std::optional<ControlReply> dispatch_filesystem_query(WorkspaceEngine& engine,
                                                      const std::string& verb,
                                                      const std::string& path) {
  if (verb == "stats.upper") return ok_reply({std::to_string(engine.upper_bytes())});
  if (verb == "fs.stat") {
    auto attributes = engine.getattr(path);
    if (!attributes) return errno_error_reply(attributes.error());
    return ok_reply({format_file_mode(attributes->mode), std::to_string(attributes->size),
                     attributes->from_upper ? "upper" : "base"});
  }
  if (verb == "fs.ls") {
    auto entries = engine.readdir(path);
    if (!entries) return errno_error_reply(entries.error());
    ControlReply payload;
    for (const auto& entry : *entries) payload.push_back(entry.name);
    return ok_reply(payload);
  }
  if (verb == "fs.read") {
    auto attributes = engine.getattr(path);
    if (!attributes) return errno_error_reply(attributes.error());
    auto data = engine.read_file(path, attributes->size, 0);
    return data ? ok_reply({*data}) : errno_error_reply(data.error());
  }
  if (verb == "fs.readlink") {
    auto target = engine.readlink(path);
    return target ? ok_reply({*target}) : errno_error_reply(target.error());
  }
  return std::nullopt;
}

ControlReply dispatch_filesystem_mutation(WorkspaceEngine& engine, const std::string& verb,
                                          const std::vector<std::string>& request,
                                          const std::string& path) {
  if (verb == "fs.write") {
    const std::string data = request_argument(request, 3);
    const std::uint64_t offset =
        std::strtoull(request_argument(request, 4).c_str(), nullptr, 10);
    auto written = engine.write_file(path, data, offset);
    if (!written) return errno_error_reply(written.error());
    if (offset == 0) {
      auto trimmed = engine.truncate(path, data.size());
      if (!trimmed) return errno_error_reply(trimmed.error());
    }
    return ok_reply({std::to_string(*written)});
  }
  if (verb == "fs.create") return filesystem_status_reply(engine.create(path, 0644));
  if (verb == "fs.mkdir") return filesystem_status_reply(engine.mkdir(path, 0755));
  if (verb == "fs.rm") return filesystem_status_reply(engine.unlink(path));
  if (verb == "fs.rmdir") return filesystem_status_reply(engine.rmdir(path));
  if (verb == "fs.mv") {
    return filesystem_status_reply(engine.rename(path, request_argument(request, 3)));
  }
  if (verb == "fs.symlink") {
    return filesystem_status_reply(engine.symlink(path, request_argument(request, 3)));
  }
  if (verb == "fs.chmod") {
    const auto mode =
        static_cast<mode_t>(std::strtoul(request_argument(request, 3).c_str(), nullptr, 8));
    return filesystem_status_reply(engine.chmod(path, mode));
  }
  if (verb == "fs.truncate") {
    const std::uint64_t size =
        std::strtoull(request_argument(request, 3).c_str(), nullptr, 10);
    return filesystem_status_reply(engine.truncate(path, size));
  }
  if (verb == "fs.hardlink" || verb == "fs.setxattr" || verb == "fs.getxattr" ||
      verb == "fs.lock" || verb == "fs.mknod") {
    return errno_error_reply(ENOTSUP);
  }
  return error_reply("unknown filesystem request: " + verb);
}

ControlReply dispatch_filesystem_request(ProjectManager& manager, const std::string& verb,
                                         const std::vector<std::string>& request) {
  auto engine = manager.find_active_workspace_engine(request_argument(request, 1));
  if (engine == nullptr) {
    return error_reply("no such active Workspace: " + request_argument(request, 1));
  }
  const std::string path = request_argument(request, 2);
  auto query_reply = dispatch_filesystem_query(*engine, verb, path);
  if (query_reply) return *query_reply;
  return dispatch_filesystem_mutation(*engine, verb, request, path);
}

}  // namespace

std::vector<std::string> ControlServer::dispatch_control_request(
    const std::vector<std::string>& request) {
  if (request.empty()) return error_reply("empty request");
  const std::string& verb = request[0];

  if (verb == "ping") return ok_reply({"pong"});

  if (verb == "info") {
    const auto& record = manager_.project_record();
    return ok_reply({record.root, record.mount_point, "git",
                     std::to_string(record.base_entry_count), std::to_string(record.base_bytes),
                     std::to_string(record.base_capture_ms),
                     mount_active_.load() ? "mounted" : "unmounted"});
  }

  if (verb == "shutdown") {
    running_.store(false);
    return ok_reply({"stopping"});
  }

  if (verb.starts_with("ws.")) return dispatch_workspace_request(manager_, verb, request);
  if (verb.starts_with("fs.") || verb == "stats.upper") {
    return dispatch_filesystem_request(manager_, verb, request);
  }

  return error_reply("unknown request: " + verb);
}

OutcomeVoid ControlServer::serve() {
  ::unlink(socket_path_.c_str());
  listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (listen_fd_ < 0) return error("cannot create control socket");

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const std::string path = socket_path_.string();
  if (path.size() >= sizeof(address.sun_path)) return error("control socket path is too long");
  std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
  if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    return error("cannot bind control socket at " + path);
  }
  if (::listen(listen_fd_, 64) != 0) return error("cannot listen on control socket");

  while (running_.load()) {
    const int client = ::accept(listen_fd_, nullptr, nullptr);
    if (client < 0) {
      if (!running_.load()) break;
      continue;
    }
    std::string buffer;
    char chunk[4096];
    ssize_t n = 0;
    while (buffer.find('\n') == std::string::npos &&
           (n = ::read(client, chunk, sizeof(chunk))) > 0) {
      buffer.append(chunk, static_cast<std::size_t>(n));
    }
    const auto reply = encode_message(dispatch_control_request(decode_message(buffer)));
    ssize_t written = ::write(client, reply.data(), reply.size());
    (void)written;
    ::close(client);
  }
  ::close(listen_fd_);
  ::unlink(socket_path_.c_str());
  return {};
}

void ControlServer::stop() {
  running_.store(false);
  if (listen_fd_ >= 0) ::shutdown(listen_fd_, SHUT_RDWR);
}

}  // namespace tribios
