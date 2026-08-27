#include "daemon/control_server.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
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
           << workspace.logical_remove_us << "\t" << workspace.reclaim_us << "\t";
      if (workspace.state == WorkspaceState::Active) {
        auto status = manager.workspace_status(workspace.name);
        line << (status ? std::to_string(status->writable_remaining_bytes) : "unknown");
      } else {
        line << "-";
      }
      payload.push_back(line.str());
    }
    return ok_reply(payload);
  }
  if (verb == "ws.wait-reclaim") {
    manager.wait_for_reclamation();
    return ok_reply();
  }
  if (verb == "ws.status") {
    auto status = manager.workspace_status(request_argument(request, 1));
    if (!status) return error_reply(status.error());
    return ok_reply({status->attached ? "attached" : "detached",
                     std::to_string(status->writable_capacity_bytes),
                     std::to_string(status->writable_remaining_bytes)});
  }
  return error_reply("unknown Workspace request: " + verb);
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
                     std::to_string(record.base_capture_ms), record.storage_backend,
                     std::to_string(record.storage_format_version),
                     std::to_string(record.growth_allowance_bytes)});
  }
  if (verb == "shutdown") {
    running_.store(false);
    return ok_reply({"stopping"});
  }
  if (verb.starts_with("ws.")) return dispatch_workspace_request(manager_, verb, request);
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
    const ssize_t ignored = ::write(client, reply.data(), reply.size());
    (void)ignored;
    ::close(client);
  }
  // Unlinking before the listening socket closes keeps the path from outliving
  // the daemon that owns it. `daemon stop` returns as soon as a connect is
  // refused, so a later unlink could delete a replacement daemon's socket.
  ::unlink(socket_path_.c_str());
  ::close(listen_fd_);
  listen_fd_ = -1;
  return {};
}

void ControlServer::stop() {
  running_.store(false);
  if (listen_fd_ >= 0) ::shutdown(listen_fd_, SHUT_RDWR);
}

}  // namespace tribios
