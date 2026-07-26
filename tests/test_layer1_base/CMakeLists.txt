# tests/test_layer1_base/CMakeLists.txt
#
# Layer 1 (Base) tests - Pure API tests (no lifecycle required)
#
# These tests verify base utilities built on plh_platform.hpp:
# - Guards (atomic, recursion, scope)
# - Format tools
# - Debug info
# - Module definitions

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

layer1_require_staged_utils(test_layer1_spinlock)

pylabhub_add_gtest(TARGET test_layer1_spinlock
    LABELS "layer1;base;guard;spinlock"
    TIMEOUT 120)

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

layer1_require_staged_utils(test_layer1_recursion_guard)

pylabhub_add_gtest(TARGET test_layer1_recursion_guard
    LABELS "layer1;base;guard;recursion"
    TIMEOUT 30)

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

layer1_require_staged_utils(test_layer1_scope_guard)

pylabhub_add_gtest(TARGET test_layer1_scope_guard
    LABELS "layer1;base;guard;scope"
    TIMEOUT 30)

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

layer1_require_staged_utils(test_layer1_format_tools)

pylabhub_add_gtest(TARGET test_layer1_format_tools
    LABELS "layer1;base;format"
    TIMEOUT 30)

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

layer1_require_staged_utils(test_layer1_result)

pylabhub_add_gtest(TARGET test_layer1_result
    LABELS "layer1;base;result"
    TIMEOUT 30)

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

layer1_require_staged_utils(test_layer1_backoff_strategy)

pylabhub_add_gtest(TARGET test_layer1_backoff_strategy
    LABELS "layer1;base;backoff"
    TIMEOUT 30)

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

layer1_require_staged_utils(test_layer1_portable_atomic_shared_ptr)

pylabhub_add_gtest(TARGET test_layer1_portable_atomic_shared_ptr
    LABELS "layer1;base;atomic;shared_ptr"
    TIMEOUT 30)

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

layer1_require_staged_utils(test_layer1_module_def)

pylabhub_add_gtest(TARGET test_layer1_module_def
    LABELS "layer1;base;module"
    TIMEOUT 30)

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

layer1_require_staged_utils(test_layer1_debug_info)

pylabhub_add_gtest(TARGET test_layer1_debug_info
    LABELS "layer1;base;debug"
    TIMEOUT 30)

# ============================================================================
# Test: scripting::PeriodicTask (utility class, function-level logic)
#
# Moved from tests/test_layer3_datahub/test_datahub_zmq_poll_loop.cpp
# 2026-05-14 — PeriodicTask is a pure-logic utility (no LOGGER_*, no
# lifecycle), correctly L1 by the test-layer purpose framework.
# Sibling ZmqPollLoop tests (L2) live in
# tests/test_layer2_service/test_zmq_poll_loop.cpp.
# ============================================================================

add_executable(test_layer1_periodic_task
    test_periodic_task.cpp
)

target_link_libraries(test_layer1_periodic_task
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer1_periodic_task)

layer1_require_staged_utils(test_layer1_periodic_task)

pylabhub_add_gtest(TARGET test_layer1_periodic_task
    LABELS "layer1;base;periodic_task"
    TIMEOUT 30)

# ============================================================================
# Test: WireEnvelope + typed body classes
# ============================================================================

add_executable(test_layer1_wire_envelope
    test_wire_envelope.cpp
)

target_link_libraries(test_layer1_wire_envelope
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
        libzmq-static
)

pylabhub_ensure_sanitizer_for_executable(test_layer1_wire_envelope)

layer1_require_staged_utils(test_layer1_wire_envelope)

pylabhub_add_gtest(TARGET test_layer1_wire_envelope
    LABELS "layer1;wire;envelope"
    TIMEOUT 30)

# ============================================================================
# Test: Admission gates (reusable pre-state-mutation pipeline)
# ============================================================================

add_executable(test_layer1_admission_gates
    test_admission_gates.cpp
)

target_link_libraries(test_layer1_admission_gates
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
        libzmq-static
)

pylabhub_ensure_sanitizer_for_executable(test_layer1_admission_gates)

layer1_require_staged_utils(test_layer1_admission_gates)

