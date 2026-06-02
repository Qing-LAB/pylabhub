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

# ============================================================================
# Test: ZapRouter (PeerAdmission Phase C, caller-pumped)
# Pattern 3 — worker subprocesses own LifecycleGuard for Logger +
# FileLock + JsonConfig + ZMQContext (the ZapRouter persistent-dynamic
# module's deps).  Tests use ZapPumpThread RAII helper.
# Pins:
#   - real CURVE handshake accept/deny via ZAP
#   - mid-flight allowlist swap
#   - unknown zap_domain → deny
#   - RAII handle unregister + ref-count decrement
#   - double-register refuses
#   - empty domain / null admission throw
#   - pump_one when module-not-loaded returns false (no crash)
# ============================================================================

add_executable(test_layer2_zap_router
    test_zap_router.cpp
    workers/zap_router_workers.cpp
)

target_link_libraries(test_layer2_zap_router
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gmock
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_zap_router)

pylabhub_register_test_for_staging(
    TARGET test_layer2_zap_router
    SUITE_NAME "Layer2_ZapRouter"
)
layer2_require_staged_utils(test_layer2_zap_router)

gtest_discover_tests(test_layer2_zap_router
    PROPERTIES
        LABELS "layer2;service;security;zap_router"
        TIMEOUT 60
)

# ============================================================================
# Test: ZmqQueue CURVE+ZAP auth (PeerAdmission Phase C, ZmqQueue side)
# Pattern 3 — subprocess per scenario.  Pins:
#   - allowed peer → CURVE handshake succeeds + message round-trip
#   - unallowed peer → CURVE handshake fails (no message delivery)
#   - mid-flight allowlist swap visible to next connection
#   - legacy unauth pull_from / push_to factories unchanged
# ============================================================================

add_executable(test_layer2_zmq_queue_auth
    test_zmq_queue_auth.cpp
    workers/zmq_queue_auth_workers.cpp
)

target_link_libraries(test_layer2_zmq_queue_auth
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gmock
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_zmq_queue_auth)

pylabhub_register_test_for_staging(
    TARGET test_layer2_zmq_queue_auth
    SUITE_NAME "Layer2_ZmqQueueAuth"
)
layer2_require_staged_utils(test_layer2_zmq_queue_auth)

gtest_discover_tests(test_layer2_zmq_queue_auth
    PROPERTIES
        LABELS "layer2;service;security;zmq_queue;peer_admission"
        TIMEOUT 60
)

# ============================================================================
# Test: KnownRolesStore (PeerAdmission Phase B)
# Pattern 1 — value-type + filesystem round-trip via the
# atomic_write_owner_only_file helper from key_file_acl.cpp.
# No LOGGER_*, no lifecycle.  Pins:
#   - add/remove/find/find_by_pubkey value semantics
#   - upsert-by-uid contract (replace on duplicate uid)
#   - validate_entry rejects empty uid / empty pubkey / wrong-length
#   - file round-trip: save → load reproduces state
#   - missing file → nullopt (P-Bootstrap option b)
#   - rejects unknown version / missing version / duplicate uid / bad JSON
#   - enforces ACL (mode 0600) per HEP-CORE-0035 §4.6.2
#   - as_peer_allowlist excludes empty-pubkey legacy + never unrestricted
#   - atomic_write_owner_only_file: overwrite, symlink-refusal on tmp
# ============================================================================

add_executable(test_layer2_known_roles
    test_known_roles.cpp
)

target_link_libraries(test_layer2_known_roles
    PRIVATE
        pylabhub::utils
        GTest::gtest
        GTest::gtest_main
        GTest::gmock
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_known_roles)

pylabhub_register_test_for_staging(
    TARGET test_layer2_known_roles
    SUITE_NAME "Layer2_KnownRoles"
)
layer2_require_staged_utils(test_layer2_known_roles)

gtest_discover_tests(test_layer2_known_roles
    PROPERTIES
        LABELS "layer2;service;security;known_roles"
        TIMEOUT 30
)

