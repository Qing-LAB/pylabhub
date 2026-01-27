# /cpp/tests/CMakeLists.txt
#
# Configures the project's test suite.
# This file enables testing, includes GoogleTest, defines an aggregator target
# for staging test executables, and configures various test sub-projects.
# It relies on the BUILD_TESTS option to enable or disable the test build.
#
cmake_minimum_required(VERSION 3.29)

if(BUILD_TESTS)
  message(STATUS "-- BUILD_TESTS is enabled, configuring test sub-projects...")
  enable_testing()
  include(GoogleTest)
  include(StageHelpers)

  # Define staging paths for subdirectories to use
  set(STAGED_TEST_DIR "${PYLABHUB_STAGING_DIR}/tests")
  set(STAGED_BIN_DIR "${PYLABHUB_STAGING_DIR}/bin")

  # Create an aggregator target for all test-related staging.
  # Sub-projects will register their executables to the PYLABHUB_TEST_EXECUTABLES_TO_STAGE
  # global property. We collect them here and add them as dependencies.
  add_custom_target(stage_tests COMMENT "Aggregate target for staging test executables and dependencies")

  # Ensure staged library (pylabhub-utils) is available before its dependent tests are staged.
  if (TARGET stage_pylabhub_utils)
    add_dependencies(stage_tests stage_pylabhub_utils)
  endif()

  # Configure all test sub-projects
  add_subdirectory(test_framework)
  add_subdirectory(test_pylabhub_corelib)
  add_subdirectory(test_pylabhub_utils)
  add_subdirectory(test_misc)

  # Collect all test executables registered by the subdirectories
  get_property(test_executables_to_stage GLOBAL PROPERTY PYLABHUB_TEST_EXECUTABLES_TO_STAGE)

  if(test_executables_to_stage)
    message(STATUS "Found test executables to stage: ${test_executables_to_stage}")
    # The executables themselves will be copied to the staged 'tests' directory because
    # pylabhub_register_test_for_staging sets their RUNTIME_OUTPUT_DIRECTORY.
    # We just need to make the aggregator 'stage_tests' target depend on them to ensure they are built.
    add_dependencies(stage_tests ${test_executables_to_stage})
  else()
    message(STATUS "No test executables registered for staging.")
  endif()

  message(STATUS "-- Configured test sub-projects.")
  message(STATUS "")
else()
  message(STATUS "-- BUILD_TESTS turned off, building of tests will be skipped.")
  message(STATUS "")
endif()