# tests/test_pylabhub_utils/CMakeLists.txt
#
# Configures the test executable for the pylabhub-utils shared library.
# These tests use a custom main() to act as both a test runner and a
# worker process dispatcher.

include(MsvcHelper)
message(STATUS "** Configuring test executable for pylabhub-utils...")
add_executable(test_pylabhub_utils
  # Test suites
  test_filelock.cpp
  test_jsonconfig.cpp
  test_logger.cpp
  test_lifecycle.cpp
  test_lifecycle_dynamic.cpp
  test_messagehub.cpp
  test_datablock.cpp
  test_datablock_management_mutex.cpp
  test_transaction_api.cpp
  test_recovery_api.cpp
  test_schema_validation.cpp
  test_benchmarks.cpp

  # Worker implementations
  filelock_workers.cpp
  jsonconfig_workers.cpp
  logger_workers.cpp
  lifecycle_workers.cpp
  messagehub_workers.cpp
  datablock_management_mutex_workers.cpp
  worker_dispatcher.cpp
)

# This test executable uses the generic main() from the pylabhub::test_framework.
# It also compiles 'worker_dispatcher.cpp', which defines and registers a set of
# worker scenarios specific to the pylabhub-utils tests. This dispatcher is then
# called by the framework's main() if the executable is run in a worker mode.
target_link_libraries(test_pylabhub_utils
  PRIVATE
    pylabhub::test_framework
    pylabhub::utils
)

if(PYLABHUB_SANITIZER_FLAGS_SET)
  message(STATUS "** Sanitizer detected, adding to test_pylabhub_utils.")
  target_compile_options(test_pylabhub_utils PRIVATE $<TARGET_PROPERTY:pylabhub::sanitizer_flags,INTERFACE_COMPILE_OPTIONS>)
  target_link_libraries(test_pylabhub_utils PRIVATE pylabhub::sanitizer_flags)
endif()

target_compile_features(test_pylabhub_utils PRIVATE cxx_std_20)

# Set the logger compile level to TRACE for tests.
target_compile_definitions(test_pylabhub_utils PRIVATE LOGGER_COMPILE_LEVEL=0)

pylabhub_apply_standard_msvc_options(test_pylabhub_utils)

# Ensure the main utils library is built and staged before this test executable.
if (TARGET stage_pylabhub_utils)
  add_dependencies(test_pylabhub_utils stage_pylabhub_utils)
endif()

# Register the executable for staging and discover its tests
pylabhub_register_test_for_staging(TARGET test_pylabhub_utils)

# Use the TEST_LAUNCHER property to safely modify the PATH for test discovery
# and execution. This is the modern, robust way to handle environment
# variables, especially on Windows where paths can contain spaces.
# The `cmake -E env --modify` command correctly handles native path separators.
set_property(TARGET test_pylabhub_utils PROPERTY
  TEST_LAUNCHER "${CMAKE_COMMAND}" -E env --modify "PATH=path_list_append:${PYLABHUB_STAGING_DIR}/bin"
)

gtest_discover_tests(test_pylabhub_utils
  WORKING_DIRECTORY "${PYLABHUB_STAGING_DIR}/tests"
  DISCOVERY_TIMEOUT 60
)
