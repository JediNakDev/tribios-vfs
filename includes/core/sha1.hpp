#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace tribios {

// Git seals its index file with a SHA-1 of the preceding bytes. This is that
// checksum, not a security boundary, so it has no collision detection.
std::array<std::uint8_t, 20> sha1_digest(std::string_view data);

}  // namespace tribios
