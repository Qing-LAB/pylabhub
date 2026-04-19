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

# ── Net Address Validation ──────────────────────────────────────────────────

add_executable(test_layer2_net_address
    test_net_address.cpp
)

target_link_libraries(test_layer2_net_address
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_net_address)

pylabhub_register_test_for_staging(
    TARGET test_layer2_net_address
    SUITE_NAME "Layer2_NetAddress"
)
layer2_require_staged_utils(test_layer2_net_address)

gtest_discover_tests(test_layer2_net_address
    PROPERTIES
        LABELS "layer2;service;net"
        TIMEOUT 30
)

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
    workers/filelock_singleprocess_workers.cpp
)

target_include_directories(test_layer2_filelock_singleprocess PRIVATE workers)

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

target_include_directories(test_layer2_jsonconfig PRIVATE workers)

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
# Test: ZmqContext lifecycle (Pattern 3 — workers own LifecycleGuard)
# ============================================================================

add_executable(test_layer2_zmq_context
    test_zmq_context.cpp
    workers/zmq_context_workers.cpp
)

target_include_directories(test_layer2_zmq_context PRIVATE workers)

target_link_libraries(test_layer2_zmq_context
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
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
    workers/signal_handler_workers.cpp
)

target_include_directories(test_layer2_interactive_signal_handler PRIVATE workers)

