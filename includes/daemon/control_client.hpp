#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/error.hpp"

namespace tribios {

// Sends one request to a running daemon and returns its decoded reply fields.
Outcome<std::vector<std::string>> control_request(const std::filesystem::path& socket_path,
                                                  const std::vector<std::string>& request);

}  // namespace tribios
