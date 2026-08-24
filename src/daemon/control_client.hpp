#pragma once

#include <string>
#include <vector>

#include "core/error.hpp"
#include "core/paths.hpp"

namespace tribios {

// Sends one request to a running daemon and returns its decoded reply fields.
Outcome<std::vector<std::string>> control_request(const fs::path& socket_path,
                                                  const std::vector<std::string>& request);

}  // namespace tribios
