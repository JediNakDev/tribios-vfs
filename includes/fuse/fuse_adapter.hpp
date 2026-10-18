#pragma once

#include <filesystem>
#include <string>

#include "core/error.hpp"
#include "core/project_manager.hpp"

namespace tribios {

// True when this build has a FUSE backend linked in.
bool mount_supported();

// Explains how to make mounting available when mount_supported() is false.
std::string mount_unavailable_reason();

// Runs the FUSE event loop for one mounted Project view whose immediate
// children are its visible Workspaces. Returns when the view is unmounted.
OutcomeVoid run_project_mount(ProjectManager& manager, const std::filesystem::path& mount_point,
                              bool debug);

// Asks the kernel to unmount the Project view.
void request_unmount(const std::filesystem::path& mount_point);

// Drops what the kernel cached for a Workspace path that changed without going
// through the mount. An empty path is the Workspace root. `name_unlinked` means
// the path no longer names what it named before, so its directory entry goes
// too. Does nothing on a mount that relies on short cache timeouts instead.
void forget_kernel_cache_for_workspace_path(const std::string& workspace,
                                            const std::string& relative_path, bool name_unlinked);

}  // namespace tribios
