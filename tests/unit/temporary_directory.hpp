#pragma once

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

// A uniquely named directory under the system temporary directory, removed with
// everything below it when the test scope ends. Shared by the tests that need
// real files on disk: Base capture and tombstone resolution.
class TemporaryDirectory {
 public:
  explicit TemporaryDirectory(std::string_view label) {
    std::string name_template =
        (std::filesystem::temp_directory_path() / (std::string(label) + ".XXXXXX")).string();
    if (::mkdtemp(name_template.data()) == nullptr) {
      throw std::runtime_error("cannot create a temporary directory for " + std::string(label));
    }
    path_ = name_template;
  }

  ~TemporaryDirectory() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  TemporaryDirectory(const TemporaryDirectory&) = delete;
  TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

inline void write_file_creating_parents(const std::filesystem::path& path,
                                        std::string_view contents) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << contents;
  if (!out) throw std::runtime_error("cannot write " + path.string());
}

inline std::string read_whole_file(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
