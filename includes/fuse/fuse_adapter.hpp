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

// Unmounts the Project view. macOS forces the unmount when it is busy. An
// unprivileged Linux user cannot, so a Linux mount that something holds for
// the whole retry window stays mounted and the reason is returned.
OutcomeVoid unmount_project_view(const std::filesystem::path& mount_point);

}  // namespace tribios
