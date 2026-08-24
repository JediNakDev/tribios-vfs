# Tests observe behavior through the same external seam a person or coding agent
# uses: the `tribios` CLI, Workspace paths and unmodified developer tools.

set(TRIBIOS_TEST_SCRIPTS
  workspace_lifecycle
  filesystem_semantics
  isolation
  deletion_and_tombstones
  restart_persistence
  unsupported_operations
  git_workflow
  end_to_end_build
)

foreach(script IN LISTS TRIBIOS_TEST_SCRIPTS)
  add_test(NAME ${script}
           COMMAND ${CMAKE_COMMAND} -E env
                   TRIBIOS_BIN=$<TARGET_FILE:tribios>
                   TRIBIOS_DAEMON=$<TARGET_FILE:tribios_daemon>
                   bash ${CMAKE_CURRENT_SOURCE_DIR}/e2e/${script}.sh)
  set_tests_properties(${script} PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77)
endforeach()
