# tests/test_pylabhub_corelib/CMakeLists.txt
#
# Configures the test executable for the pylabhub-basic static library.
# These tests do not require lifecycle management or subprocess workers.

add_executable(test_pylabhub_corelib
  test_atomicguard.cpp
  test_recursionguard.cpp
)

target_link_libraries(test_pylabhub_corelib
  PRIVATE
    pylabhub::basic
    pylabhub::third_party::gtest_main
)

target_compile_features(test_pylabhub_corelib PRIVATE cxx_std_20)

if(MSVC)
  target_compile_options(test_pylabhub_corelib PRIVATE /EHsc /wd5105 /Zc:preprocessor)
endif()

# Register the executable for staging and discover its tests
pylabhub_register_test_for_staging(TARGET test_pylabhub_corelib)

gtest_discover_tests(test_pylabhub_corelib
  WORKING_DIRECTORY "${STAGED_TEST_DIR}"
)