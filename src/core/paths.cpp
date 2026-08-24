#include "core/paths.hpp"

namespace tribios {

std::string normalize_relative(std::string_view path) {
  std::string normalized;
  std::string segment;
  auto take_segment = [&] {
    if (!segment.empty() && segment != ".") {
      if (!normalized.empty()) normalized.push_back('/');
      normalized += segment;
    }
    segment.clear();
  };
  for (char c : path) {
    if (c == '/') {
      take_segment();
    } else {
      segment.push_back(c);
    }
  }
  take_segment();
  return normalized;
}

std::string parent_of(const std::string& relative) {
  const auto slash = relative.rfind('/');
  return slash == std::string::npos ? std::string{} : relative.substr(0, slash);
}

std::string join_relative(const std::string& parent, const std::string& name) {
  return parent.empty() ? name : parent + "/" + name;
}

}  // namespace tribios
