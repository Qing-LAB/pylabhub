# tests/test_harness/CMakeLists.txt
#
# This library provides a generic test harness for running tests that require
# subprocess workers. It includes a custom main() entry point and process-spawning helpers.

add_library(pylabhub-test-harness
  STATIC
  test_entrypoint.cpp
  shared_test_helpers.cpp
  test_process_utils.cpp
)

target_compile_features(pylabhub-test-harness PRIVATE cxx_std_20)

target_link_libraries(pylabhub-test-harness
  PRIVATE
    pylabhub::utils
    pylabhub::basic
    pylabhub::third_party::gtest
)

# Publicly expose the include directory so consumers can find the helper headers.
target_include_directories(pylabhub-test-harness
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

if(MSVC)
  target_compile_options(pylabhub-test-harness PRIVATE /Zc:preprocessor)
endif()