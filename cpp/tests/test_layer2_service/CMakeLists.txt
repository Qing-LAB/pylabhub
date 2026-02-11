# tests/test_layer2_service/CMakeLists.txt
#
# Layer 2 (Service) tests - Lifecycle-managed services
#
# Structure:
#   test_*.cpp          — GTest test suites (one file per module)
#   workers/            — Worker process implementations (spawned by tests as subprocesses)
#
# Each test executable links only the worker file(s) it uses.
# Each worker .cpp self-registers its dispatcher via a static initializer.

cmake_minimum_required(VERSION 3.29)

message(STATUS "Configuring Layer 2 (Service) tests...")

# Ensure pylabhub-utils is staged (soname symlink in stage-debug/lib/) before
# any test binary runs test discovery via RPATH ($ORIGIN/../lib).
# Without this, a parallel build may try to discover tests before the .so
# symlink exists in the staging directory.
function(layer2_require_staged_utils target)
    if(TARGET stage_pylabhub_utils)
        add_dependencies(${target} stage_pylabhub_utils)
    endif()
endfunction()

# ============================================================================
# CRITICAL: Framework Self-Test
# Verifies run_gtest_worker/run_worker_bare correctly propagate failures.
# Must pass before trusting any other worker-based test.
# ============================================================================

add_executable(test_layer2_framework_selftest
    test_framework_selftest.cpp
    workers/selftest_workers.cpp
)

target_link_libraries(test_layer2_framework_selftest
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gmock
)

pylabhub_register_test_for_staging(
    TARGET test_layer2_framework_selftest
    SUITE_NAME "Layer2_FrameworkSelftest"
)
layer2_require_staged_utils(test_layer2_framework_selftest)

gtest_discover_tests(test_layer2_framework_selftest
    PROPERTIES
        LABELS "layer2;framework;selftest;critical"
        TIMEOUT 60
)

# ============================================================================
# CRITICAL Test: Backoff Strategy (blocks DataBlock tests)
# Pure API test — no lifecycle, no worker dispatcher needed.
# ============================================================================

add_executable(test_layer2_backoff_strategy
    test_backoff_strategy.cpp
)

target_link_libraries(test_layer2_backoff_strategy
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
)

pylabhub_register_test_for_staging(
    TARGET test_layer2_backoff_strategy
    SUITE_NAME "Layer2_BackoffStrategy"
)
layer2_require_staged_utils(test_layer2_backoff_strategy)

gtest_discover_tests(test_layer2_backoff_strategy
    PROPERTIES
        LABELS "layer2;service;backoff;critical"
        TIMEOUT 60
)

# ============================================================================
# CRITICAL Test: Crypto Utils (blocks schema validation)
# ============================================================================

add_executable(test_layer2_crypto_utils
    test_crypto_utils.cpp
    workers/crypto_workers.cpp
)

target_include_directories(test_layer2_crypto_utils PRIVATE workers)

target_link_libraries(test_layer2_crypto_utils
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gmock
)

pylabhub_register_test_for_staging(
    TARGET test_layer2_crypto_utils
    SUITE_NAME "Layer2_CryptoUtils"
)
layer2_require_staged_utils(test_layer2_crypto_utils)

gtest_discover_tests(test_layer2_crypto_utils
    PROPERTIES
        LABELS "layer2;service;crypto;critical"
        TIMEOUT 60
)

# ============================================================================
# Test: Lifecycle (static modules)
# ============================================================================

add_executable(test_layer2_lifecycle
    test_lifecycle.cpp
    workers/lifecycle_workers.cpp
)

target_link_libraries(test_layer2_lifecycle
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gmock
)

pylabhub_register_test_for_staging(
    TARGET test_layer2_lifecycle
    SUITE_NAME "Layer2_Lifecycle"
)
layer2_require_staged_utils(test_layer2_lifecycle)

gtest_discover_tests(test_layer2_lifecycle
    PROPERTIES
        LABELS "layer2;service;lifecycle"
        TIMEOUT 60
)

