# tests/test_layer1_base/CMakeLists.txt
#
# Layer 1 (Base) tests - Pure API tests (no lifecycle required)
#
# These tests verify base utilities built on plh_platform.hpp:
# - Guards (atomic, recursion, scope)
# - Format tools
# - Debug info
# - Module definitions

cmake_minimum_required(VERSION 3.29)

message(STATUS "Configuring Layer 1 (Base) tests...")

macro(layer1_require_staged_utils target)
    if(TARGET stage_pylabhub_utils)
        add_dependencies(${target} stage_pylabhub_utils)
    endif()
endmacro()

# ============================================================================
# Test: In-process spinlock (unified owner, token mode; replaces AtomicGuard)
# ============================================================================

add_executable(test_layer1_spinlock
    test_spinlock.cpp
)

target_link_libraries(test_layer1_spinlock
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
)

pylabhub_register_test_for_staging(
    TARGET test_layer1_spinlock
    SUITE_NAME "Layer1_Spinlock"
)
layer1_require_staged_utils(test_layer1_spinlock)

gtest_discover_tests(test_layer1_spinlock
    PROPERTIES
        LABELS "layer1;base;guard;spinlock"
        TIMEOUT 120
)

# ============================================================================
# Test: RecursionGuard
# ============================================================================

add_executable(test_layer1_recursion_guard
    test_recursionguard.cpp
)

target_link_libraries(test_layer1_recursion_guard
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
)

pylabhub_register_test_for_staging(
    TARGET test_layer1_recursion_guard
    SUITE_NAME "Layer1_RecursionGuard"
)
layer1_require_staged_utils(test_layer1_recursion_guard)

gtest_discover_tests(test_layer1_recursion_guard
    PROPERTIES
        LABELS "layer1;base;guard;recursion"
        TIMEOUT 30
)

# ============================================================================
# Test: ScopeGuard
# ============================================================================

add_executable(test_layer1_scope_guard
    test_scopeguard.cpp
)

target_link_libraries(test_layer1_scope_guard
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
)

pylabhub_register_test_for_staging(
    TARGET test_layer1_scope_guard
    SUITE_NAME "Layer1_ScopeGuard"
)
layer1_require_staged_utils(test_layer1_scope_guard)

gtest_discover_tests(test_layer1_scope_guard
    PROPERTIES
        LABELS "layer1;base;guard;scope"
        TIMEOUT 30
)

# ============================================================================
# Test: Format Tools
# ============================================================================

add_executable(test_layer1_format_tools
    test_formattable.cpp
)

target_link_libraries(test_layer1_format_tools
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
)

pylabhub_register_test_for_staging(
    TARGET test_layer1_format_tools
    SUITE_NAME "Layer1_FormatTools"
)
layer1_require_staged_utils(test_layer1_format_tools)

gtest_discover_tests(test_layer1_format_tools
    PROPERTIES
        LABELS "layer1;base;format"
        TIMEOUT 30
)

# ============================================================================
# Test: Result<T,E>
# Generic error-handling type (independent of datahub; belongs at base layer)
# Moved here from test_raii_layer/ (T5.1: Result is a layer-1 utility)
# ============================================================================

add_executable(test_layer1_result
    test_result.cpp
)

target_link_libraries(test_layer1_result
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
)

target_compile_features(test_layer1_result PRIVATE cxx_std_20)

pylabhub_register_test_for_staging(
    TARGET test_layer1_result
    SUITE_NAME "Layer1_Result"
)
layer1_require_staged_utils(test_layer1_result)

gtest_discover_tests(test_layer1_result
    PROPERTIES
        LABELS "layer1;base;result"
        TIMEOUT 30
)

message(STATUS "Layer 1 (Base) tests configured successfully")
message(STATUS "  - test_layer1_spinlock")
message(STATUS "  - test_layer1_recursion_guard")
message(STATUS "  - test_layer1_scope_guard")
message(STATUS "  - test_layer1_format_tools")
message(STATUS "  - test_layer1_result")
