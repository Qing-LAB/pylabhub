# /cpp/tests/CMakeLists.txt
#
# Configures the project's test suite.
# This file enables testing, includes GoogleTest, defines an aggregator target
# for staging test executables, and configures various test sub-projects.
# It relies on the BUILD_TESTS option to enable or disable the test build.
#
cmake_minimum_required(VERSION 3.29)

# Validate PYLABHUB_STAGING_DIR. This is crucial as all staging relies on it.
if(NOT DEFINED PYLABHUB_STAGING_DIR OR "${PYLABHUB_STAGING_DIR}" STREQUAL "")
  message(FATAL_ERROR "tests/CMakeLists.txt expects PYLABHUB_STAGING_DIR to be defined by the parent project.")
endif()

if(BUILD_TESTS)
  message(STATUS "-- BUILD_TESTS is enabled, configuring test sub-projects...")
  enable_testing()
  include(GoogleTest)
  include(StageHelpers)

  # Define staging paths for subdirectories to use


  # Create an aggregator target for all test-related staging.
  # Sub-projects will register their executables to the PYLABHUB_TEST_EXECUTABLES_TO_STAGE
  # global property. We collect them here and add them as dependencies.
  add_custom_target(stage_tests COMMENT "Aggregate target for staging test executables and dependencies")

  # Ensure staged library (pylabhub-utils) is available before its dependent tests are staged.
  if (TARGET stage_pylabhub_utils)
    add_dependencies(stage_tests stage_pylabhub_utils)
  endif()

  # Configure test framework (shared infrastructure)
  add_subdirectory(test_framework)

  # Configure layered test sub-projects (new architecture)
  option(BUILD_LAYERED_TESTS "Build layered test architecture (new)" ON)
  if(BUILD_LAYERED_TESTS)
    message(STATUS "-- Building layered test architecture...")
    add_subdirectory(test_layer0_platform)
    add_subdirectory(test_layer1_base)
    add_subdirectory(test_layer2_service)
    add_subdirectory(test_layer3_datahub)
    message(STATUS "-- Layered tests configured.")
  endif()

  # Configure legacy test sub-projects (during migration)
  option(BUILD_LEGACY_TESTS "Build legacy test structure (old)" ON)
  if(BUILD_LEGACY_TESTS)
    message(STATUS "-- Building legacy test structure...")
    add_subdirectory(test_pylabhub_corelib)
    add_subdirectory(test_pylabhub_utils)
    add_subdirectory(test_misc)
    message(STATUS "-- Legacy tests configured.")
  endif()

  # Collect all test executables registered by the subdirectories
  get_property(test_executables_to_stage GLOBAL PROPERTY PYLABHUB_TEST_EXECUTABLES_TO_STAGE)

  if(test_executables_to_stage)
    message(STATUS "Found test executables to stage: ${test_executables_to_stage}")
    # The executables themselves will be copied to the staged 'tests' directory because
    # pylabhub_register_test_for_staging sets their RUNTIME_OUTPUT_DIRECTORY.
    # On Windows, this also ensures dependent DLLs are found for running tests.
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