cmake_minimum_required(VERSION 3.24)
project(tribios_vfs VERSION 0.0.1 LANGUAGES CXX)

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
  src/core/workspace_engine.cpp
  src/core/git_worktree.cpp
  src/core/project_manager.cpp
)
target_include_directories(tribios_core PUBLIC includes)
target_link_libraries(tribios_core PUBLIC PkgConfig::SQLITE3)
find_package(Threads REQUIRED)
target_link_libraries(tribios_core PUBLIC Threads::Threads)

add_library(tribios_control STATIC
  src/daemon/protocol.cpp
  src/daemon/control_server.cpp
  src/daemon/control_client.cpp
)
target_link_libraries(tribios_control PUBLIC tribios_core)

# The FUSE adapter is the only operating-system specific component. It speaks
# the FUSE 2.x API: macFUSE on macOS, libfuse 2.x on Linux.
#   macOS:  brew install --cask macfuse
#   Debian: apt-get install libfuse-dev
option(TRIBIOS_ENABLE_FUSE "Build the mounted FUSE backend" ON)
if(TRIBIOS_ENABLE_FUSE)
  pkg_check_modules(FUSE IMPORTED_TARGET fuse)
  if(NOT FUSE_FOUND)
    pkg_check_modules(FUSE IMPORTED_TARGET osxfuse)
  endif()
endif()
if(FUSE_FOUND)
  add_library(tribios_fuse STATIC src/fuse/fuse_adapter.cpp)
  target_link_libraries(tribios_fuse PUBLIC tribios_core PkgConfig::FUSE)
  target_compile_definitions(tribios_fuse PUBLIC TRIBIOS_HAVE_FUSE=1)
  message(STATUS "tribios: FUSE ${FUSE_VERSION} found, mount support enabled")
else()
  add_library(tribios_fuse STATIC src/fuse/fuse_adapter_stub.cpp)
  target_link_libraries(tribios_fuse PUBLIC tribios_core)
  if(NOT TRIBIOS_ENABLE_FUSE)
    message(STATUS "tribios: FUSE backend disabled, building without mount support")
  else()
    pkg_check_modules(FUSE3 fuse3)
    if(FUSE3_FOUND)
      message(WARNING "tribios: found FUSE 3 only; Tribios needs the FUSE 2.x API "
                      "(macfuse or libfuse-dev). Building without mount support.")
    else()
      message(STATUS "tribios: no FUSE backend found, building without mount support")
    endif()
  endif()
endif()

add_executable(tribios_daemon src/daemon/main.cpp)
target_link_libraries(tribios_daemon PRIVATE tribios_control tribios_fuse)

add_executable(tribios src/cli/main.cpp)
target_link_libraries(tribios PRIVATE tribios_control)
target_compile_definitions(tribios PRIVATE TRIBIOS_VERSION="${PROJECT_VERSION}")

include(GNUInstallDirs)

set(CMAKE_INSTALL_DOCDIR "${CMAKE_INSTALL_DATAROOTDIR}/doc/tribios-vfs")
set(TRIBIOS_DAEMON_LIBEXEC_DIR "${CMAKE_INSTALL_LIBEXECDIR}/tribios")
target_compile_definitions(tribios PRIVATE
  TRIBIOS_DAEMON_LIBEXEC_DIR="${TRIBIOS_DAEMON_LIBEXEC_DIR}")

install(TARGETS tribios RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS tribios_daemon RUNTIME DESTINATION ${TRIBIOS_DAEMON_LIBEXEC_DIR})
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