# ============================================================================
# Test: PeerAdmission abstraction (HEP-CORE-0035 §4.5 / HEP-CORE-0036 §I1)
# Pattern 1 — pure value-type tests; no LOGGER_*, no lifecycle.
# Phase A of the PeerAdmission architecture: pins the interface contract
# (PeerIdentity equality/ordering, PeerAllowlist set + unrestricted
# semantics, PeerAdmission snapshot/replace contract, concurrent
# set+is_allowed safety).  Concrete transports (ZmqQueue ZAP, etc.) are
# exercised at their respective layer-2/-3 test files in Phase C+.
# ============================================================================

add_executable(test_layer2_peer_admission
    test_peer_admission.cpp
)

target_link_libraries(test_layer2_peer_admission
    PRIVATE
        pylabhub::utils
        GTest::gtest
        GTest::gtest_main
        GTest::gmock
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_peer_admission)

pylabhub_register_test_for_staging(
    TARGET test_layer2_peer_admission
    SUITE_NAME "Layer2_PeerAdmission"
)
layer2_require_staged_utils(test_layer2_peer_admission)

gtest_discover_tests(test_layer2_peer_admission
    PROPERTIES
        LABELS "layer2;service;security;peer_admission"
        TIMEOUT 30
)

# ============================================================================
# Test: Key-file ACL utility (HEP-CORE-0035 §4.6)
# Pattern 1 — pure POSIX stat/chmod utility; no LOGGER_*, no lifecycle.
# Owns the verdict matrix for all KeyFileRole + mode combinations.
# Vault / directory tests (test_hub_vault, test_role_vault, test_hub_directory)
# delegate inline mode checks to this utility in #101 sub-phase 1C.
# ============================================================================

add_executable(test_layer2_key_file_acl
    test_key_file_acl.cpp
)

target_link_libraries(test_layer2_key_file_acl
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_key_file_acl)

pylabhub_register_test_for_staging(
    TARGET test_layer2_key_file_acl
    SUITE_NAME "Layer2_KeyFileAcl"
)
layer2_require_staged_utils(test_layer2_key_file_acl)

gtest_discover_tests(test_layer2_key_file_acl
    PROPERTIES
        LABELS "layer2;service;security;key_file_acl"
        TIMEOUT 30
)

# ============================================================================
# Test: Naming validator (HEP-CORE-0033 §G2.2.0b)
# Pure validator test — no workers, no staging dependency.
# ============================================================================

add_executable(test_layer2_naming
    test_naming.cpp
)

target_link_libraries(test_layer2_naming
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_naming)

pylabhub_register_test_for_staging(
    TARGET test_layer2_naming
    SUITE_NAME "Layer2_Naming"
)
layer2_require_staged_utils(test_layer2_naming)

gtest_discover_tests(test_layer2_naming
    PROPERTIES
        LABELS "layer2;service;naming"
        TIMEOUT 30
)

# ============================================================================
# Test: HubState skeleton (HEP-CORE-0033 §G2.1)
# Pure class test — no workers, no staging dependency beyond the shared lib.
# ============================================================================

add_executable(test_layer2_hub_state
    test_hub_state.cpp
)

target_link_libraries(test_layer2_hub_state
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_hub_state)

pylabhub_register_test_for_staging(
    TARGET test_layer2_hub_state
    SUITE_NAME "Layer2_HubState"
)
layer2_require_staged_utils(test_layer2_hub_state)

gtest_discover_tests(test_layer2_hub_state
    PROPERTIES
        LABELS "layer2;service;hubstate"
        TIMEOUT 30
)

# ============================================================================
# Test: HubState query helpers (HEP-CORE-0039 Phase A)
# Pure value-type tests for the Layer 2a helpers in hub_state_queries.hpp.
# No HubState instance needed; helpers operate over const refs to ChannelEntry
# / RoleEntry / HubStateSnapshot, constructed directly by the fixtures.
# Pins HEP-0039 §3.2a contracts: enumerate_live_producers, for_each_*,
# find_role_attachments, is_producer_live, for_each_presence_matching,
# producer_uids/consumer_uids, plus HubStateSnapshot metadata wiring.
# ============================================================================

