# tests/test_pylabhub_utils/CMakeLists.txt
#
# Configures the test executable for the pylabhub-utils shared library.
# These tests use a custom main() to act as both a test runner and a
# worker process dispatcher.

add_executable(test_pylabhub_utils
  # Test suites
  test_filelock.cpp
  test_jsonconfig.cpp
  test_logger.cpp

  # Worker implementations from the harness directory
  ../test_harness/filelock_workers.cpp
  ../test_harness/jsonconfig_workers.cpp
  ../test_harness/logger_workers.cpp
)

target_link_libraries(test_pylabhub_utils
  PRIVATE
    pylabhub::utils
    pylabhub-test-harness
    pylabhub::third_party::gtest
)

target_compile_features(test_pylabhub_utils PRIVATE cxx_std_20)

if(MSVC)
  target_compile_options(test_pylabhub_utils PRIVATE /EHsc /wd5105 /Zc:preprocessor)
endif()

# Ensure the main utils library is built and staged before this test executable.
if (TARGET stage_pylabhub_utils)
  add_dependencies(test_pylabhub_utils stage_pylabhub_utils)
endif()

# Register the executable for staging and discover its tests
pylabhub_register_test_for_staging(TARGET test_pylabhub_utils)

gtest_discover_tests(test_pylabhub_utils
  WORKING_DIRECTORY "${STAGED_TEST_DIR}"
  PROPERTIES
    "ENVIRONMENT=PATH=${STAGED_LIB_DIR}$<$<PLATFORM_ID:Windows>:;$ENV{PATH}>"
)