#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace tribios {

// The control interface is a newline-delimited, tab-separated field protocol.
// Fields are percent-escaped so payloads may contain tabs and newlines.
std::string encode_message(const std::vector<std::string>& fields);
std::vector<std::string> decode_message(const std::string& line);

// Sends all of `message` on a connected socket. A peer that has already closed
// the connection yields false instead of SIGPIPE, so neither the CLI nor the
// daemon dies when the other side goes away mid-request.
bool send_whole_message(int socket_fd, std::string_view message);

}  // namespace tribios
