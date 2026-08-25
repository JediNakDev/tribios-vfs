#include "core/fault_injection.hpp"

#include <cerrno>
#include <cstdlib>
#include <string>

namespace tribios {

int injected_io_error(std::string_view boundary) {
  const char* configured = std::getenv("TRIBIOS_IO_FAULT");
  if (configured == nullptr) return 0;
  const std::string value = configured;
  const std::size_t separator = value.find('=');
  if (value.substr(0, separator) != boundary) return 0;
  if (separator == std::string::npos) return ENOSPC;
  const int error_number = std::atoi(value.c_str() + separator + 1);
  return error_number > 0 ? error_number : ENOSPC;
}

std::size_t injected_short_write_size(std::size_t requested) {
  const char* configured = std::getenv("TRIBIOS_SHORT_WRITE_BYTES");
  if (configured == nullptr) return requested;
  const unsigned long long limit = std::strtoull(configured, nullptr, 10);
  return limit < requested ? static_cast<std::size_t>(limit) : requested;
}

}  // namespace tribios