# ============================================================================
# Test: Lifecycle (dynamic modules)
# ============================================================================

add_executable(test_layer2_lifecycle_dynamic
    test_lifecycle_dynamic.cpp
    workers/lifecycle_workers.cpp
)

target_link_libraries(test_layer2_lifecycle_dynamic
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gmock
)

pylabhub_register_test_for_staging(
    TARGET test_layer2_lifecycle_dynamic
    SUITE_NAME "Layer2_LifecycleDynamic"
)
layer2_require_staged_utils(test_layer2_lifecycle_dynamic)

gtest_discover_tests(test_layer2_lifecycle_dynamic
    PROPERTIES
        LABELS "layer2;service;lifecycle;dynamic"
        TIMEOUT 60
)

# ============================================================================
# Test: FileLock (Multi-Process)
# ============================================================================

add_executable(test_layer2_filelock
    test_filelock.cpp
    workers/filelock_workers.cpp
)

target_link_libraries(test_layer2_filelock
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gmock
)

pylabhub_register_test_for_staging(
    TARGET test_layer2_filelock
    SUITE_NAME "Layer2_FileLock"
)
layer2_require_staged_utils(test_layer2_filelock)

gtest_discover_tests(test_layer2_filelock
    PROPERTIES
        LABELS "layer2;service;filelock;multiprocess"
        TIMEOUT 120
)

# ============================================================================
# Test: FileLock Single-Process (pure API, no worker dispatcher)
# ============================================================================

add_executable(test_layer2_filelock_singleprocess
    test_filelock_singleprocess.cpp
)

target_link_libraries(test_layer2_filelock_singleprocess
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
)

pylabhub_register_test_for_staging(
    TARGET test_layer2_filelock_singleprocess
    SUITE_NAME "Layer2_FileLockSingleProcess"
)
layer2_require_staged_utils(test_layer2_filelock_singleprocess)

gtest_discover_tests(test_layer2_filelock_singleprocess
    PROPERTIES
        LABELS "layer2;service;filelock;singleprocess"
        TIMEOUT 60
)

# ============================================================================
# Test: Logger
# ============================================================================

add_executable(test_layer2_logger
    test_logger.cpp
    workers/logger_workers.cpp
)

target_link_libraries(test_layer2_logger
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gmock
)

pylabhub_register_test_for_staging(
    TARGET test_layer2_logger
    SUITE_NAME "Layer2_Logger"
)
layer2_require_staged_utils(test_layer2_logger)

gtest_discover_tests(test_layer2_logger
    PROPERTIES
        LABELS "layer2;service;logger"
        TIMEOUT 60
)

# ============================================================================
# Test: JsonConfig
# ============================================================================

add_executable(test_layer2_jsonconfig
    test_jsonconfig.cpp
    workers/jsonconfig_workers.cpp
)

target_link_libraries(test_layer2_jsonconfig
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gmock
)

pylabhub_register_test_for_staging(
    TARGET test_layer2_jsonconfig
    SUITE_NAME "Layer2_JsonConfig"
)
layer2_require_staged_utils(test_layer2_jsonconfig)

gtest_discover_tests(test_layer2_jsonconfig
    PROPERTIES
        LABELS "layer2;service;jsonconfig"
        TIMEOUT 60
)

message(STATUS "Layer 2 (Service) tests configured successfully")
message(STATUS "  - test_layer2_framework_selftest (CRITICAL - framework self-verification)")
message(STATUS "  - test_layer2_backoff_strategy   (pure API, no dispatcher)")
message(STATUS "  - test_layer2_crypto_utils       (multi-process)")
message(STATUS "  - test_layer2_lifecycle          (multi-process)")
message(STATUS "  - test_layer2_lifecycle_dynamic  (multi-process)")
message(STATUS "  - test_layer2_filelock           (multi-process IPC)")
message(STATUS "  - test_layer2_filelock_singleprocess (pure API)")
message(STATUS "  - test_layer2_logger             (multi-process)")
message(STATUS "  - test_layer2_jsonconfig         (multi-process)")
