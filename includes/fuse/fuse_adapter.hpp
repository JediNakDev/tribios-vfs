#pragma once

#include <filesystem>

#include "core/error.hpp"
#include "core/project_manager.hpp"

namespace tribios {

// True when this build has a FUSE backend linked in.
bool mount_supported();

// Runs the FUSE event loop for one mounted Project view whose immediate
// children are its visible Workspaces. Returns when the view is unmounted.
OutcomeVoid run_project_mount(ProjectManager& manager, const std::filesystem::path& mount_point,
                              bool debug);

// Asks the kernel to unmount the Project view.
void request_unmount(const std::filesystem::path& mount_point);

}  // namespace tribios
