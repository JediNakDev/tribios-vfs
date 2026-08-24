#include "daemon/control_client.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>
#include <filesystem>

#include "daemon/protocol.hpp"

namespace tribios {

Outcome<std::vector<std::string>> control_request(const std::filesystem::path& socket_path,
                                                  const std::vector<std::string>& request) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return error("cannot create control socket");
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const std::string path = socket_path.string();
  if (path.size() >= sizeof(address.sun_path)) {
    ::close(fd);
    return error("control socket path is too long");
  }
  std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(fd);
    return error("no daemon is listening at " + path);
  }

  const std::string message = encode_message(request);
  if (::write(fd, message.data(), message.size()) < 0) {
    ::close(fd);
    return error("cannot send control request");
  }
  ::shutdown(fd, SHUT_WR);

  std::string buffer;
  char chunk[4096];
  ssize_t n = 0;
  while ((n = ::read(fd, chunk, sizeof(chunk))) > 0) {
    buffer.append(chunk, static_cast<std::size_t>(n));
  }
  ::close(fd);
  auto fields = decode_message(buffer);
  if (fields.empty()) return error("empty reply from daemon");
  if (fields[0] == "ERR") {
    return error(fields.size() > 1 ? fields[1] : "unknown daemon error");
  }
  fields.erase(fields.begin());
  return fields;
}

}  // namespace tribios
