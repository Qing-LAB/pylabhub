# /cpp/tests/CMakeLists.txt
#
# Configures a single test executable for the project using GoogleTest.

cmake_minimum_required(VERSION 3.18)

include(GoogleTest)
include(StageHelpers)

# Set a predictable output location for the test executable.
# For multi-configuration generators like Visual Studio, this will create a
# subdirectory (e.g., 'bin/Debug').
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

file(GLOB TEST_SOURCES "*.cpp")
add_executable(run_tests ${TEST_SOURCES})

target_link_libraries(run_tests
  PRIVATE
  pylabhub::utils
  pylabhub::third_party::gtest
  pylabhub::third_party::gtest_main
)

target_compile_features(run_tests PRIVATE cxx_std_20)
target_compile_definitions(run_tests PRIVATE LOGGER_COMPILE_LEVEL=0)
if(PYLABHUB_LOGGER_DEBUG)
  target_compile_definitions(run_tests PRIVATE _LOGGER_DEBUG_ENABLED=1)
endif()

if(MSVC)
  target_compile_options(run_tests PRIVATE /EHsc /wd5105)
  target_compile_options(run_tests PRIVATE /Zc:preprocessor)
endif()

# --- Staging ---
# The executable is already being built into a 'bin' directory, but we still
# need to stage it to the global staging area so it can find the main app's
# shared libraries at runtime.
add_custom_target(stage_tests COMMENT "Staging test executable")
pylabhub_stage_executable(
  TARGET run_tests
  DESTINATION bin
  ATTACH_TO stage_tests
)
add_dependencies(stage_tests run_tests)

# --- CTest Integration ---
# Use POST_BUILD discovery mode, which is more robust for Visual Studio and
# custom build/staging setups. It ensures discovery runs after the executable
# is guaranteed to be built.
gtest_discover_tests(run_tests DISCOVERY_MODE POST_BUILD)

message(STATUS "Configured GoogleTest executable: run_tests")