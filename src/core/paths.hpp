#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace tribios {

namespace fs = std::filesystem;

inline constexpr const char* kTribiosDirName = ".tribios";
inline constexpr const char* kGitDirName = ".git";

// Turns any path into a Project-relative path: no leading slash, no "."
// segments, no trailing slash. The Project view root is the empty string.
std::string normalize_relative(std::string_view path);

std::string parent_of(const std::string& relative);
std::string join_relative(const std::string& parent, const std::string& name);

}  // namespace tribios
