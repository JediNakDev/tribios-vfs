#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>
#include <vector>

#include "core/metadata_store.hpp"
#include "core/workspace_storage.hpp"
#include "temporary_directory.hpp"

using tribios::MetadataStore;
using tribios::ProjectRecord;
using tribios::StorageCapability;
using tribios::WorkspaceRecord;
using tribios::WorkspaceState;
using tribios::choose_supported_backend;

TEST_CASE("backend selection follows the fixed platform preference order") {
#ifdef __APPLE__
  std::vector<StorageCapability> capabilities{
      {tribios::kApfsShadowBackend, true, ""},
  };
  REQUIRE(choose_supported_backend(capabilities) == tribios::kApfsShadowBackend);
#elif defined(__linux__)
  std::vector<StorageCapability> capabilities{
      {tribios::kOverlayFsBackend, true, ""},
      {tribios::kBtrfsSnapshotBackend, true, ""},
  };
  REQUIRE(choose_supported_backend(capabilities) == tribios::kBtrfsSnapshotBackend);
  capabilities[1] = {tribios::kBtrfsSnapshotBackend, false, "snapshot deletion is unavailable"};
  REQUIRE(choose_supported_backend(capabilities) == tribios::kOverlayFsBackend);
#endif
}

TEST_CASE("backend selection reports every missing capability") {
#ifdef __APPLE__
  const std::vector<StorageCapability> capabilities{
      {tribios::kApfsShadowBackend, false, "hdiutil cannot attach sparse images"},
  };
#else
  const std::vector<StorageCapability> capabilities{
      {tribios::kBtrfsSnapshotBackend, false, "snapshot deletion is unavailable"},
      {tribios::kOverlayFsBackend, false, "host-namespace mounting is unavailable"},
  };
#endif
  auto selected = choose_supported_backend(capabilities);
  REQUIRE_FALSE(selected.has_value());
  for (const auto& capability : capabilities) {
    REQUIRE(selected.error().find(capability.backend) != std::string::npos);
    REQUIRE(selected.error().find(capability.missing_capability) != std::string::npos);
  }
}

TEST_CASE("storage backend and recovery locator survive a metadata store reopen") {
  TemporaryDirectory root("workspace-storage");
  const auto database = root.path() / "meta.db";
  {
    auto store = MetadataStore::open_database(database);
    REQUIRE(store.has_value());
    ProjectRecord project;
    project.root = root.path().string();
    project.mount_point = (root.path() / "workspaces").string();
    project.storage_backend = tribios::kApfsShadowBackend;
    project.storage_format_version = tribios::kStorageFormatVersion;
    project.growth_allowance_bytes = 32ULL * 1024 * 1024 * 1024;
    REQUIRE((*store)->save_project_record(project).has_value());

    WorkspaceRecord workspace;
    workspace.name = "agent-one";
    workspace.branch = "feature";
    workspace.state = WorkspaceState::Active;
    workspace.storage_locator = "shadows/agent-one.shadow";
    REQUIRE((*store)->save_workspace_record(workspace).has_value());
  }

  auto reopened = MetadataStore::open_database(database);
  REQUIRE(reopened.has_value());
  const auto project = (*reopened)->load_project_record();
  REQUIRE(project.has_value());
  REQUIRE(project->storage_backend == tribios::kApfsShadowBackend);
  REQUIRE(project->storage_format_version == tribios::kStorageFormatVersion);
  REQUIRE(project->growth_allowance_bytes == 32ULL * 1024 * 1024 * 1024);
  const auto workspace = (*reopened)->load_workspace_record("agent-one");
  REQUIRE(workspace.has_value());
  REQUIRE(workspace->storage_locator == "shadows/agent-one.shadow");
}
