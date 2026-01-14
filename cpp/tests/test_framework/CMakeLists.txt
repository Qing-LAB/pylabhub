# tests/test_framework/CMakeLists.txt
#
# This library provides a generic test framework for running tests.
# It includes a generic main() entry point (from test_entrypoint.cpp) that can
# dispatch to registered worker functions.
#
# Because this framework provides a main() function, executables that link
# against it should NOT link against gtest_main.
cmake_minimum_required(VERSION 3.29)

add_library(pylabhub-test-framework STATIC
  shared_test_helpers.cpp
  shared_test_helpers.h
  test_entrypoint.cpp
  test_entrypoint.h
  test_process_utils.cpp
  test_process_utils.h
)
add_library(pylabhub::test_framework ALIAS pylabhub-test-framework)

target_compile_features(pylabhub-test-framework PRIVATE cxx_std_20)

# The framework needs basic pylabhub libraries and gtest
target_link_libraries(pylabhub-test-framework
  PUBLIC
    pylabhub::basic
    pylabhub::utils
    pylabhub::third_party::gtest
    pylabhub::third_party::gmock
)

if(PYLABHUB_SANITIZER_FLAG)
  target_link_libraries(pylabhub-test-framework PRIVATE pylabhub::sanitizer_flags)
endif()

# Publicly expose this directory so consumers can find the helper headers.
target_include_directories(pylabhub-test-framework PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
  "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src/include>"
  "$<INSTALL_INTERFACE:include>"
)

if(MSVC)
  target_compile_options(pylabhub-test-framework PRIVATE /Zc:preprocessor)
endif()