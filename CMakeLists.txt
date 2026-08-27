cmake_minimum_required(VERSION 3.24)
project(tribios_vfs VERSION 0.0.1 LANGUAGES CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)

add_library(tribios_core STATIC
  src/core/paths.cpp
  src/core/proc.cpp
  src/core/metadata_store.cpp
  src/core/recovery.cpp
  src/core/fault_injection.cpp
  src/core/base_capture.cpp
  src/core/storage/workspace_storage.cpp
  src/core/git_worktree.cpp
  src/core/project_manager.cpp
)
if(APPLE)
  target_sources(tribios_core PRIVATE src/core/storage/apfs_shadow_storage.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(tribios_core PRIVATE
    src/core/storage/btrfs_snapshot_storage.cpp
    src/core/storage/overlayfs_storage.cpp)
endif()
target_include_directories(tribios_core PUBLIC includes)
target_link_libraries(tribios_core PUBLIC PkgConfig::SQLITE3)
target_compile_definitions(tribios_core PRIVATE
  TRIBIOS_STORAGE_SERVICE_DEFAULT_SOCKET="/run/tribios/storage.sock")
find_package(Threads REQUIRED)
target_link_libraries(tribios_core PUBLIC Threads::Threads)

add_library(tribios_control STATIC
  src/daemon/protocol.cpp
  src/daemon/control_server.cpp
  src/daemon/control_client.cpp
)
target_link_libraries(tribios_control PUBLIC tribios_core)

add_executable(tribios_daemon src/daemon/main.cpp)
target_link_libraries(tribios_daemon PRIVATE tribios_control)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(tribios_storage_service src/storage_service/main.cpp)
  configure_file(packaging/systemd/tribios-storage.service.in
                 tribios-storage.service @ONLY)
endif()

add_executable(tribios src/cli/main.cpp)
target_link_libraries(tribios PRIVATE tribios_control)
target_compile_definitions(tribios PRIVATE TRIBIOS_VERSION="${PROJECT_VERSION}")

set(CMAKE_INSTALL_DOCDIR "${CMAKE_INSTALL_DATAROOTDIR}/doc/tribios-vfs")
set(TRIBIOS_DAEMON_LIBEXEC_DIR "${CMAKE_INSTALL_LIBEXECDIR}/tribios")
set(TRIBIOS_SYSTEMD_UNIT_DIR "lib/systemd/system" CACHE PATH
    "Installation directory for systemd service units")
target_compile_definitions(tribios PRIVATE
  TRIBIOS_DAEMON_LIBEXEC_DIR="${TRIBIOS_DAEMON_LIBEXEC_DIR}")

install(TARGETS tribios RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS tribios_daemon RUNTIME DESTINATION ${TRIBIOS_DAEMON_LIBEXEC_DIR})
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  install(TARGETS tribios_storage_service RUNTIME DESTINATION ${TRIBIOS_DAEMON_LIBEXEC_DIR})
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/tribios-storage.service
          DESTINATION ${TRIBIOS_SYSTEMD_UNIT_DIR})
endif()
install(FILES README.md LICENSE DESTINATION ${CMAKE_INSTALL_DOCDIR})

# Packagers build from a release archive in a network-isolated chroot, and the
# unit tier fetches Catch2 at configure time. Turning the tests off keeps that
# fetch, and the Python interpreter the benchmark harness test needs, out of a
# packaging build.
option(TRIBIOS_BUILD_TESTS "Build and register the test suite" ON)
if(TRIBIOS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
