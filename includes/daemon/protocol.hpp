#pragma once

#include <string>
#include <vector>

namespace tribios {

// The control interface is a newline-delimited, tab-separated field protocol.
// Fields are percent-escaped so payloads may contain tabs and newlines.
std::string encode_message(const std::vector<std::string>& fields);
std::vector<std::string> decode_message(const std::string& line);

}  // namespace tribios
