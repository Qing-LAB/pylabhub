# tests/test_framework/CMakeLists.txt
#
# This library provides a generic test framework for running tests.
# It includes a generic main() entry point (from test_entrypoint.cpp) that can
# dispatch to registered worker functions.
#
# Because this framework provides a main() function, executables that link
# against it should NOT link against gtest_main.
cmake_minimum_required(VERSION 3.29)

include(MsvcHelper)

message(STATUS "-- start configuring pylabhub::test_framework static lib...")

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
    pylabhub::utils
    pylabhub::third_party::gtest
    pylabhub::third_party::gmock
)

# When sanitizer is enabled, the framework's code is instrumented. All test executables
# that link this framework must also link the sanitizer runtime. We use PUBLIC so
# consumers inherit the link dependency; otherwise they get undefined references
# (e.g. __tsan_func_entry, __tsan_read8).
if(PYLABHUB_SANITIZER_FLAGS_SET)
  target_link_libraries(pylabhub-test-framework PUBLIC pylabhub::sanitizer_flags)
endif()

# Publicly expose this directory so consumers can find the helper headers.
target_include_directories(pylabhub-test-framework PUBLIC
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
)

pylabhub_apply_standard_msvc_options(pylabhub-test-framework)