pylabhub_add_gtest(TARGET test_layer1_admission_gates
    LABELS "layer1;admission;gates"
    TIMEOUT 30)

# ============================================================================
# Test: ReplayGuard (header-only I-REPLAY-BOUND dedup primitive) — the ONE
# sliding-window nonce store shared by the inbox, REG, and admin planes.
# Pins the trusted-clock + window>=2*skew contract behind task #67.
# ============================================================================

add_executable(test_layer1_replay_guard
    test_replay_guard.cpp
)

target_link_libraries(test_layer1_replay_guard
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer1_replay_guard)

layer1_require_staged_utils(test_layer1_replay_guard)

pylabhub_add_gtest(TARGET test_layer1_replay_guard
    LABELS "layer1;replay;security"
    TIMEOUT 30)

# ============================================================================
# Test: ConsoleOutputBuffer (header-only pull output buffer for the operator
# console — HEP-CORE-0033 §11.0.1 layer 6 / §11.0.4).  Pins append/drain
# return-and-clear, request_id tagging, the JSON-object content contract, and
# the cap-as-safety-valve overflow→log + dropped_count behaviour.
# ============================================================================

add_executable(test_layer1_console_output_buffer
    test_console_output_buffer.cpp
)

target_link_libraries(test_layer1_console_output_buffer
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer1_console_output_buffer)

layer1_require_staged_utils(test_layer1_console_output_buffer)

pylabhub_add_gtest(TARGET test_layer1_console_output_buffer
    LABELS "layer1;admin;console"
    TIMEOUT 30)

# ============================================================================
# Test: parse_hub_admin_config — the admin.output_buffer console-buffer caps
# (HEP-CORE-0033 §11.0.4).  Pure-function parse + strict validation.
# ============================================================================

add_executable(test_layer1_hub_admin_config
    test_hub_admin_config.cpp
)

target_link_libraries(test_layer1_hub_admin_config
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer1_hub_admin_config)

layer1_require_staged_utils(test_layer1_hub_admin_config)

pylabhub_add_gtest(TARGET test_layer1_hub_admin_config
    LABELS "layer1;admin;config"
    TIMEOUT 30)

# ============================================================================
# Test: wire_dispatch::kDispatchTable regression pin (Group 5 gap 3)
# ============================================================================

add_executable(test_layer1_wire_dispatch_table
    test_wire_dispatch_table.cpp
)

target_link_libraries(test_layer1_wire_dispatch_table
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
        libzmq-static
)

pylabhub_ensure_sanitizer_for_executable(test_layer1_wire_dispatch_table)

layer1_require_staged_utils(test_layer1_wire_dispatch_table)

pylabhub_add_gtest(TARGET test_layer1_wire_dispatch_table
    LABELS "layer1;wire;dispatch"
    TIMEOUT 15)

# ============================================================================
# Test: VersionedAdmissionLedger (header-only template) — the primitive
# that fan-in/fan-out/one-to-one topologies all reduce to (HEP-CORE-0042
# §5.5.2 INVARIANT-BIND-CONFIRM-1..3).
# ============================================================================

add_executable(test_layer1_versioned_admission_ledger
    test_versioned_admission_ledger.cpp
)

target_link_libraries(test_layer1_versioned_admission_ledger
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer1_versioned_admission_ledger)

layer1_require_staged_utils(test_layer1_versioned_admission_ledger)

pylabhub_add_gtest(TARGET test_layer1_versioned_admission_ledger
    LABELS "layer1;admission;ledger;hep0042"
    TIMEOUT 30)

message(STATUS "Layer 1 (Base) tests configured successfully")
message(STATUS "  - test_layer1_spinlock")
message(STATUS "  - test_layer1_recursion_guard")
message(STATUS "  - test_layer1_scope_guard")
message(STATUS "  - test_layer1_format_tools")
message(STATUS "  - test_layer1_result")
message(STATUS "  - test_layer1_backoff_strategy")
message(STATUS "  - test_layer1_module_def")
message(STATUS "  - test_layer1_debug_info")
message(STATUS "  - test_layer1_periodic_task")
message(STATUS "  - test_layer1_wire_envelope")
message(STATUS "  - test_layer1_versioned_admission_ledger")