add_executable(test_layer2_hub_state_queries
    test_hub_state_queries.cpp
)

target_link_libraries(test_layer2_hub_state_queries
    PRIVATE
        pylabhub::utils
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_hub_state_queries)

pylabhub_register_test_for_staging(
    TARGET test_layer2_hub_state_queries
    SUITE_NAME "Layer2_HubStateQueries"
)
layer2_require_staged_utils(test_layer2_hub_state_queries)

gtest_discover_tests(test_layer2_hub_state_queries
    PROPERTIES
        LABELS "layer2;service;hubstate;query_layer"
        TIMEOUT 30
)

# ============================================================================
# Test: RoleHandler — Wave-B M3 skeleton (Presence dedup + index lookup)
# Pattern 1+ (BinaryLifecycleEnvironment) — RoleHandler emits LOGGER_ERROR
# on a duplicate-channel input, so Logger needs the binary-wide guard.
# Network-touching ops (start/shutdown/dispatch) arrive in Wave-B M4
# and land as L3 IsolatedProcessTest scenarios.
# ============================================================================

add_executable(test_layer2_role_handler
    test_role_handler.cpp
)

target_link_libraries(test_layer2_role_handler
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_role_handler)

pylabhub_register_test_for_staging(
    TARGET test_layer2_role_handler
    SUITE_NAME "Layer2_RoleHandler"
)
layer2_require_staged_utils(test_layer2_role_handler)

