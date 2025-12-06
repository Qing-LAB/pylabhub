# /cpp/tests/CMakeLists.txt
#
# Configures the project's test suite.
#
cmake_minimum_required(VERSION 3.18)

# Create a custom target to which all test staging commands will be attached.
# The top-level CMakeLists.txt makes `stage_all` depend on this target.
add_custom_target(stage_tests COMMENT "Staging all test executables")

# Find all test source files. The convention is one executable per test file.
file(GLOB TEST_SOURCES "*.cpp")

# Create a test executable for each source file found.
foreach(TEST_SOURCE ${TEST_SOURCES})
  # Create a unique target name from the source file name.
  get_filename_component(TEST_NAME ${TEST_SOURCE} NAME_WE)
  set(TEST_TARGET "test_${TEST_NAME}")

  message(STATUS "Configuring test: ${TEST_TARGET}")

  add_executable(${TEST_TARGET} ${TEST_SOURCE})

  # Link the test against the core library and the test framework.
  # The core library brings in all other necessary dependencies.
  target_link_libraries(${TEST_TARGET} PRIVATE
    pylabhub::core
    # Assuming a test framework like Catch2 is available as a third-party target.
    # pylabhub::third_party::catch2
  )

  # Stage the test executable to the 'bin' directory so it can find
  # the shared libraries (like pylabhub-utils.so) at runtime.
  pylabhub_stage_executable(
    TARGET ${TEST_TARGET}
    DESTINATION bin
    ATTACH_TO stage_tests
  )

  # Register the executable with CTest.
  add_test(NAME ${TEST_TARGET} COMMAND ${TEST_TARGET})
endforeach()

message(STATUS "Configured test suite. Run 'ctest' or build the 'test' target.")