# /cpp/tests/CMakeLists.txt
#
# Configures all test executables for the project.

cmake_minimum_required(VERSION 3.18)

# Include helper modules that provide custom staging functions.
include(StageHelpers)

# Define the test executables.
set(TEST_SOURCES
  test_logger.cpp
  test_filelock.cpp
  test_jsonconfig.cpp
  test_atomicguard.cpp
)

# Create a list of target names from source files.
set(TEST_TARGETS "")
foreach(test_src ${TEST_SOURCES})
  get_filename_component(test_name ${test_src} NAME_WE)
  list(APPEND TEST_TARGETS ${test_name})
endforeach()

foreach(test_target ${TEST_TARGETS})
  add_executable(${test_target} ${test_target}.cpp)

  # Set the C++ standard for the test executables to C++20.
  target_compile_features(${test_target} PRIVATE cxx_std_20)

  # All tests depend on the pylabhub-utils shared library.
  # Linking against the namespaced alias is the correct modern CMake approach.
  target_link_libraries(${test_target} PRIVATE pylabhub::utils)

  # Define the compile-time log level for tests to enable all log messages.
  # This overrides any default and ensures the LOGGER_* macros are compiled in.
  target_compile_definitions(${test_target} PRIVATE LOGGER_COMPILE_LEVEL=0)

  if (PYLABHUB_LOGGER_DEBUG)
    target_compile_definitions(${test_target} PRIVATE _LOGGER_DEBUG_ENABLED=1)
  endif()

  # Add each test to CTest for `ctest` command integration.
  # The COMMAND points to the executable in the staging directory. This ensures
  # that the test runs in an environment identical to deployment, where all
  # shared libraries are in the same directory.
  add_test(
    NAME ${test_target}
    COMMAND "${PYLABHUB_STAGING_DIR}/bin/${test_target}"
  )
endforeach()

# --- Staging ---
# Create a custom target to stage all test executables to the `bin` directory.
# This target will be a dependency of the top-level `stage_all` target.
add_custom_target(stage_tests COMMENT "Staging all test executables")

# Loop through each test target and attach its staging command to `stage_tests`.
# This pattern follows the one used in `src/CMakeLists.txt` and uses the
# singular `pylabhub_stage_executable` function, which is known to exist.
foreach(test_target ${TEST_TARGETS})
  pylabhub_stage_executable(
    TARGET ${test_target}
    DESTINATION bin
    ATTACH_TO stage_tests
  )
endforeach()

# The staging target must depend on the executables it intends to stage.
# This ensures they are built before the staging commands are run.
add_dependencies(stage_tests ${TEST_TARGETS})

message(STATUS "Configured test executables: ${TEST_TARGETS}")
