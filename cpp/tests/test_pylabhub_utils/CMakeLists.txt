# tests/test_pylabhub_utils/CMakeLists.txt
#
# Configures the test executable for the pylabhub-utils shared library.
# These tests use a custom main() to act as both a test runner and a
# worker process dispatcher.

add_executable(test_pylabhub_utils
  # Test suites
  test_filelock.cpp
  test_jsonconfig.cpp
  test_logger.cpp
  test_lifecycle.cpp
  test_lifecycle_dynamic.cpp

  # Worker implementations
  filelock_workers.cpp
  jsonconfig_workers.cpp
  logger_workers.cpp
  lifecycle_workers.cpp
  worker_dispatcher.cpp
)

# This test executable uses the generic main() from the pylabhub::test_framework.
# It also compiles 'worker_dispatcher.cpp', which defines and registers a set of
# worker scenarios specific to the pylabhub-utils tests. This dispatcher is then
# called by the framework's main() if the executable is run in a worker mode.
target_link_libraries(test_pylabhub_utils
  PRIVATE
    pylabhub::test_framework
)

target_compile_features(test_pylabhub_utils PRIVATE cxx_std_20)

# Set the logger compile level to TRACE for tests.
target_compile_definitions(test_pylabhub_utils PRIVATE LOGGER_COMPILE_LEVEL=0)

if(MSVC)
  target_compile_options(test_pylabhub_utils PRIVATE /EHsc /wd5105 /Zc:preprocessor)
endif()

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
  TEST_LAUNCHER "${CMAKE_COMMAND}" -E env --modify "PATH=path_list_append:${STAGED_LIB_DIR}"
)



gtest_discover_tests(test_pylabhub_utils
  WORKING_DIRECTORY "${STAGED_TEST_DIR}"
  DISCOVERY_TIMEOUT 60
)
