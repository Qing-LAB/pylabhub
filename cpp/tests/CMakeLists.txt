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
# This executable contains all simple, single-process unit tests.
add_executable(utils_tests
  test_atomicguard.cpp
  test_recursionguard.cpp
)

target_link_libraries(utils_tests
  PRIVATE
  pylabhub::utils
  pylabhub::third_party::gtest
  pylabhub_test_helpers
)

target_compile_features(utils_tests PRIVATE cxx_std_20)
target_compile_definitions(utils_tests PRIVATE LOGGER_COMPILE_LEVEL=0)
if(PYLABHUB_LOGGER_DEBUG)
  target_compile_definitions(utils_tests PRIVATE _LOGGER_DEBUG_ENABLED=1)
endif()
if(MSVC)
  target_compile_options(utils_tests PRIVATE /EHsc /wd5105 /Zc:preprocessor)
endif()

# --- 2. Multi-Process Test Executable ---
# This executable contains tests that require spawning worker processes to ensure
# full isolation for stateful, singleton-based components.
add_executable(multiprocess_tests
  test_filelock.cpp
  test_jsonconfig.cpp
)

# --- 3. Logger Test Executable ---
# This executable is built from the refactored, split-out logger test files.
add_executable(test_logger
  # Sources are added from the subdirectory
)
add_subdirectory(logger)

target_link_libraries(multiprocess_tests
  PRIVATE
  pylabhub::utils
  pylabhub::third_party::gtest
  pylabhub_test_helpers
)

target_link_libraries(test_logger
  PRIVATE
  pylabhub::utils
  pylabhub::third_party::gtest
  pylabhub_test_helpers
)

target_compile_features(multiprocess_tests PRIVATE cxx_std_20)
target_compile_definitions(multiprocess_tests PRIVATE LOGGER_COMPILE_LEVEL=0)
if(PYLABHUB_LOGGER_DEBUG)
  target_compile_definitions(multiprocess_tests PRIVATE _LOGGER_DEBUG_ENABLED=1)
endif()
if(MSVC)
  target_compile_options(multiprocess_tests PRIVATE /EHsc /wd5105 /Zc:preprocessor)
endif()

target_compile_features(test_logger PRIVATE cxx_std_20)
target_compile_definitions(test_logger PRIVATE LOGGER_COMPILE_LEVEL=0)
if(PYLABHUB_LOGGER_DEBUG)
  target_compile_definitions(test_logger PRIVATE _LOGGER_DEBUG_ENABLED=1)
endif()
if(MSVC)
  target_compile_options(test_logger PRIVATE /EHsc /wd5105 /Zc:preprocessor)
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
pylabhub_stage_executable(
  TARGET test_logger
  DESTINATION bin
  ATTACH_TO stage_tests
)

# Ensure staging happens after the executables are built, and after the staging directories have been created.
add_dependencies(stage_tests utils_tests multiprocess_tests test_logger create_staging_dirs)

# --- CTest Integration ---

# 1. DEPENDENCY: Ensure the pylabhub-utils shared library is staged BEFORE
#    test discovery is run. This is the crucial step that makes the test
#    executable runnable by gtest_discover_tests.
add_dependencies(utils_tests stage_pylabhub_utils)
add_dependencies(multiprocess_tests stage_pylabhub_utils)
add_dependencies(test_logger stage_pylabhub_utils)

# 2. RPATH (for macOS/Linux): Set the RPATH for the test executables so they
#    can find the staged library at build/run time. On Windows, this has no effect.
if(NOT WIN32)
  set(STAGED_LIB_DIR "${PYLABHUB_STAGING_DIR}/tests")
  set_target_properties(utils_tests multiprocess_tests test_logger
    PROPERTIES
      BUILD_RPATH "${STAGED_LIB_DIR}"
  )
endif()

# 3. DISCOVERY: Now that the library is staged and findable (via RPATH or PATH),
#    run the discovery process in the staging directory.
set(TEST_RUNTIME_DIR "${PYLABHUB_STAGING_DIR}/tests")
message(STATUS "  ** Setting test runtime directory to: ${TEST_RUNTIME_DIR}")
gtest_discover_tests(utils_tests
    WORKING_DIRECTORY "${TEST_RUNTIME_DIR}"
    PROPERTIES
        # This is primarily for Windows, but doesn't hurt other platforms.
        ENVIRONMENT "PATH=${STAGED_LIB_DIR}"
)
gtest_discover_tests(multiprocess_tests
    WORKING_DIRECTORY "${TEST_RUNTIME_DIR}"
    PROPERTIES
        # This is primarily for Windows, but doesn't hurt other platforms.
        ENVIRONMENT "PATH=${STAGED_LIB_DIR}"
)
gtest_discover_tests(test_logger
    WORKING_DIRECTORY "${TEST_RUNTIME_DIR}"
    PROPERTIES
        # This is primarily for Windows, but doesn't hurt other platforms.
        ENVIRONMENT "PATH=${STAGED_LIB_DIR}"
)

message(STATUS "Configured test executables: utils_tests, multiprocess_tests, test_logger")
