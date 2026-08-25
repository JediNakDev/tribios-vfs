# Tier 0: pure, self-contained logic. No daemon, no FUSE mount, no Git, no
# sleeps. The whole tier is expected to finish in well under two seconds.

include(FetchContent)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.15.3
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(Catch2)

add_executable(tribios_unit_tests
  path_normalization_test.cpp
  control_protocol_framing_test.cpp
  base_capture_exclusion_test.cpp
  tombstone_resolution_test.cpp
)
target_link_libraries(tribios_unit_tests PRIVATE tribios_control tribios_core
                                                 Catch2::Catch2WithMain)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(tribios_unit_tests PROPERTIES LABELS "unit")
