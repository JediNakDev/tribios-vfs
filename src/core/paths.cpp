#include "core/paths.hpp"

#include <vector>

namespace tribios {

std::string normalize_relative(std::string_view path) {
  std::vector<std::string> segments;
  std::string segment;
  auto take_segment = [&] {
    if (segment == "..") {
      // The parent of the view root is the view root, as on any filesystem, so
      // a ".." that would climb past it is dropped instead of followed. Without
      // this a caller that reaches the engine without kernel path resolution in
      // front of it, such as the control socket, could name a path outside the
      // Workspace.
      if (!segments.empty()) segments.pop_back();
    } else if (!segment.empty() && segment != ".") {
      segments.push_back(segment);
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

  std::string normalized;
  for (const std::string& taken : segments) {
    if (!normalized.empty()) normalized.push_back('/');
    normalized += taken;
  }
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
