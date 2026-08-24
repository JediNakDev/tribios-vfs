// Built when no FUSE backend is available. The daemon still serves its control
// interface and the Workspace engine, so Workspace lifecycle and filesystem
// semantics remain testable; only the mounted view is missing.
#include "fuse/fuse_adapter.hpp"

namespace tribios {

bool mount_supported() { return false; }

OutcomeVoid run_project_mount(ProjectManager&, const fs::path&, bool) {
  return error("this build has no FUSE backend: mounting is unavailable");
}

void request_unmount(const fs::path&) {}

}  // namespace tribios
