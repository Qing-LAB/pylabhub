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

cmake_minimum_required(VERSION 3.28)

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

pylabhub_ensure_sanitizer_for_executable(test_layer2_framework_selftest)

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

pylabhub_ensure_sanitizer_for_executable(test_layer2_backoff_strategy)

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
# Test: SharedSpinLock (foundational for DataBlock; Part 0 of test plan)
# ============================================================================

add_executable(test_layer2_shared_memory_spinlock
    test_shared_memory_spinlock.cpp
    workers/spinlock_workers.cpp
)

target_include_directories(test_layer2_shared_memory_spinlock PRIVATE workers)

target_link_libraries(test_layer2_shared_memory_spinlock
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
        GTest::gmock
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_shared_memory_spinlock)

pylabhub_register_test_for_staging(
    TARGET test_layer2_shared_memory_spinlock
    SUITE_NAME "Layer2_SharedMemorySpinlock"
)
layer2_require_staged_utils(test_layer2_shared_memory_spinlock)

gtest_discover_tests(test_layer2_shared_memory_spinlock
    PROPERTIES
        LABELS "layer2;service;spinlock"
        TIMEOUT 30
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

pylabhub_ensure_sanitizer_for_executable(test_layer2_crypto_utils)

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

pylabhub_ensure_sanitizer_for_executable(test_layer2_lifecycle)

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

pylabhub_ensure_sanitizer_for_executable(test_layer2_lifecycle_dynamic)

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

pylabhub_ensure_sanitizer_for_executable(test_layer2_filelock)

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

pylabhub_ensure_sanitizer_for_executable(test_layer2_filelock_singleprocess)

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

pylabhub_ensure_sanitizer_for_executable(test_layer2_logger)

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

pylabhub_ensure_sanitizer_for_executable(test_layer2_jsonconfig)

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

# ============================================================================
# Test: HubVault (pure API — no lifecycle, no worker dispatcher)
# Note: Argon2id KDF takes ~0.5s per call; timeout set to 120s.
# ============================================================================

add_executable(test_layer2_hub_vault
    test_hub_vault.cpp
)

target_link_libraries(test_layer2_hub_vault
    PRIVATE
        pylabhub::utils
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_hub_vault)

pylabhub_register_test_for_staging(
    TARGET test_layer2_hub_vault
    SUITE_NAME "Layer2_HubVault"
)
layer2_require_staged_utils(test_layer2_hub_vault)

gtest_discover_tests(test_layer2_hub_vault
    PROPERTIES
        LABELS "layer2;service;vault;security"
        TIMEOUT 120
)

# Legacy test_layer2_script_host removed — ScriptHost replaced by ScriptEngine.

# ============================================================================
# Test: RoleVault (pure API — no lifecycle, no worker dispatcher)
# Note: Argon2id KDF takes ~0.5s per call; timeout set to 120s.
# ============================================================================

add_executable(test_layer2_role_vault
    test_role_vault.cpp
)

target_link_libraries(test_layer2_role_vault
    PRIVATE
        pylabhub::utils
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_role_vault)

pylabhub_register_test_for_staging(
    TARGET test_layer2_role_vault
    SUITE_NAME "Layer2_RoleVault"
)
layer2_require_staged_utils(test_layer2_role_vault)

gtest_discover_tests(test_layer2_role_vault
    PROPERTIES
        LABELS "layer2;service;vault;security"
        TIMEOUT 120
)

# ============================================================================
# Test: ZmqContext lifecycle (uses LifecycleGuard)
# ============================================================================

add_executable(test_layer2_zmq_context
    test_zmq_context.cpp
)

target_link_libraries(test_layer2_zmq_context
    PRIVATE
        pylabhub::utils
        GTest::gtest
        GTest::gtest_main
)

target_compile_definitions(test_layer2_zmq_context PRIVATE LOGGER_COMPILE_LEVEL=0)

pylabhub_ensure_sanitizer_for_executable(test_layer2_zmq_context)

pylabhub_register_test_for_staging(
    TARGET test_layer2_zmq_context
    SUITE_NAME "Layer2_ZmqContext"
)
layer2_require_staged_utils(test_layer2_zmq_context)

gtest_discover_tests(test_layer2_zmq_context
    PROPERTIES
        LABELS "layer2;service;zmq"
        TIMEOUT 30
)

# ============================================================================
# Test: UID Utils (pure API — no lifecycle, no worker dispatcher)
# ============================================================================

add_executable(test_layer2_uid_utils
    test_uid_utils.cpp
)

target_link_libraries(test_layer2_uid_utils
    PRIVATE
        pylabhub::utils
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_uid_utils)

pylabhub_register_test_for_staging(
    TARGET test_layer2_uid_utils
    SUITE_NAME "Layer2_UidUtils"
)
layer2_require_staged_utils(test_layer2_uid_utils)

gtest_discover_tests(test_layer2_uid_utils
    PROPERTIES
        LABELS "layer2;service;uid"
        TIMEOUT 30
)

# ============================================================================
# Test: InteractiveSignalHandler lifecycle (pure API — no signal injection)
# ============================================================================

add_executable(test_layer2_interactive_signal_handler
    test_interactive_signal_handler.cpp
)

target_link_libraries(test_layer2_interactive_signal_handler
    PRIVATE
        pylabhub::utils
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_interactive_signal_handler)

pylabhub_register_test_for_staging(
    TARGET test_layer2_interactive_signal_handler
    SUITE_NAME "Layer2_InteractiveSignalHandler"
)
layer2_require_staged_utils(test_layer2_interactive_signal_handler)

gtest_discover_tests(test_layer2_interactive_signal_handler
    PROPERTIES
        LABELS "layer2;service;signal"
        TIMEOUT 30
)

# ============================================================================
# Test: SlotView helpers (make_slot_view + wrap_as_readonly_ctypes)
# Requires pylabhub-scripting (pybind11 embedded Python interpreter).
# ============================================================================

add_executable(test_layer2_slot_view_helpers
    test_slot_view_helpers.cpp
)

target_include_directories(test_layer2_slot_view_helpers PRIVATE
    ${CMAKE_SOURCE_DIR}/src/scripting
)

target_link_libraries(test_layer2_slot_view_helpers
    PRIVATE
        pylabhub::scripting
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_slot_view_helpers)

pylabhub_register_test_for_staging(
    TARGET test_layer2_slot_view_helpers
    SUITE_NAME "Layer2_SlotViewHelpers"
)
layer2_require_staged_utils(test_layer2_slot_view_helpers)

gtest_discover_tests(test_layer2_slot_view_helpers
    PROPERTIES
        LABELS "layer2;service;scripting;slotview"
        TIMEOUT 60
)

# ============================================================================
# Test: RoleDirectory + role_cli (pure filesystem API — no lifecycle)
# ============================================================================

add_executable(test_layer2_role_directory
    test_role_directory.cpp
)

target_link_libraries(test_layer2_role_directory
    PRIVATE
        pylabhub::utils
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_role_directory)

pylabhub_register_test_for_staging(
    TARGET test_layer2_role_directory
    SUITE_NAME "Layer2_RoleDirectory"
)
layer2_require_staged_utils(test_layer2_role_directory)

gtest_discover_tests(test_layer2_role_directory
    PROPERTIES
        LABELS "layer2;service;role_directory"
        TIMEOUT 30
)

# ============================================================================
# Test: LoopTimingPolicy (pure header-only API)
# ============================================================================

add_executable(test_layer2_loop_timing_policy
    test_loop_timing_policy.cpp
)

target_link_libraries(test_layer2_loop_timing_policy
    PRIVATE
        pylabhub::utils
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_loop_timing_policy)

pylabhub_register_test_for_staging(
    TARGET test_layer2_loop_timing_policy
    SUITE_NAME "Layer2_LoopTimingPolicy"
)
layer2_require_staged_utils(test_layer2_loop_timing_policy)

gtest_discover_tests(test_layer2_loop_timing_policy
    PROPERTIES
        LABELS "layer2;service;loop_timing"
        TIMEOUT 30
)

# --- LuaEngine unit tests ---
add_executable(test_layer2_lua_engine
    test_lua_engine.cpp
)

target_include_directories(test_layer2_lua_engine PRIVATE
    ${CMAKE_SOURCE_DIR}/src/scripting
    ${CMAKE_SOURCE_DIR}/tests/test_framework
)

target_link_libraries(test_layer2_lua_engine
    PRIVATE
        pylabhub::utils
        pylabhub::scripting
        GTest::gtest
        GTest::gtest_main
)

target_compile_definitions(test_layer2_lua_engine PRIVATE LOGGER_COMPILE_LEVEL=0)

pylabhub_ensure_sanitizer_for_executable(test_layer2_lua_engine)

pylabhub_register_test_for_staging(
    TARGET test_layer2_lua_engine
    SUITE_NAME "Layer2_LuaEngine"
)
layer2_require_staged_utils(test_layer2_lua_engine)

gtest_discover_tests(test_layer2_lua_engine
    PROPERTIES
        LABELS "layer2;service;scripting;lua;engine"
        TIMEOUT 30
)

# --- PythonEngine unit tests ---
add_executable(test_layer2_python_engine
    test_python_engine.cpp
)

target_include_directories(test_layer2_python_engine PRIVATE
    ${CMAKE_SOURCE_DIR}/src/scripting
    ${CMAKE_SOURCE_DIR}/tests/test_framework
)

target_link_libraries(test_layer2_python_engine
    PRIVATE
        pylabhub::utils
        pylabhub::scripting
        pylabhub::third_party::pybind11_embed
        GTest::gtest
        GTest::gtest_main
)

target_compile_definitions(test_layer2_python_engine PRIVATE LOGGER_COMPILE_LEVEL=0)

pylabhub_ensure_sanitizer_for_executable(test_layer2_python_engine)

pylabhub_register_test_for_staging(
    TARGET test_layer2_python_engine
    SUITE_NAME "Layer2_PythonEngine"
)
layer2_require_staged_utils(test_layer2_python_engine)

gtest_discover_tests(test_layer2_python_engine
    PROPERTIES
        LABELS "layer2;service;scripting;python;engine"
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
message(STATUS "  - test_layer2_script_host        (pure unit, ScriptHost threading)")

# SlotRWCoordinator is a pure C API over SlotRWState + SharedMemoryHeader.
# The dedicated test below exercises it in isolation (no DataBlock / shm),
# so higher-level tests can rely on a verified lower layer.

add_executable(test_layer2_slot_rw_coordinator
    test_slot_rw_coordinator.cpp
)

target_link_libraries(test_layer2_slot_rw_coordinator
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_slot_rw_coordinator)

pylabhub_register_test_for_staging(
    TARGET test_layer2_slot_rw_coordinator
    SUITE_NAME "Layer2_SlotRWCoordinator"
)
layer2_require_staged_utils(test_layer2_slot_rw_coordinator)

gtest_discover_tests(test_layer2_slot_rw_coordinator
    PROPERTIES
        LABELS "layer2;service;slotrw"
        TIMEOUT 60
)

# ============================================================================
# Test: RoleHostCore — shared state container for role hosts
# ============================================================================

add_executable(test_layer2_role_host_core
    test_role_host_core.cpp
)

target_link_libraries(test_layer2_role_host_core
    PRIVATE
        pylabhub::utils
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_role_host_core)

pylabhub_register_test_for_staging(
    TARGET test_layer2_role_host_core
    SUITE_NAME "Layer2_RoleHostCore"
)
layer2_require_staged_utils(test_layer2_role_host_core)

gtest_discover_tests(test_layer2_role_host_core
    PROPERTIES
        LABELS "layer2;service;scripting"
        TIMEOUT 30
)

# ============================================================================
# Test: RoleConfig — unified config class
# ============================================================================

add_executable(test_layer2_role_config
    test_role_config.cpp
)

target_link_libraries(test_layer2_role_config
    PRIVATE
        pylabhub::utils
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_role_config)

pylabhub_register_test_for_staging(
    TARGET test_layer2_role_config
    SUITE_NAME "Layer2_RoleConfig"
)
layer2_require_staged_utils(test_layer2_role_config)

gtest_discover_tests(test_layer2_role_config
    PROPERTIES
        LABELS "layer2;service;config"
        TIMEOUT 30
)