target_link_libraries(test_layer2_interactive_signal_handler
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
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

# set(PYLABHUB_SKIP_ENGINE_TESTS TRUE)  # Phase 3/4/5: test migration in progress

# ============================================================================
# Test: SlotView helpers (make_slot_view + wrap_as_readonly_ctypes)
# Requires pylabhub-scripting (pybind11 embedded Python interpreter).
# ============================================================================
if(NOT PYLABHUB_SKIP_ENGINE_TESTS)

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
# Test: role_cli::parse_role_args + ParseResult (in-process, no std::exit)
# ============================================================================

add_executable(test_layer2_role_cli test_role_cli.cpp)
target_link_libraries(test_layer2_role_cli PRIVATE
    pylabhub::utils GTest::gtest GTest::gtest_main)
pylabhub_ensure_sanitizer_for_executable(test_layer2_role_cli)
pylabhub_register_test_for_staging(
    TARGET test_layer2_role_cli SUITE_NAME "Layer2_RoleCli")
layer2_require_staged_utils(test_layer2_role_cli)
gtest_discover_tests(test_layer2_role_cli
    PROPERTIES LABELS "layer2;service;role_cli" TIMEOUT 30)

# ============================================================================
# Test: RoleRegistry + RuntimeBuilder
# ============================================================================

add_executable(test_layer2_role_registry test_role_registry.cpp)
target_link_libraries(test_layer2_role_registry PRIVATE
    pylabhub::utils GTest::gtest GTest::gtest_main)
pylabhub_ensure_sanitizer_for_executable(test_layer2_role_registry)
pylabhub_register_test_for_staging(
    TARGET test_layer2_role_registry SUITE_NAME "Layer2_RoleRegistry")
layer2_require_staged_utils(test_layer2_role_registry)
gtest_discover_tests(test_layer2_role_registry
    PROPERTIES LABELS "layer2;service;role_registry" TIMEOUT 30)

# ============================================================================
# Test: make_engine_from_script_config (scripting layer helper)
# ============================================================================

add_executable(test_layer2_engine_factory test_engine_factory.cpp)
target_include_directories(test_layer2_engine_factory PRIVATE
    ${CMAKE_SOURCE_DIR}/src/scripting)
target_link_libraries(test_layer2_engine_factory PRIVATE
    pylabhub::utils
    pylabhub::scripting
    pylabhub::third_party::pybind11_embed
    GTest::gtest GTest::gtest_main)
pylabhub_ensure_sanitizer_for_executable(test_layer2_engine_factory)
pylabhub_register_test_for_staging(
    TARGET test_layer2_engine_factory SUITE_NAME "Layer2_EngineFactory")
layer2_require_staged_utils(test_layer2_engine_factory)
gtest_discover_tests(test_layer2_engine_factory
    PROPERTIES LABELS "layer2;service;engine_factory" TIMEOUT 30)

# ============================================================================
# Test: RoleDirectory::init_directory with LogInitOverrides across 3 roles
# ============================================================================

add_executable(test_layer2_role_init_directory test_role_init_directory.cpp)
target_include_directories(test_layer2_role_init_directory PRIVATE
    ${CMAKE_SOURCE_DIR}/src/producer
    ${CMAKE_SOURCE_DIR}/src/consumer
    ${CMAKE_SOURCE_DIR}/src/processor)
target_link_libraries(test_layer2_role_init_directory PRIVATE
    pylabhub::utils GTest::gtest GTest::gtest_main)
pylabhub_ensure_sanitizer_for_executable(test_layer2_role_init_directory)
pylabhub_register_test_for_staging(
    TARGET test_layer2_role_init_directory
    SUITE_NAME "Layer2_RoleInitDirectory")
layer2_require_staged_utils(test_layer2_role_init_directory)
gtest_discover_tests(test_layer2_role_init_directory
    PROPERTIES LABELS "layer2;service;role_init_directory" TIMEOUT 60)

# ============================================================================
# Test: CLI log flags → JSON → LoggingConfig round-trip (all 3 roles)
#
# Pattern 3 (IsolatedProcessTest): parent spawns workers; each worker owns
# its own LifecycleGuard via run_gtest_worker. No in-process lifecycle.
# See workers/role_logging_workers.{h,cpp} and
# docs/tech_draft/test_compliance_audit.md.
# ============================================================================

add_executable(test_layer2_role_logging_roundtrip
    test_role_logging_roundtrip.cpp
    workers/role_logging_workers.cpp)
target_include_directories(test_layer2_role_logging_roundtrip PRIVATE
    workers
    ${CMAKE_SOURCE_DIR}/src/producer
    ${CMAKE_SOURCE_DIR}/src/consumer
    ${CMAKE_SOURCE_DIR}/src/processor)
target_link_libraries(test_layer2_role_logging_roundtrip PRIVATE
    pylabhub::utils
    pylabhub::test_framework
    GTest::gtest
    GTest::gmock)
pylabhub_ensure_sanitizer_for_executable(test_layer2_role_logging_roundtrip)
pylabhub_register_test_for_staging(
    TARGET test_layer2_role_logging_roundtrip
    SUITE_NAME "Layer2_RoleLoggingRoundtrip")
layer2_require_staged_utils(test_layer2_role_logging_roundtrip)
gtest_discover_tests(test_layer2_role_logging_roundtrip
    PROPERTIES LABELS "layer2;service;role_logging_roundtrip" TIMEOUT 60)

# ============================================================================
# Test: configure_logger_from_config — auto-path, rotation params, errors
#
# Shares the role_logging_workers.cpp worker file with the round-trip test.
# ============================================================================

add_executable(test_layer2_configure_logger
    test_configure_logger.cpp
    workers/role_logging_workers.cpp)
target_include_directories(test_layer2_configure_logger PRIVATE
    workers
    ${CMAKE_SOURCE_DIR}/src/producer
    ${CMAKE_SOURCE_DIR}/src/consumer
    ${CMAKE_SOURCE_DIR}/src/processor)
target_link_libraries(test_layer2_configure_logger PRIVATE
    pylabhub::utils
    pylabhub::test_framework
    GTest::gtest
    GTest::gmock)
pylabhub_ensure_sanitizer_for_executable(test_layer2_configure_logger)
pylabhub_register_test_for_staging(
    TARGET test_layer2_configure_logger
    SUITE_NAME "Layer2_ConfigureLogger")
layer2_require_staged_utils(test_layer2_configure_logger)
gtest_discover_tests(test_layer2_configure_logger
    PROPERTIES LABELS "layer2;service;configure_logger" TIMEOUT 60)

# ============================================================================
# Test: RoleHostBase lifecycle (Pattern 3 — spawned workers + panic/abort)
#
# Non-abort workers use run_gtest_worker; the two abort workers install a
# manual LifecycleGuard and rely on PLH_PANIC → std::abort() to terminate
# the subprocess. The parent asserts exit_code != 0 + panic message.
# ============================================================================

add_executable(test_layer2_role_host_base
    test_role_host_base.cpp
    workers/role_host_base_workers.cpp)
target_include_directories(test_layer2_role_host_base PRIVATE
    workers
    ${CMAKE_SOURCE_DIR}/src/producer)
target_link_libraries(test_layer2_role_host_base PRIVATE
    pylabhub::utils
    pylabhub::test_framework
    GTest::gtest
    GTest::gmock)
pylabhub_ensure_sanitizer_for_executable(test_layer2_role_host_base)
pylabhub_register_test_for_staging(
    TARGET test_layer2_role_host_base
    SUITE_NAME "Layer2_RoleHostBase")
layer2_require_staged_utils(test_layer2_role_host_base)
gtest_discover_tests(test_layer2_role_host_base
    PROPERTIES LABELS "layer2;service;role_host_base" TIMEOUT 60)

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

# ── NativeEngine test modules (.so) ──────────────────────────────────────
add_library(test_good_producer_plugin SHARED
    test_plugins/good_producer_plugin.cpp
)
target_include_directories(test_good_producer_plugin PRIVATE
    ${CMAKE_SOURCE_DIR}/src/include
)
target_link_libraries(test_good_producer_plugin PRIVATE pylabhub::utils)
target_compile_features(test_good_producer_plugin PRIVATE cxx_std_20)
set_target_properties(test_good_producer_plugin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${PYLABHUB_STAGING_DIR}/tests"
)

add_library(test_native_multifield_module SHARED
    test_plugins/native_multifield_module.cpp
)
target_include_directories(test_native_multifield_module PRIVATE
    ${CMAKE_SOURCE_DIR}/src/include
)
target_link_libraries(test_native_multifield_module PRIVATE pylabhub::utils)
target_compile_features(test_native_multifield_module PRIVATE cxx_std_20)
set_target_properties(test_native_multifield_module PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${PYLABHUB_STAGING_DIR}/tests"
)

# ── NativeEngine test binary ─────────────────────────────────────────────
add_executable(test_layer2_scriptengine_native_dylib
    test_scriptengine_native_dylib.cpp
    workers/scriptengine_native_dylib_workers.cpp
)
target_include_directories(test_layer2_scriptengine_native_dylib PRIVATE
    workers
    ${CMAKE_SOURCE_DIR}/src/scripting
    ${CMAKE_SOURCE_DIR}/tests/test_framework
)
target_link_libraries(test_layer2_scriptengine_native_dylib PRIVATE
    pylabhub::utils
    pylabhub::scripting
    pylabhub::test_framework
    GTest::gtest
)
target_compile_definitions(test_layer2_scriptengine_native_dylib PRIVATE
    LOGGER_COMPILE_LEVEL=0
    TEST_PLUGIN_DIR="${PYLABHUB_STAGING_DIR}/tests"
)
add_dependencies(test_layer2_scriptengine_native_dylib test_good_producer_plugin test_native_multifield_module)
# No -rdynamic needed: plugin API goes through PlhPluginContext function pointers,
# not host-exported symbols.
pylabhub_ensure_sanitizer_for_executable(test_layer2_scriptengine_native_dylib)
pylabhub_register_test_for_staging(
    TARGET test_layer2_scriptengine_native_dylib
    SUITE_NAME "Layer2_NativeEngine"
)
layer2_require_staged_utils(test_layer2_scriptengine_native_dylib)
gtest_discover_tests(test_layer2_scriptengine_native_dylib
    PROPERTIES
        LABELS "layer2;service;scripting;native;engine"
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
    workers/role_config_workers.cpp
)

target_include_directories(test_layer2_role_config PRIVATE workers)

target_link_libraries(test_layer2_role_config
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gmock
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

# ============================================================================
# Test: SchemaValidation — parse_schema_json, compute_schema_size, to_field_descs
# ============================================================================

add_executable(test_layer2_schema_validation
    test_schema_validation.cpp
)

target_link_libraries(test_layer2_schema_validation
    PRIVATE
        pylabhub::utils
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_schema_validation)

pylabhub_register_test_for_staging(
    TARGET test_layer2_schema_validation
    SUITE_NAME "Layer2_SchemaValidation"
)
layer2_require_staged_utils(test_layer2_schema_validation)

gtest_discover_tests(test_layer2_schema_validation
    PROPERTIES
        LABELS "layer2;service;schema"
        TIMEOUT 30
)

# ============================================================================
# Test: MetricsAPI — hierarchical metrics serialization (JSON + py::dict)
# ============================================================================

add_executable(test_layer2_metrics_api
    test_metrics_api.cpp
    workers/metrics_api_workers.cpp
)

target_include_directories(test_layer2_metrics_api PRIVATE
    workers
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/producer
    ${CMAKE_SOURCE_DIR}/src/consumer
    ${CMAKE_SOURCE_DIR}/src/processor
    ${CMAKE_SOURCE_DIR}/src/scripting
)

target_link_libraries(test_layer2_metrics_api
    PRIVATE
        pylabhub::utils
        pylabhub::scripting
        pylabhub::test_framework
        pylabhub::third_party::pybind11_embed
        GTest::gtest
        GTest::gmock
)

target_compile_features(test_layer2_metrics_api PRIVATE cxx_std_20)
target_compile_definitions(test_layer2_metrics_api PRIVATE LOGGER_COMPILE_LEVEL=0)

pylabhub_ensure_sanitizer_for_executable(test_layer2_metrics_api)

pylabhub_register_test_for_staging(
    TARGET test_layer2_metrics_api
    SUITE_NAME "Layer2_MetricsAPI"
)
layer2_require_staged_utils(test_layer2_metrics_api)

gtest_discover_tests(test_layer2_metrics_api
    PROPERTIES
        LABELS "layer2;service;metrics"
        TIMEOUT 60
    DISCOVERY_TIMEOUT 60
)

# ============================================================================
# Test: RoleDataLoop — retry_acquire, run_data_loop, thread manager
# ============================================================================

add_executable(test_layer2_role_data_loop
    test_role_data_loop.cpp
    workers/role_data_loop_workers.cpp
)

target_include_directories(test_layer2_role_data_loop PRIVATE
    workers
    ${CMAKE_SOURCE_DIR}/src/utils   # internal headers (service/data_loop.hpp, service/cycle_ops.hpp)
)

target_link_libraries(test_layer2_role_data_loop
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_role_data_loop)

pylabhub_register_test_for_staging(
    TARGET test_layer2_role_data_loop
    SUITE_NAME "Layer2_RoleDataLoop"
)
layer2_require_staged_utils(test_layer2_role_data_loop)

gtest_discover_tests(test_layer2_role_data_loop
    PROPERTIES
        LABELS "layer2;service;dataloop"
        TIMEOUT 30
)
endif() # PYLABHUB_SKIP_ENGINE_TESTS
