# End-to-end tests observe behavior through the same external seam a person or
# coding agent uses: the `tribios` CLI, Workspace paths and unmodified developer
# tools. Tier 0 unit tests under tests/unit/ cover self-contained logic that has
# no observable seam.

# Tier 1: forced invariants, assertions that are never editable.
set(TRIBIOS_INVARIANT_TESTS
  workspace_lifecycle
  filesystem_semantics
  isolation
  deletion_persistence
  restart_persistence
  native_lifecycle_recovery
  concurrent_workspaces
  git_state_durability
)

# Tier 2: designed decisions traceable to an ADR or a documented deliberate limit.
set(TRIBIOS_DESIGN_TESTS
  configure_contract
  git_workflow
  end_to_end_build
  packaging_install
  packaging_rpm_spec
)

foreach(script IN LISTS TRIBIOS_INVARIANT_TESTS TRIBIOS_DESIGN_TESTS)
  if("${script}" IN_LIST TRIBIOS_INVARIANT_TESTS)
    set(tier invariant)
  else()
    set(tier design)
  endif()
  add_test(NAME ${script}
           COMMAND ${CMAKE_COMMAND} -E env
                   TRIBIOS_BIN=$<TARGET_FILE:tribios>
                   TRIBIOS_DAEMON=$<TARGET_FILE:tribios_daemon>
                   TRIBIOS_BUILD_DIR=${CMAKE_BINARY_DIR}
                   bash ${CMAKE_CURRENT_SOURCE_DIR}/e2e/${script}.sh)
  set_tests_properties(${script} PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77 LABELS ${tier})
endforeach()

add_subdirectory(unit)
