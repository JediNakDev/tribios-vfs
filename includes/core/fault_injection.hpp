#pragma once

#include <cstddef>
#include <string_view>

namespace tribios {

int injected_io_error(std::string_view boundary);
std::size_t injected_short_write_size(std::size_t requested);

}  // namespace tribios
