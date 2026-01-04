# tests/test_misc/CMakeLists.txt
#
# Configures for individual tests that are not using gtest framework
# These tests are compiled individually and may depend on different 
# components within the library to test individual functions of interest.

add_executable(test_debug_output
  test_debug_output.cpp
)

# Link against the test framework. This provides:
# 1. A generic main() entry point (from test_entrypoint.cpp).
# 2. Transitive dependencies on gtest and gmock.
# 3. Shared helper functions and headers.
#
# Note: We do NOT link gtest_main, as that would cause a "multiple definition of main"
# linker error.
target_link_libraries(test_debug_output
  PRIVATE
    pylabhub::basic
)

target_compile_features(test_debug_output PRIVATE cxx_std_20)

if(MSVC)
  target_compile_options(test_debug_output PRIVATE /EHsc /wd5105 /Zc:preprocessor)
endif()

# Register the executable for staging and discover its tests
pylabhub_register_test_for_staging(TARGET test_debug_output)

# These tests are not registered for gtest framework. they are individually run