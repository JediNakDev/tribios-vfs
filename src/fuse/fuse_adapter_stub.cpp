// Built when no FUSE backend is available. The daemon still serves its control
// interface and the Workspace engine, so Workspace lifecycle and filesystem
// semantics remain testable; only the mounted view is missing.
#include <filesystem>

#include "fuse/fuse_adapter.hpp"

namespace tribios {

bool mount_supported() { return false; }

std::string mount_unavailable_reason() {
#ifdef __APPLE__
  return "this build has no macFUSE backend; install macFUSE and rebuild Tribios";
#elif defined(__linux__)
  return "this build has no libfuse3 backend; install libfuse3-dev and fuse3, then rebuild Tribios";
#else
  return "this build has no FUSE backend for this operating system";
#endif
}

OutcomeVoid run_project_mount(ProjectManager&, const std::filesystem::path&, bool) {
  return error(mount_unavailable_reason());
}

void request_unmount(const std::filesystem::path&) {}

}  // namespace tribios
