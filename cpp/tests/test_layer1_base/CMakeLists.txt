# tests/test_layer1_base/CMakeLists.txt
#
# Layer 1: Base utilities tests
# Tests for plh_base.hpp (depends on plh_platform.hpp)

message(STATUS "** Configuring Layer 1 (Base) tests...")

add_executable(test_layer1_base
  # Placeholder during Phase 0
  test_base_placeholder.cpp

  # To be migrated from test_pylabhub_corelib in Phase 2
  # test_format_tools.cpp
  # test_debug_info.cpp
  # test_atomic_guard.cpp
  # test_recursion_guard.cpp
  # test_scope_guard.cpp
  # test_module_def.cpp
)

target_link_libraries(test_layer1_base
  PRIVATE
    pylabhub::test_framework
    pylabhub::utils
)

target_compile_features(test_layer1_base PRIVATE cxx_std_20)
target_compile_definitions(test_layer1_base PRIVATE LOGGER_COMPILE_LEVEL=0)

pylabhub_register_test_for_staging(TARGET test_layer1_base)

set_property(TARGET test_layer1_base PROPERTY
  TEST_LAUNCHER "${CMAKE_COMMAND}" -E env --modify "PATH=path_list_append:${PYLABHUB_STAGING_DIR}/bin"
)

gtest_discover_tests(test_layer1_base
  WORKING_DIRECTORY "${PYLABHUB_STAGING_DIR}/tests"
  DISCOVERY_TIMEOUT 60
)

message(STATUS "** Layer 1 tests configured.")
