#pragma once

#include <filesystem>
#include <string_view>

#include "core/error.hpp"
#include "core/metadata_store.hpp"
#include "core/workspace_storage.hpp"

namespace tribios {

// Terminates the process at a named persistence boundary when deterministic
// fault injection is enabled. The trace is flushed before termination.
void trigger_failpoint(std::string_view name, const std::filesystem::path& tribios_dir,
                       std::string_view context = {});

int sync_file_data(const std::filesystem::path& path);
int sync_directory(const std::filesystem::path& path);
int sync_parent_directory(const std::filesystem::path& path);

// Completes or rolls back every journaled operation before any Workspace is
// exposed by the daemon.
OutcomeVoid recover_interrupted_operations(const std::filesystem::path& project_root,
                                           const std::filesystem::path& tribios_dir,
                                           WorkspaceStorage& storage,
                                           MetadataStore& store);

OutcomeVoid validate_project_storage_invariants(const std::filesystem::path& project_root,
                                                WorkspaceStorage& storage,
                                                MetadataStore& store);

}  // namespace tribios
