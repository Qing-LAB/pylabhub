# tests/helpers/CMakeLists.txt

# This library contains common helper code for all test executables,
# including the main() entry point, worker dispatch logic, and worker implementations.

add_library(pylabhub_test_helpers STATIC
  test_entrypoint.cpp
  test_entrypoint.h
  test_process_utils.cpp
  test_process_utils.h

  # New refactored worker sources
  logger_workers.cpp
  filelock_workers.cpp
  jsonconfig_workers.cpp
  shared_test_helpers.cpp

  # Headers for the workers and helpers
  workers.h
  worker_filelock.h
  worker_jsonconfig.h
  worker_logger.h
  shared_test_helpers.h
)

# The helpers need to link against the utils library for its own implementation
# (e.g., workers use FileLock, JsonConfig, etc.)
target_link_libraries(pylabhub_test_helpers
  PRIVATE
    pylabhub::utils
    pylabhub::third_party::gtest
)

# The helpers also need to link against gtest because the entry point uses it.
target_link_libraries(pylabhub_test_helpers PRIVATE pylabhub::third_party::gtest)

target_include_directories(pylabhub_test_helpers
  PUBLIC
    # Expose the current directory so consumers can #include "test_entrypoint.h", "workers.h"
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

target_compile_features(pylabhub_test_helpers PRIVATE cxx_std_20)

if(MSVC)
  target_compile_options(pylabhub_test_helpers PRIVATE /Zc:preprocessor)
endif()