gtest_discover_tests(test_layer2_role_handler
    PROPERTIES
        LABELS "layer2;service;rolehandler"
        TIMEOUT 30
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
# Test: ThreadManager::join_named (HEP-CORE-0033 §4.2 step 2)
# ============================================================================

add_executable(test_layer2_thread_manager_join_named
    test_thread_manager_join_named.cpp
    workers/thread_manager_join_named_workers.cpp
)

target_include_directories(test_layer2_thread_manager_join_named PRIVATE
    workers
)

target_link_libraries(test_layer2_thread_manager_join_named
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gmock
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_thread_manager_join_named)

pylabhub_register_test_for_staging(
    TARGET test_layer2_thread_manager_join_named
    SUITE_NAME "Layer2_ThreadManagerJoinNamed"
)
layer2_require_staged_utils(test_layer2_thread_manager_join_named)

gtest_discover_tests(test_layer2_thread_manager_join_named
    PROPERTIES
        LABELS "layer2;service;thread_manager"
        TIMEOUT 30
)

# ============================================================================
# Test: ThreadManager active_loop_exited primitive (HEP-CORE-0031 §4.1)
# ============================================================================

add_executable(test_layer2_thread_manager_active_loop
    test_thread_manager_active_loop.cpp
    workers/thread_manager_active_loop_workers.cpp
)

target_include_directories(test_layer2_thread_manager_active_loop PRIVATE
    workers
)

target_link_libraries(test_layer2_thread_manager_active_loop
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gmock
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_thread_manager_active_loop)

pylabhub_register_test_for_staging(
    TARGET test_layer2_thread_manager_active_loop
    SUITE_NAME "Layer2_ThreadManagerActiveLoop"
)
layer2_require_staged_utils(test_layer2_thread_manager_active_loop)

gtest_discover_tests(test_layer2_thread_manager_active_loop
    PROPERTIES
        LABELS "layer2;service;thread_manager"
        TIMEOUT 30
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
# Test: HubDirectory (pure filesystem API — no lifecycle)
# HEP-CORE-0033 §7
# ============================================================================

add_executable(test_layer2_hub_directory
    test_hub_directory.cpp
)

target_link_libraries(test_layer2_hub_directory
    PRIVATE
        pylabhub::utils
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_hub_directory)

pylabhub_register_test_for_staging(
    TARGET test_layer2_hub_directory
    SUITE_NAME "Layer2_HubDirectory"
)
layer2_require_staged_utils(test_layer2_hub_directory)

gtest_discover_tests(test_layer2_hub_directory
    PROPERTIES
        LABELS "layer2;service;hub_directory"
        TIMEOUT 30
)

# ============================================================================
# Test: HubHost (lifecycle owner — Pattern 2, in-process LifecycleGuard)
# HEP-CORE-0033 §4 / Phase 6.1b
# ============================================================================

add_executable(test_layer2_hub_host
    test_hub_host.cpp
    workers/hub_host_workers.cpp
)
target_include_directories(test_layer2_hub_host PRIVATE workers)

target_link_libraries(test_layer2_hub_host
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_hub_host)

pylabhub_register_test_for_staging(
    TARGET test_layer2_hub_host
    SUITE_NAME "Layer2_HubHost"
)
layer2_require_staged_utils(test_layer2_hub_host)

gtest_discover_tests(test_layer2_hub_host
    PROPERTIES
        LABELS "layer2;service;hub_host"
        TIMEOUT 30
)

# ============================================================================
# Test: AdminService skeleton (HEP-CORE-0033 §11 / Phase 6.2a)
# Pattern 2 — in-process LifecycleGuard, REP socket round-trip via the
# shared ZMQ context.  Mirrors test_layer2_hub_host shape.
# ============================================================================

add_executable(test_layer2_admin_service
    test_admin_service.cpp
)

target_link_libraries(test_layer2_admin_service
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        pylabhub::third_party::cppzmq
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_admin_service)

pylabhub_register_test_for_staging(
    TARGET test_layer2_admin_service
    SUITE_NAME "Layer2_AdminService"
)
layer2_require_staged_utils(test_layer2_admin_service)

gtest_discover_tests(test_layer2_admin_service
    PROPERTIES
        LABELS "layer2;service;admin"
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
# Test: hub_cli::parse_hub_args + ParseResult (HEP-CORE-0033 §15 Phase 2)
# ============================================================================

add_executable(test_layer2_hub_cli test_hub_cli.cpp)
target_link_libraries(test_layer2_hub_cli PRIVATE
    pylabhub::utils GTest::gtest GTest::gtest_main)
pylabhub_ensure_sanitizer_for_executable(test_layer2_hub_cli)
pylabhub_register_test_for_staging(
    TARGET test_layer2_hub_cli SUITE_NAME "Layer2_HubCli")
layer2_require_staged_utils(test_layer2_hub_cli)
gtest_discover_tests(test_layer2_hub_cli
    PROPERTIES LABELS "layer2;service;hub_cli" TIMEOUT 30)

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

add_executable(test_layer2_engine_factory
    test_engine_factory.cpp
    workers/engine_factory_workers.cpp)
target_include_directories(test_layer2_engine_factory PRIVATE
    ${CMAKE_SOURCE_DIR}/src/scripting
    workers)
target_link_libraries(test_layer2_engine_factory PRIVATE
    pylabhub::utils
    pylabhub::scripting
    pylabhub::test_framework
    pylabhub::third_party::pybind11_embed
    GTest::gtest)
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
# Test: config→opts translation round-trip (closes TODO #83 / N1)
#
# Production round-trip: init_directory → JSON-on-disk → modify (mimic user
# edit) → load_from_directory → ProducerRoleHost::make_tx_opts (et al) →
# assert every field.  Closes the systemic test gap that produced bugs B5
# (shm_name not copied, 2026-05-20) and B11 (zmq fields not copied,
# 2026-05-21) — both lived in the translation layer that prior L3 tests
# bypassed by hand-constructing the opts struct.
# ============================================================================

add_executable(test_layer2_setup_infrastructure_translation
    test_setup_infrastructure_translation.cpp)
target_include_directories(test_layer2_setup_infrastructure_translation PRIVATE
    ${CMAKE_SOURCE_DIR}/src/producer
    ${CMAKE_SOURCE_DIR}/src/consumer
    ${CMAKE_SOURCE_DIR}/src/processor)
target_link_libraries(test_layer2_setup_infrastructure_translation PRIVATE
    pylabhub::utils pylabhub::test_framework GTest::gtest GTest::gtest_main)
pylabhub_ensure_sanitizer_for_executable(
    test_layer2_setup_infrastructure_translation)
pylabhub_register_test_for_staging(
    TARGET test_layer2_setup_infrastructure_translation
    SUITE_NAME "Layer2_SetupInfrastructureTranslation")
layer2_require_staged_utils(test_layer2_setup_infrastructure_translation)
gtest_discover_tests(test_layer2_setup_infrastructure_translation
    PROPERTIES LABELS "layer2;service;setup_infrastructure_translation"
               TIMEOUT 60)

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
    GTest::gtest)
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
    GTest::gtest)
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
    workers/lua_engine_workers.cpp
)

