#pragma once

#include <cerrno>
#include <expected>
#include <string>

namespace tribios {

// Internal filesystem helpers return errno-compatible status values.
template <typename T>
using Result = std::expected<T, int>;
using Status = std::expected<void, int>;

inline std::unexpected<int> fail(int code) { return std::unexpected(code); }

// Control-plane results carry a message for the CLI.
template <typename T>
using Outcome = std::expected<T, std::string>;
using OutcomeVoid = std::expected<void, std::string>;

inline std::unexpected<std::string> error(std::string message) {
  return std::unexpected(std::move(message));
}

}  // namespace tribios
