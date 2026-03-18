# tests/test_layer1_base/CMakeLists.txt
#
# Layer 1 (Base) tests - Pure API tests (no lifecycle required)
#
# These tests verify base utilities built on plh_platform.hpp:
# - Guards (atomic, recursion, scope)
# - Format tools
# - Debug info
# - Module definitions

cmake_minimum_required(VERSION 3.28)

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

pylabhub_ensure_sanitizer_for_executable(test_layer1_spinlock)

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

pylabhub_ensure_sanitizer_for_executable(test_layer1_recursion_guard)

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

pylabhub_ensure_sanitizer_for_executable(test_layer1_scope_guard)

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

pylabhub_ensure_sanitizer_for_executable(test_layer1_format_tools)

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

pylabhub_ensure_sanitizer_for_executable(test_layer1_result)

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

# ============================================================================
# Test: Backoff Strategy (header-only, pure API)
# ============================================================================

add_executable(test_layer1_backoff_strategy
    test_backoff_strategy.cpp
)

target_link_libraries(test_layer1_backoff_strategy
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer1_backoff_strategy)

pylabhub_register_test_for_staging(
    TARGET test_layer1_backoff_strategy
    SUITE_NAME "Layer1_BackoffStrategy"
)
layer1_require_staged_utils(test_layer1_backoff_strategy)

gtest_discover_tests(test_layer1_backoff_strategy
    PROPERTIES
        LABELS "layer1;base;backoff"
        TIMEOUT 30
)

# ============================================================================
# Test: PortableAtomicSharedPtr
# ============================================================================

add_executable(test_layer1_portable_atomic_shared_ptr
    test_portable_atomic_shared_ptr.cpp
)

target_include_directories(test_layer1_portable_atomic_shared_ptr
    PRIVATE
        ${CMAKE_SOURCE_DIR}/src/utils
)

target_link_libraries(test_layer1_portable_atomic_shared_ptr
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer1_portable_atomic_shared_ptr)

pylabhub_register_test_for_staging(
    TARGET test_layer1_portable_atomic_shared_ptr
    SUITE_NAME "Layer1_PortableAtomicSharedPtr"
)
layer1_require_staged_utils(test_layer1_portable_atomic_shared_ptr)

gtest_discover_tests(test_layer1_portable_atomic_shared_ptr
    PROPERTIES
        LABELS "layer1;base;atomic;shared_ptr"
        TIMEOUT 30
)

# ============================================================================
# Test: ModuleDef builder API (Pimpl, links pylabhub::utils)
# ============================================================================

add_executable(test_layer1_module_def
    test_module_def.cpp
)

target_link_libraries(test_layer1_module_def
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer1_module_def)

pylabhub_register_test_for_staging(
    TARGET test_layer1_module_def
    SUITE_NAME "Layer1_ModuleDef"
)
layer1_require_staged_utils(test_layer1_module_def)

gtest_discover_tests(test_layer1_module_def
    PROPERTIES
        LABELS "layer1;base;module"
        TIMEOUT 30
)

# ============================================================================
# Test: Debug Info (stack trace, panic, debug messaging)
# ============================================================================

add_executable(test_layer1_debug_info
    test_debug_info.cpp
)

target_link_libraries(test_layer1_debug_info
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer1_debug_info)

pylabhub_register_test_for_staging(
    TARGET test_layer1_debug_info
    SUITE_NAME "Layer1_DebugInfo"
)
layer1_require_staged_utils(test_layer1_debug_info)

gtest_discover_tests(test_layer1_debug_info
    PROPERTIES
        LABELS "layer1;base;debug"
        TIMEOUT 30
)

message(STATUS "Layer 1 (Base) tests configured successfully")
message(STATUS "  - test_layer1_spinlock")
message(STATUS "  - test_layer1_recursion_guard")
message(STATUS "  - test_layer1_scope_guard")
message(STATUS "  - test_layer1_format_tools")
message(STATUS "  - test_layer1_result")
message(STATUS "  - test_layer1_backoff_strategy")
message(STATUS "  - test_layer1_module_def")
message(STATUS "  - test_layer1_debug_info")
