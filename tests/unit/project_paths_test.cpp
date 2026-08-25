#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include "core/project_manager.hpp"

using tribios::ProjectPaths;

TEST_CASE("Project control sockets stay short when Project and temporary paths are long") {
  const std::filesystem::path project =
      "/Volumes/PortableSSD/tribios-vfs-benchmark/runs/"
      "20260825T072014Z-81184/fixture";

  const ProjectPaths first = ProjectPaths::from_project_root(project);
  const ProjectPaths second = ProjectPaths::from_project_root(project);

  CHECK(first.socket.parent_path() == "/tmp");
  CHECK(first.socket.filename().string().starts_with("tribios-"));
  CHECK(first.socket.extension() == ".sock");
  CHECK(first.socket.string().size() < 100);
  CHECK(first.socket == second.socket);
}
