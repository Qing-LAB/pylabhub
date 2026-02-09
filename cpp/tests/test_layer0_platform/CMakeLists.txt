# tests/test_layer0_platform/CMakeLists.txt
#
# Layer 0: Platform abstraction tests
# Tests for plh_platform.hpp (no dependencies)

message(STATUS "** Configuring Layer 0 (Platform) tests...")

add_executable(test_layer0_platform
  # Placeholder during Phase 0
  test_platform_placeholder.cpp

  # Platform tests (to be added in Phase 1)
  # test_platform_detection.cpp
  # test_platform_process.cpp
  # test_platform_timing.cpp
  # test_platform_info.cpp
)

target_link_libraries(test_layer0_platform
  PRIVATE
    pylabhub::test_framework
    pylabhub::utils  # For platform functions
)

target_compile_features(test_layer0_platform PRIVATE cxx_std_20)
target_compile_definitions(test_layer0_platform PRIVATE LOGGER_COMPILE_LEVEL=0)

# Register for staging and test discovery
pylabhub_register_test_for_staging(TARGET test_layer0_platform)

set_property(TARGET test_layer0_platform PROPERTY
  TEST_LAUNCHER "${CMAKE_COMMAND}" -E env --modify "PATH=path_list_append:${PYLABHUB_STAGING_DIR}/bin"
)

gtest_discover_tests(test_layer0_platform
  WORKING_DIRECTORY "${PYLABHUB_STAGING_DIR}/tests"
  DISCOVERY_TIMEOUT 60
)

message(STATUS "** Layer 0 tests configured.")
