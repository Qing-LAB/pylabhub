# tests/test_layer0_platform/CMakeLists.txt
#
# Layer 0 (Platform) tests - No lifecycle required (pure API tests)
#
# These tests verify platform abstraction layer functionality:
# - Process/thread identification
# - Time measurement
# - Process liveness detection
# - Debug utilities
# - Sanitizer detection

cmake_minimum_required(VERSION 3.29)

message(STATUS "Configuring Layer 0 (Platform) tests...")

# Ensure pylabhub-utils is staged before test discovery tries to load it.
macro(layer0_require_staged_utils target)
    if(TARGET stage_pylabhub_utils)
        add_dependencies(${target} stage_pylabhub_utils)
    endif()
endmacro()

# ============================================================================
# Test: Platform Core APIs
# ============================================================================

set(TEST_PLATFORM_CORE_SOURCES
    test_platform_core.cpp
)

add_executable(test_layer0_platform_core ${TEST_PLATFORM_CORE_SOURCES})

target_link_libraries(test_layer0_platform_core
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
        GTest::gmock
)

# Register for staging
pylabhub_register_test_for_staging(
    TARGET test_layer0_platform_core
    SUITE_NAME "Layer0_Platform_Core"
)
layer0_require_staged_utils(test_layer0_platform_core)

# Register with CTest
gtest_discover_tests(test_layer0_platform_core
    PROPERTIES
        LABELS "layer0;platform;core"
        TIMEOUT 30
)

# ============================================================================
# Test: Platform Debug Utilities
# ============================================================================

set(TEST_PLATFORM_DEBUG_SOURCES
    test_platform_debug.cpp
)

add_executable(test_layer0_platform_debug ${TEST_PLATFORM_DEBUG_SOURCES})

target_link_libraries(test_layer0_platform_debug
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
        GTest::gmock
)

# Register for staging
pylabhub_register_test_for_staging(
    TARGET test_layer0_platform_debug
    SUITE_NAME "Layer0_Platform_Debug"
)
layer0_require_staged_utils(test_layer0_platform_debug)

# Register with CTest
gtest_discover_tests(test_layer0_platform_debug
    PROPERTIES
        LABELS "layer0;platform;debug"
        TIMEOUT 30
)

# ============================================================================
# Test: Platform Sanitizers
# ============================================================================

set(TEST_PLATFORM_SANITIZERS_SOURCES
    test_platform_sanitizers.cpp
)

add_executable(test_layer0_platform_sanitizers ${TEST_PLATFORM_SANITIZERS_SOURCES})

target_link_libraries(test_layer0_platform_sanitizers
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
        GTest::gmock
)

# Register for staging
pylabhub_register_test_for_staging(
    TARGET test_layer0_platform_sanitizers
    SUITE_NAME "Layer0_Platform_Sanitizers"
)
layer0_require_staged_utils(test_layer0_platform_sanitizers)

# Register with CTest
gtest_discover_tests(test_layer0_platform_sanitizers
    PROPERTIES
        LABELS "layer0;platform;sanitizer"
        TIMEOUT 60  # Sanitizer tests may take longer
)

message(STATUS "Layer 0 (Platform) tests configured successfully")
message(STATUS "  - test_layer0_platform_core")
message(STATUS "  - test_layer0_platform_debug")
message(STATUS "  - test_layer0_platform_sanitizers")
