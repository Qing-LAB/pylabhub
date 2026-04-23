# /cpp/tests/CMakeLists.txt
#
# Configures the project's test suite.
# This file enables testing, includes GoogleTest, defines an aggregator target
# for staging test executables, and configures various test sub-projects.
# It relies on the BUILD_TESTS option to enable or disable the test build.
#
cmake_minimum_required(VERSION 3.28)

# Validate PYLABHUB_STAGING_DIR. This is crucial as all staging relies on it.
if(NOT DEFINED PYLABHUB_STAGING_DIR OR "${PYLABHUB_STAGING_DIR}" STREQUAL "")
  message(FATAL_ERROR "tests/CMakeLists.txt expects PYLABHUB_STAGING_DIR to be defined by the parent project.")
endif()

if(BUILD_TESTS)
  message(STATUS "-- BUILD_TESTS is enabled, configuring test sub-projects...")
  enable_testing()
  include(GoogleTest)
  include(StageHelpers)

  # PYLABHUB_BUILD_TESTS is now defined project-wide in ToplevelOptions.cmake
  # when BUILD_TESTS=ON. This makes PYLABHUB_UTILS_TEST_EXPORT symbols visible
  # in the shared library AND in test binaries.

  # ── CI-environment detection ────────────────────────────────────────
  # Detect at configure time whether this build is running inside a CI
  # environment.  Captured as PYLABHUB_CI_BUILD (cache variable) and
  # propagated to test translation units as -DPYLABHUB_CI_BUILD so test
  # helpers can select CI-specific defaults (longer wait timeouts, etc.)
  # at compile time.  No runtime env check needed.
  #
  # Signals:
  #   - CI               — de-facto standard, set by GitHub Actions,
  #                        GitLab CI, CircleCI, Travis, Jenkins, Azure
  #                        DevOps, etc.
  #   - GITHUB_ACTIONS   — GH-Actions-specific fallback.
  #
  # To simulate a CI build locally, `CI=1 cmake -S . -B build-ci`.
  if(DEFINED ENV{CI} OR DEFINED ENV{GITHUB_ACTIONS})
    set(PYLABHUB_CI_BUILD ON CACHE BOOL
        "CI environment detected at configure time; tests apply relaxed deadlines")
    message(STATUS "-- CI environment detected; enabling CI-specific test relaxations")

    # Library-side: swap Argon2id from INTERACTIVE (~100ms @ 64MiB) to MIN
    # (~1ms @ 8KiB) for CI test runs.  INTERACTIVE memory-thrashes on
    # loaded GH Actions runners, stretching keygen from 100ms to 60+
    # seconds.  MIN is cryptographically weak but acceptable for test
    # vaults that never leave the test directory.  Only applied when
    # BUILD_TESTS=ON + CI — production builds (no BUILD_TESTS) never
    # set this flag and retain the production KDF.  See
    # src/utils/service/vault_crypto.hpp header doc.
    if(TARGET pylabhub-utils)
      target_compile_definitions(pylabhub-utils PRIVATE PYLABHUB_VAULT_TEST_KDF)
      message(STATUS "-- CI + BUILD_TESTS: Argon2id tuned to MIN for test vaults "
                     "(production security unaffected)")
    endif()
  else()
    set(PYLABHUB_CI_BUILD OFF CACHE BOOL
        "CI environment detected at configure time; tests apply relaxed deadlines")
  endif()

  # Use PRE_TEST discovery mode on multi-config generators (Visual Studio).
  # The default POST_BUILD mode runs the test executable at build time to
  # enumerate tests, which fails with VS because the WORKING_DIRECTORY
  # doesn't contain a CMakeLists.txt.  PRE_TEST defers discovery to ctest.
  if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE "PRE_TEST" CACHE STRING "" FORCE)
  endif()

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

  # Configure layered test sub-projects
  add_subdirectory(test_layer0_platform)
  add_subdirectory(test_layer1_base)
  add_subdirectory(test_layer2_service)
  add_subdirectory(test_layer3_datahub)
  # Historical note — removed test directories:
  #   - test_layer4_actor: pylabhub-actor replaced by per-role binaries
  #     (2026-03), then consolidated into plh_role (2026-04).
  #   - test_layer4_{producer,consumer,processor}: retired with the
  #     per-role binaries as HEP-0024 Phase 20 landed; CLI coverage
  #     moved to test_layer4_plh_role (no-hub tier).
  #   - test_layer4_integration test_channel_broadcast + test_pipeline_roundtrip:
  #     broker-involved; deferred — see docs/todo/TESTING_TODO.md
  #     "Deferred L4 broker tests".
  add_subdirectory(test_layer4_plh_role)
  add_subdirectory(test_layer4_integration)
  # test_raii_layer REMOVED: test_result.cpp moved to test_layer1_base (T5.1)

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