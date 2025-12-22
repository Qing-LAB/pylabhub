# /cpp/tests/CMakeLists.txt
#
# Configures the test suite for the project, creating separate test executables
# for different test categories (e.g., unit tests, multi-process tests).

cmake_minimum_required(VERSION 3.18)

include(GoogleTest)
include(StageHelpers)

# This helper library contains the main() entry point, worker dispatch logic,
# and worker function implementations. All test executables will link against it.
add_subdirectory(helpers)

# Set a predictable output location for all test executables.
# For multi-configuration generators like Visual Studio, this will create a
# subdirectory (e.g., 'bin/Debug').
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# --- 1. Unit Test Executable ---
# This executable contains all single-process unit tests. It's fast to link
# and has minimal dependencies.
add_executable(utils_tests
  test_atomicguard.cpp
  test_logger.cpp
  test_recursionguard.cpp
)

target_link_libraries(utils_tests
  PRIVATE
  pylabhub::utils
  pylabhub::third_party::gtest
  pylabhub_test_helpers
)

target_compile_features(utils_tests PRIVATE cxx_std_20)
target_compile_definitions(utils_tests PRIVATE LOGGER_COMPILE_LEVEL=0 PYLABHUB_TESTING)
if(PYLABHUB_LOGGER_DEBUG)
  target_compile_definitions(utils_tests PRIVATE _LOGGER_DEBUG_ENABLED=1)
endif()
if(MSVC)
  target_compile_options(utils_tests PRIVATE /EHsc /wd5105 /Zc:preprocessor)
endif()

# --- 2. Multi-Process Test Executable ---
# This executable contains tests that require spawning worker processes.
# It is kept separate to isolate the complexity of multi-process testing.
add_executable(multiprocess_tests
  test_filelock.cpp
  test_jsonconfig.cpp
  test_logger_multiprocess.cpp
)

target_link_libraries(multiprocess_tests
  PRIVATE
  pylabhub::utils
  pylabhub::third_party::gtest
  pylabhub_test_helpers
)

target_compile_features(multiprocess_tests PRIVATE cxx_std_20)
target_compile_definitions(multiprocess_tests PRIVATE LOGGER_COMPILE_LEVEL=0 PYLABHUB_TESTING)
if(PYLABHUB_LOGGER_DEBUG)
  target_compile_definitions(multiprocess_tests PRIVATE _LOGGER_DEBUG_ENABLED=1)
endif()
if(MSVC)
  target_compile_options(multiprocess_tests PRIVATE /EHsc /wd5105 /Zc:preprocessor)
endif()

# --- Staging ---
# The master 'stage_tests' target will trigger the staging of all individual
# test executables. This ensures they can find shared libraries from the main
# project at runtime.
add_custom_target(stage_tests COMMENT "Staging all test executables")

pylabhub_stage_executable(
  TARGET utils_tests
  DESTINATION bin
  ATTACH_TO stage_tests
)
pylabhub_stage_executable(
  TARGET multiprocess_tests
  DESTINATION bin
  ATTACH_TO stage_tests
)

# Ensure staging happens after the executables are built.
add_dependencies(stage_tests utils_tests multiprocess_tests)

# --- CTest Integration ---
# Discover tests in each executable. CTest will run them as separate test groups.
gtest_discover_tests(utils_tests)
gtest_discover_tests(multiprocess_tests)

message(STATUS "Configured test executables: utils_tests, multiprocess_tests")
