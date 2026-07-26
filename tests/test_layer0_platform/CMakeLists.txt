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

pylabhub_ensure_sanitizer_for_executable(test_layer0_platform_core)

# Register for staging
layer0_require_staged_utils(test_layer0_platform_core)

# Register with CTest
pylabhub_add_gtest(TARGET test_layer0_platform_core
    LABELS "layer0;platform;core"
    TIMEOUT 30)

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

pylabhub_ensure_sanitizer_for_executable(test_layer0_platform_debug)

# Register for staging
layer0_require_staged_utils(test_layer0_platform_debug)

# Register with CTest
pylabhub_add_gtest(TARGET test_layer0_platform_debug
    LABELS "layer0;platform;debug"
    TIMEOUT 30)

# ============================================================================
# Test: Platform Shared Memory (shm_create, shm_attach, shm_close, shm_unlink)
# ============================================================================

set(TEST_PLATFORM_SHM_SOURCES
    test_platform_shm.cpp
)

add_executable(test_layer0_platform_shm ${TEST_PLATFORM_SHM_SOURCES})

target_link_libraries(test_layer0_platform_shm
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
        GTest::gmock
)

pylabhub_ensure_sanitizer_for_executable(test_layer0_platform_shm)

layer0_require_staged_utils(test_layer0_platform_shm)

pylabhub_add_gtest(TARGET test_layer0_platform_shm
    LABELS "layer0;platform;shm"
    TIMEOUT 30)

# ============================================================================
# Test: Platform Sanitizers
# ============================================================================

add_executable(test_layer0_platform_sanitizers test_platform_sanitizers.cpp)

target_link_libraries(test_layer0_platform_sanitizers
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gmock
)

pylabhub_ensure_sanitizer_for_executable(test_layer0_platform_sanitizers)

# Register for staging
layer0_require_staged_utils(test_layer0_platform_sanitizers)

# Register with CTest (tests assert on stderr content only, not exit code)
pylabhub_add_gtest(TARGET test_layer0_platform_sanitizers
    LABELS "layer0;platform;sanitizer"
    TIMEOUT 60)

# ============================================================================
# Test: UUID Utils and Format Tools (bytes_to_hex, bytes_from_hex)
# ============================================================================

add_executable(test_layer0_uuid_and_format test_uuid_and_format.cpp)

target_link_libraries(test_layer0_uuid_and_format
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer0_uuid_and_format)

layer0_require_staged_utils(test_layer0_uuid_and_format)

pylabhub_add_gtest(TARGET test_layer0_uuid_and_format
    LABELS "layer0;platform;uuid;format"
    TIMEOUT 30)

# ============================================================================
# Test: ABI check facility (HEP-CORE-0032)
# ============================================================================

add_executable(test_layer0_abi_check test_abi_check.cpp)

target_link_libraries(test_layer0_abi_check
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer0_abi_check)

layer0_require_staged_utils(test_layer0_abi_check)

pylabhub_add_gtest(TARGET test_layer0_abi_check
    LABELS "layer0;platform;abi"
    TIMEOUT 30)

message(STATUS "Layer 0 (Platform) tests configured successfully")
message(STATUS "  - test_layer0_platform_core")
message(STATUS "  - test_layer0_platform_debug")
message(STATUS "  - test_layer0_platform_shm")
message(STATUS "  - test_layer0_platform_sanitizers")
message(STATUS "  - test_layer0_uuid_and_format")
message(STATUS "  - test_layer0_abi_check")
