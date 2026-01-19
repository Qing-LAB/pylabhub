# tests/test_pylabhub_corelib/CMakeLists.txt
#
# Configures the test executable for the pylabhub-basic static library.
# These tests do not require lifecycle management or subprocess workers.

cmake_minimum_required(VERSION 3.29)

add_executable(test_pylabhub_corelib
  test_atomicguard.cpp
  test_recursionguard.cpp
  test_formattable.cpp
  test_platform.cpp
  test_scopeguard.cpp
)
add_dependencies(test_pylabhub_corelib stage_pylabhub_utils)

# Link against the test framework. This provides:
# 1. A generic main() entry point (from test_entrypoint.cpp).
# 2. Transitive dependencies on gtest and gmock.
# 3. Shared helper functions and headers.
#
# Note: We do NOT link gtest_main, as that would cause a "multiple definition of main"
# linker error.
target_link_libraries(test_pylabhub_corelib
  PRIVATE
    pylabhub::test_framework
)

# Define the stress level for the atomic_guard test based on the CMake option.
if(PYLABHUB_ATOMICGUARD_STRESS_LEVEL STREQUAL "None")
    set(ATOMICGUARD_STRESS_DEFINE "ATOMICGUARD_STRESS_LEVEL=0")
elseif(PYLABHUB_ATOMICGUARD_STRESS_LEVEL STREQUAL "Light")
    set(ATOMICGUARD_STRESS_DEFINE "ATOMICGUARD_STRESS_LEVEL=1")
elseif(PYLABHUB_ATOMICGUARD_STRESS_LEVEL STREQUAL "Heavy")
    set(ATOMICGUARD_STRESS_DEFINE "ATOMICGUARD_STRESS_LEVEL=2")
endif()

if(ATOMICGUARD_STRESS_DEFINE)
    target_compile_definitions(test_pylabhub_corelib PRIVATE ${ATOMICGUARD_STRESS_DEFINE})
endif()

target_compile_features(test_pylabhub_corelib PRIVATE cxx_std_20)

# Set the logger compile level to TRACE for tests.
target_compile_definitions(test_pylabhub_corelib PRIVATE LOGGER_COMPILE_LEVEL=0)

if(MSVC)
  target_compile_options(test_pylabhub_corelib PRIVATE /EHsc /wd5105 /Zc:preprocessor)
endif()

# Register the executable for staging and discover its tests
pylabhub_register_test_for_staging(TARGET test_pylabhub_corelib)

# Use the TEST_LAUNCHER property to safely modify the PATH for test discovery
# and execution. This ensures that the test executable can find any required DLLs
# from the staging directory at runtime.
set_property(TARGET test_pylabhub_corelib PROPERTY
  TEST_LAUNCHER "${CMAKE_COMMAND}" -E env --modify "PATH=path_list_append:${STAGED_LIB_DIR}"
)

gtest_discover_tests(test_pylabhub_corelib
  WORKING_DIRECTORY "${STAGED_TEST_DIR}"
  DISCOVERY_TIMEOUT 60
)