target_include_directories(test_layer2_lua_engine PRIVATE
    workers
    ${CMAKE_SOURCE_DIR}/src/scripting
    ${CMAKE_SOURCE_DIR}/src/utils   # service/cycle_ops.hpp for dispatcher test
    ${CMAKE_SOURCE_DIR}/tests/test_framework
)

target_link_libraries(test_layer2_lua_engine
    PRIVATE
        pylabhub::utils
        pylabhub::scripting
        pylabhub::test_framework
        GTest::gtest
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
    workers/python_engine_workers.cpp
)

target_include_directories(test_layer2_python_engine PRIVATE
    workers
    ${CMAKE_SOURCE_DIR}/src/scripting
    ${CMAKE_SOURCE_DIR}/tests/test_framework
)

target_link_libraries(test_layer2_python_engine
    PRIVATE
        pylabhub::utils
        pylabhub::scripting
        pylabhub::test_framework
        pylabhub::third_party::pybind11_embed
        GTest::gtest
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
        pylabhub::test_framework
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
# Test: HubAPI script-visible surface (HEP-CORE-0033 §12.3 Phase 7 Commit D1)
# ============================================================================

add_executable(test_layer2_hub_api
    test_hub_api.cpp
)

target_link_libraries(test_layer2_hub_api
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_hub_api)

pylabhub_register_test_for_staging(
    TARGET test_layer2_hub_api
    SUITE_NAME "Layer2_HubAPI"
)
layer2_require_staged_utils(test_layer2_hub_api)

gtest_discover_tests(test_layer2_hub_api
    PROPERTIES
        LABELS "layer2;service;hub_api"
        TIMEOUT 30
)

# ============================================================================
# Test: LogCaptureFixture itself (test framework — Phase 7 Commit D1.5)
# ============================================================================

add_executable(test_layer2_log_capture_fixture
    test_log_capture_fixture.cpp
    workers/log_capture_fixture_workers.cpp
)
target_include_directories(test_layer2_log_capture_fixture PRIVATE workers)

target_link_libraries(test_layer2_log_capture_fixture
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_log_capture_fixture)

pylabhub_register_test_for_staging(
    TARGET test_layer2_log_capture_fixture
    SUITE_NAME "Layer2_LogCaptureFixture"
)
layer2_require_staged_utils(test_layer2_log_capture_fixture)

gtest_discover_tests(test_layer2_log_capture_fixture
    PROPERTIES
        LABELS "layer2;test_framework;log_capture"
        TIMEOUT 30
)

# ============================================================================
# Test: scripting::PeriodicTask + scripting::ZmqPollLoop utility classes
#
# Moved from tests/test_layer3_datahub/ 2026-05-14: subjects are L1/L2
# utility classes, not L3 datahub protocol tests.  Uses the framework's
# `BinaryLifecycleEnvironment` (binary-wide LifecycleGuard via gtest's
# `::testing::Environment`) — requires being the only LifecycleGuard
# owner in its binary, so this is a dedicated executable rather than
# part of an aggregate target.  See test_zmq_poll_loop.cpp file header
# for the per-suite rationale.
# ============================================================================

add_executable(test_layer2_zmq_poll_loop
    test_zmq_poll_loop.cpp
)

target_include_directories(test_layer2_zmq_poll_loop PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(test_layer2_zmq_poll_loop
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_zmq_poll_loop)

pylabhub_register_test_for_staging(
    TARGET test_layer2_zmq_poll_loop
    SUITE_NAME "Layer2_ZmqPollLoop"
)
layer2_require_staged_utils(test_layer2_zmq_poll_loop)

gtest_discover_tests(test_layer2_zmq_poll_loop
    PROPERTIES
        LABELS "layer2;scripting;zmq_poll_loop"
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
# Test: HubConfig — composite hub-side config (HEP-CORE-0033 §6.1)
# ============================================================================

add_executable(test_layer2_hub_config
    test_hub_config.cpp
    workers/hub_config_workers.cpp
)

target_include_directories(test_layer2_hub_config PRIVATE workers)

target_link_libraries(test_layer2_hub_config
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gmock
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_hub_config)

pylabhub_register_test_for_staging(
    TARGET test_layer2_hub_config
    SUITE_NAME "Layer2_HubConfig"
)
layer2_require_staged_utils(test_layer2_hub_config)

gtest_discover_tests(test_layer2_hub_config
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
# Test: EngineModuleParams — validate_fz_info_cache() negative-path coverage
# ============================================================================

add_executable(test_layer2_engine_module_params
    test_engine_module_params.cpp
)

target_link_libraries(test_layer2_engine_module_params
    PRIVATE
        pylabhub::utils
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_engine_module_params)

pylabhub_register_test_for_staging(
    TARGET test_layer2_engine_module_params
    SUITE_NAME "Layer2_EngineModuleParams"
)
layer2_require_staged_utils(test_layer2_engine_module_params)

gtest_discover_tests(test_layer2_engine_module_params
    PROPERTIES
        LABELS "layer2;service;engine_module_params"
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

# ============================================================================
# Test: hub::ZmqQueue (ZMQ PULL/PUSH-backed Queue) — single-class module.
#
# Relocated 2026-05-14 from `tests/test_layer3_datahub/` (where placement
# was historical via the L3 aggregate's STS-LifecycleGuard).  Subject is
# pure single-class — no broker, no protocol — so L2 is correct.  Uses
# Pattern 1+ (BinaryLifecycleEnvironment) with Logger + crypto +
# ZMQContext, in its own dedicated executable.
# ============================================================================

add_executable(test_layer2_hub_zmq_queue
    test_hub_zmq_queue.cpp
)

target_link_libraries(test_layer2_hub_zmq_queue
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_hub_zmq_queue)

pylabhub_register_test_for_staging(
    TARGET test_layer2_hub_zmq_queue
    SUITE_NAME "Layer2_HubZmqQueue"
)
layer2_require_staged_utils(test_layer2_hub_zmq_queue)

gtest_discover_tests(test_layer2_hub_zmq_queue
    PROPERTIES
        LABELS "layer2;service;zmq_queue"
        TIMEOUT 60
)

# ============================================================================
# Test: dispatch_notifications (HEP-CORE-0011 callback table).
#
# Unit tests for the per-cycle table-driven dispatcher in
# `service/cycle_ops.hpp` that routes broker notifications
# (CHANNEL_CLOSING_NOTIFY, CONSUMER_DIED_NOTIFY, ...) to their
# dedicated script callbacks via the `NotificationId` table.
# Pattern 1+ with Logger-only guard — engine surface uses LOGGER_*
# but the test doesn't spin up a HubHost or broker.
# ============================================================================

add_executable(test_layer2_dispatch_notifications
    test_dispatch_notifications.cpp
)

target_include_directories(test_layer2_dispatch_notifications PRIVATE
    ${CMAKE_SOURCE_DIR}/src/utils   # service/cycle_ops.hpp is internal
)

target_link_libraries(test_layer2_dispatch_notifications
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_dispatch_notifications)

pylabhub_register_test_for_staging(
    TARGET test_layer2_dispatch_notifications
    SUITE_NAME "Layer2_DispatchNotifications"
)
layer2_require_staged_utils(test_layer2_dispatch_notifications)

gtest_discover_tests(test_layer2_dispatch_notifications
    PROPERTIES
        LABELS "layer2;service;notifications"
        TIMEOUT 30
)
