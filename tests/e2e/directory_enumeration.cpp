#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

struct DirectoryMetadata {
  std::uint64_t inode;
  unsigned char type;

  bool operator==(const DirectoryMetadata&) const = default;
};

struct DirectoryCloser {
  void operator()(DIR* directory) const { ::closedir(directory); }
};

std::map<std::string, DirectoryMetadata> read_directory(const std::filesystem::path& path) {
  std::unique_ptr<DIR, DirectoryCloser> directory(::opendir(path.c_str()));
  if (!directory) throw std::runtime_error("cannot open " + path.string());

  std::map<std::string, DirectoryMetadata> entries;
  while (true) {
    errno = 0;
    const dirent* entry = ::readdir(directory.get());
    if (entry == nullptr) {
      if (errno != 0) throw std::runtime_error("cannot read " + path.string());
      break;
    }
    const std::string name = entry->d_name;
    if (name == "." || name == "..") continue;
    if (entry->d_ino == 0) throw std::runtime_error("zero inode for " + name);
    if (entry->d_type == DT_UNKNOWN) throw std::runtime_error("unknown type for " + name);
    struct stat attributes{};
    if (::fstatat(::dirfd(directory.get()), entry->d_name, &attributes, AT_SYMLINK_NOFOLLOW) != 0) {
      throw std::runtime_error("cannot stat " + name);
    }
    if (entry->d_ino != attributes.st_ino) {
      throw std::runtime_error("directory and stat disagree on the inode for " + name);
    }
    const unsigned char expected_type = S_ISREG(attributes.st_mode) ? DT_REG :
                                       S_ISDIR(attributes.st_mode) ? DT_DIR : DT_LNK;
    if (entry->d_type != expected_type) throw std::runtime_error("wrong type for " + name);
    if (!entries.emplace(name, DirectoryMetadata{entry->d_ino, entry->d_type}).second) {
      throw std::runtime_error("duplicate entry " + name);
    }
  }
  return entries;
}

void require_type(const std::map<std::string, DirectoryMetadata>& entries, const std::string& name,
                  unsigned char expected_type) {
  const auto entry = entries.find(name);
  if (entry == entries.end()) throw std::runtime_error("missing entry " + name);
  if (entry->second.type != expected_type) throw std::runtime_error("wrong type for " + name);
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc != 3) {
      std::cerr << "usage: tribios_directory_enumeration <directory> "
                   "<regular-file-count>\n";
      return 2;
    }
    const std::size_t regular_file_count = std::stoull(argv[2]);
    const auto first_read = read_directory(argv[1]);
    const auto second_read = read_directory(argv[1]);
    if (first_read.size() != regular_file_count + 2) {
      throw std::runtime_error("expected " + std::to_string(regular_file_count + 2) +
                               " entries, got " + std::to_string(first_read.size()));
    }
    if (second_read != first_read) throw std::runtime_error("directory metadata changed");
    require_type(first_read, "entry-0000.txt", DT_REG);
    require_type(first_read, "subdirectory", DT_DIR);
    require_type(first_read, "link-to-subdirectory", DT_LNK);
  } catch (const std::exception& exception) {
    std::cerr << "FAIL: " << exception.what() << "\n";
    return 1;
  }
  std::cout << "PASS directory enumeration\n";
  return 0;
}
