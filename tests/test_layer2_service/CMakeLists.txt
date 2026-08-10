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

layer2_require_staged_utils(test_layer2_framework_selftest)

pylabhub_add_gtest(TARGET test_layer2_framework_selftest
    LABELS "layer2;framework;selftest;critical"
    TIMEOUT 60)

# ── StartupConfig parser (task #327) ────────────────────────────────────────

add_executable(test_layer2_startup_config
    test_startup_config.cpp
)

target_link_libraries(test_layer2_startup_config
    PRIVATE
        pylabhub::utils
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_startup_config)

layer2_require_staged_utils(test_layer2_startup_config)

pylabhub_add_gtest(TARGET test_layer2_startup_config
    LABELS "layer2;config;startup"
    TIMEOUT 30)

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

layer2_require_staged_utils(test_layer2_net_address)

pylabhub_add_gtest(TARGET test_layer2_net_address
    LABELS "layer2;service;net"
    TIMEOUT 30)

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

layer2_require_staged_utils(test_layer2_zap_router)

pylabhub_add_gtest(TARGET test_layer2_zap_router
    LABELS "layer2;service;security;zap_router"
    TIMEOUT 60)

# ============================================================================
# Test: KeyStore + LockedKey + SecureBuffer (HEP-CORE-0040 §170)
# Pattern 3 — subprocess per scenario.  SecureSubsystem and KeyStore
# are process singletons (HEP-0040 §4.1 + §5.1); serial isolation needed.
# Pins:
#   - ordering: KeyStore ctor throws if SecureSubsystem not ready
#   - singleton: second KeyStore ctor throws
#   - use-not-export: pubkey/with_seckey/with_raw_key error paths
#   - thread safety: parallel reads, remove blocks behind with_seckey
#   - LockedKey + SecureBuffer dtor zeroing semantics
# ============================================================================

add_executable(test_layer2_key_store
    test_key_store.cpp
    workers/key_store_workers.cpp
)

target_link_libraries(test_layer2_key_store
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gmock
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_key_store)

layer2_require_staged_utils(test_layer2_key_store)

pylabhub_add_gtest(TARGET test_layer2_key_store
    LABELS "layer2;service;security;key_store"
    TIMEOUT 60)

# ============================================================================
# Test: seed_curve_identities protocol-validation (HEP-CORE-0040 §172 +
# HEP-CORE-0035 §4.6.5; task #177).  Pattern 3 — subprocess per scenario.
# Pins the test fixture's seed contract:
#   - canonical names (kHubIdentityName, role_keystore_name)
#   - seed byte-equivalence (no pubkey↔seckey swap)
#   - minimality (no extra seeded entries)
# Prevents fixture drift from silently breaking the L3 broker suite.
# ============================================================================
add_executable(test_layer2_curve_test_fixtures
    test_curve_test_fixtures.cpp
    workers/curve_test_fixtures_workers.cpp
)

target_link_libraries(test_layer2_curve_test_fixtures
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gmock
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_curve_test_fixtures)

layer2_require_staged_utils(test_layer2_curve_test_fixtures)

pylabhub_add_gtest(TARGET test_layer2_curve_test_fixtures
    LABELS "layer2;service;security;test_fixture;curve_test_setup"
    TIMEOUT 60)

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

layer2_require_staged_utils(test_layer2_zmq_queue_auth)

pylabhub_add_gtest(TARGET test_layer2_zmq_queue_auth
    LABELS "layer2;service;security;zmq_queue;peer_admission"
    TIMEOUT 60)

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

layer2_require_staged_utils(test_layer2_known_roles)

pylabhub_add_gtest(TARGET test_layer2_known_roles
    LABELS "layer2;service;security;known_roles"
    TIMEOUT 30)

# ============================================================================
# Test: pubkey origin index (HEP-CORE-0035 §4.2)
#   - a key resolves to its subject AND its kind (role vs federation peer;
#     the kind decides who may carry identities other than its own)
#   - unknown / near-miss / empty key resolves to nothing, never a default
#   - one key configured for two subjects is REFUSED (role↔role and the
#     cross-category role↔peer clash the known-roles store cannot see)
#   - a refused insert leaves the index unchanged
#   - allowlist projection covers both kinds and is never unrestricted
#   - inbox roster is local roles ONLY, sorted for stable wire captures
# ============================================================================

add_executable(test_layer2_pubkey_origin
    test_pubkey_origin.cpp
)

target_link_libraries(test_layer2_pubkey_origin
    PRIVATE
        pylabhub::utils
        GTest::gtest
        GTest::gtest_main
        GTest::gmock
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_pubkey_origin)

layer2_require_staged_utils(test_layer2_pubkey_origin)

pylabhub_add_gtest(TARGET test_layer2_pubkey_origin
    LABELS "layer2;service;security;known_roles"
    TIMEOUT 30)

# ============================================================================
# Test: build-flag invariants for test/debug backdoor gates (#205, 2026-06-11)
# Pins HEP-CORE-0032 §3.2 deployment posture — the test_set_* mutators on
# RoleHostCore (and the PYLABHUB_UTILS_TEST_EXPORT macro) must be present
# only in Debug+BUILD_TESTS builds, absent in any Release configuration.
# ============================================================================

add_executable(test_layer2_build_flag_invariants
    test_build_flag_invariants.cpp
)

target_link_libraries(test_layer2_build_flag_invariants
    PRIVATE
        pylabhub::utils
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_build_flag_invariants)

layer2_require_staged_utils(test_layer2_build_flag_invariants)

pylabhub_add_gtest(TARGET test_layer2_build_flag_invariants
    LABELS "layer2;service;security;build_flags"
    TIMEOUT 10)

# ============================================================================
# Test: Z85PublicKey strong type (HEP-CORE-0040 §8.4 + AUTH_TODO §C2, #158)
# Pattern 1 — pure value-type tests; no LOGGER_*, no lifecycle.
# Pins:
#   - 40-char Z85 length contract
#   - Z85 alphabet (RFC 32 §4) — 23 punctuation chars + alphanumerics
#   - Default sentinel value (40 zero bytes) distinguishable from
#     validated input (which cannot contain '\0')
#   - Diagnostics name the offending position/length so misuse is
#     fixable from the error message alone
# ============================================================================

add_executable(test_layer2_curve_keypair
    test_curve_keypair.cpp
)

target_link_libraries(test_layer2_curve_keypair
    PRIVATE
        pylabhub::utils
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_curve_keypair)

layer2_require_staged_utils(test_layer2_curve_keypair)

# ============================================================================
# role_reg_payload — pure-value invariants for REG_REQ payload builder
# (origin: 2026-06-30 workflow code-review finding [4] CONFIRMED, task #303)
# ============================================================================

add_executable(test_layer2_role_reg_payload
    test_role_reg_payload.cpp
)

target_link_libraries(test_layer2_role_reg_payload
    PRIVATE
        pylabhub::utils
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_role_reg_payload)

layer2_require_staged_utils(test_layer2_role_reg_payload)

pylabhub_add_gtest(TARGET test_layer2_role_reg_payload
    LABELS "layer2;service;role_reg_payload"
    TIMEOUT 10)

pylabhub_add_gtest(TARGET test_layer2_curve_keypair
    LABELS "layer2;service;security;curve_keypair"
    TIMEOUT 10)

# role_identity_policy enum↔string helpers test deleted 2026-07-20 with the
# RoleIdentityPolicy placeholder (HEP-CORE-0035 §4.5 / §8 Phase 6 — the
# self-asserted string gate was redundant with the ZAP pubkey allowlist).

# ============================================================================
# Test: ShmCapabilityChannel skeleton (HEP-CORE-0041 §6)
# Pattern 1 — pure interface pins; no LifecycleGuard, no LOGGER_*.
# Pins:
#   - Abstract base types (compile-time static_assert)
#   - Non-copyable surface (compile-time static_assert)
#   - Factory stubs throw with documented HEP + task references
#   - AcceptedPeer POSIX-credential field shape
# Retires when task #249 lands a real backend and the throws disappear.
# ============================================================================

add_executable(test_layer2_shm_capability_channel
    test_shm_capability_channel.cpp
)

target_link_libraries(test_layer2_shm_capability_channel
    PRIVATE
        pylabhub::utils
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_shm_capability_channel)

layer2_require_staged_utils(test_layer2_shm_capability_channel)

pylabhub_add_gtest(TARGET test_layer2_shm_capability_channel
    LABELS "layer2;service;security;shm_capability"
    TIMEOUT 10)

# ============================================================================
# Test: AttachProtocol (HEP-CORE-0041 §9 D4 challenge-response, substep 1c)
# Pattern 1 — direct libsodium + bare Unix sockets; no LOGGER_*, no lifecycle.
# Pins:
#   - Happy round-trip hello + challenge-response.
#   - Cryptographic proof actually rejects wrong-seckey impersonator.
#   - MAC tampering rejected.
#   - Hello shape validation (protocol_version, role_uid, size cap, JSON).
#   - Timeout / connect-failure return shapes.
# ============================================================================

add_executable(test_layer2_attach_protocol
    test_attach_protocol.cpp
)

target_link_libraries(test_layer2_attach_protocol
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework   # PLH_BINARY_LIFECYCLE_MODULES for SMS setup
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_attach_protocol)

layer2_require_staged_utils(test_layer2_attach_protocol)

pylabhub_add_gtest(TARGET test_layer2_attach_protocol
    LABELS "layer2;service;security;attach_protocol"
    TIMEOUT 30)

# ============================================================================
# Test: ShmAttachOrchestrator (HEP-CORE-0041 §9 D4 steps 6-7, substep 1e)
# Pattern 1+ — binary-wide LifecycleGuard for Logger (orchestrator emits
# LOGGER_INFO/WARN for the divergence-WARN table).  No cross-test state.
# Pins (one test per HEP-0041 §9 D4 cached-allowlist table cell + edge cases):
#   - both allowed → silent + Sent
#   - both denied → silent + DeniedByBroker
#   - broker allowed + cache denied → WARN divergence + Sent
#   - broker denied + cache allowed → WARN divergence + DeniedByBroker
#   - broker_query nullopt → WARN + DeniedTransportFail
#   - broker unexpected status → WARN + DeniedTransportFail
#   - L2 handshake failure → WARN + HandshakeFailed (broker NOT queried)
#   - no consumer connects → Timeout (no logs)
# ============================================================================

add_executable(test_layer2_shm_attach_orchestrator
    test_shm_attach_orchestrator.cpp
)

target_link_libraries(test_layer2_shm_attach_orchestrator
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_shm_attach_orchestrator)

layer2_require_staged_utils(test_layer2_shm_attach_orchestrator)

pylabhub_add_gtest(TARGET test_layer2_shm_attach_orchestrator
    LABELS "layer2;service;security;shm_attach_orchestrator"
    TIMEOUT 30)
# ============================================================================
# Test: DataBlock fd-source ctors + factories (HEP-CORE-0041 substep 1f, #253)
# Pattern 1+ — binary-wide LifecycleGuard for Logger + SecureSubsystem +
# DataBlock (the fd-source factories require GetDataBlockModule() per
# the same contract as the name-based factories).
# Pins:
#   - in-process round-trip via memfd_create + socketpair/SCM_RIGHTS
#     (DataBlock-from-fd narrow contract)
#   - end-to-end via ShmCapabilityProducer + recv_capability → fd
#     handoff → DataBlock-from-fd on both sides (1b↔1f integration)
#   - producer ctor rejects fd whose fstat size doesn't match layout
#     total_size (caller-must-ftruncate contract)
# ============================================================================

add_executable(test_layer2_data_block_fd_based
    test_data_block_fd_based.cpp
)

target_link_libraries(test_layer2_data_block_fd_based
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_data_block_fd_based)

layer2_require_staged_utils(test_layer2_data_block_fd_based)

pylabhub_add_gtest(TARGET test_layer2_data_block_fd_based
    LABELS "layer2;service;shm;data_block;hep0041"
    TIMEOUT 30)

# ============================================================================
# Test: ShmQueue capability-fd path (HEP-CORE-0041 substep 1i-mig-1)
# Pattern 1+ — binary-wide LifecycleGuard (Logger + SecureSubsystem + DataBlock,
# same as test_data_block_fd_based) — Standby-mode create_*_standby
# factories + set_shm_capability_fd Standby → Configured + start() fd-source
# branch + mutual-exclusion guards with the legacy secret-mode path.
# ============================================================================

add_executable(test_layer2_hub_shm_queue_capability
    test_hub_shm_queue_capability.cpp
)

# ============================================================================
# test_layer2_hub_queue_factory — HEP-CORE-0017 §3.3.0 unified factory
# ----------------------------------------------------------------------------
# Phase C step 4: pins hub::Queue::create_reader / create_writer dispatch
# for all (topology, transport) cells, the (FanIn, Shm) legality gate, the
# transport_from_string parser, and the reader_is_binding_side /
# writer_is_binding_side matrix helpers.
# ============================================================================

add_executable(test_layer2_hub_queue_factory
    test_hub_queue_factory.cpp
)

target_link_libraries(test_layer2_hub_queue_factory
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_hub_queue_factory)

layer2_require_staged_utils(test_layer2_hub_queue_factory)

pylabhub_add_gtest(TARGET test_layer2_hub_queue_factory
    LABELS "layer2;service;hep0017"
    TIMEOUT 30)

target_link_libraries(test_layer2_hub_shm_queue_capability
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_hub_shm_queue_capability)

layer2_require_staged_utils(test_layer2_hub_shm_queue_capability)

pylabhub_add_gtest(TARGET test_layer2_hub_shm_queue_capability
    LABELS "layer2;service;shm;hep0041"
    TIMEOUT 30)

# ============================================================================
# Test: ShmQueue contract L2 revival post-S3c retirement (task #323)
# Pattern 1+ — same as _capability sibling above.
# Covers 4 low-level ShmQueue / DataBlockProducer contracts that the S3c
# retirement dropped without a real L2/L4 replacement:
#   1. NoFlexzone                — flexzone()/flexzone_size()=={nullptr,0}.
#   2. FlexzoneBidirectional     — one shared region, both endpoints see writes.
#   3. VerifyChecksumMismatch    — read_acquire refuses slot_valid=0.
#   4. DataBlockProducerRemapStubsThrow — request_/commit_structure_remap throw.
# Uses the header-only make_fd_backed_producer helper from
# test_layer3_datahub/workers/datahub_fd_test_helper.h — same fd-source
# factory the sibling capability test's inline path uses.
# ============================================================================

add_executable(test_layer2_hub_shm_queue_contract
    test_hub_shm_queue_contract.cpp
)

target_link_libraries(test_layer2_hub_shm_queue_contract
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_hub_shm_queue_contract)

layer2_require_staged_utils(test_layer2_hub_shm_queue_contract)

pylabhub_add_gtest(TARGET test_layer2_hub_shm_queue_contract
    LABELS "layer2;service;shm;hep0041"
    TIMEOUT 30)

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

layer2_require_staged_utils(test_layer2_peer_admission)

pylabhub_add_gtest(TARGET test_layer2_peer_admission
    LABELS "layer2;service;security;peer_admission"
    TIMEOUT 30)

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

layer2_require_staged_utils(test_layer2_key_file_acl)

pylabhub_add_gtest(TARGET test_layer2_key_file_acl
    LABELS "layer2;service;security;key_file_acl"
    TIMEOUT 30)

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

layer2_require_staged_utils(test_layer2_naming)

pylabhub_add_gtest(TARGET test_layer2_naming
    LABELS "layer2;service;naming"
    TIMEOUT 30)

# ============================================================================
# Test: HubState skeleton (HEP-CORE-0033 §G2.1)
# Pure class test — no workers, no staging dependency beyond the shared lib.
# ============================================================================

add_executable(test_layer2_hub_state
    test_hub_state.cpp
    workers/broker_service_workers.cpp
)

target_link_libraries(test_layer2_hub_state
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_hub_state)

layer2_require_staged_utils(test_layer2_hub_state)

pylabhub_add_gtest(TARGET test_layer2_hub_state
    LABELS "layer2;service;hubstate"
    TIMEOUT 30)

# ============================================================================
# Test: HubState anti-replay nonce dedup (I-REPLAY-BOUND)
# ============================================================================

add_executable(test_layer2_hub_state_nonce_dedup
    test_hub_state_nonce_dedup.cpp
)

target_link_libraries(test_layer2_hub_state_nonce_dedup
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_hub_state_nonce_dedup)

layer2_require_staged_utils(test_layer2_hub_state_nonce_dedup)

pylabhub_add_gtest(TARGET test_layer2_hub_state_nonce_dedup
    LABELS "layer2;service;hubstate;replay"
    TIMEOUT 30)

# ============================================================================
# Test: Wire adapter round-trip (HEP-CORE-0046 Phase B Tier 1) — proves
# semantic equivalence between current-shape JSON payloads and encoded+
# decoded wire multiparts, so Tier 2 can swap the wire path with
# confidence that legacy handlers see the same body they see today.
# ============================================================================

add_executable(test_layer2_wire_adapter_roundtrip
    test_wire_adapter_roundtrip.cpp
)

target_link_libraries(test_layer2_wire_adapter_roundtrip
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_wire_adapter_roundtrip)

layer2_require_staged_utils(test_layer2_wire_adapter_roundtrip)

pylabhub_add_gtest(TARGET test_layer2_wire_adapter_roundtrip
    LABELS "layer2;service;wire;adapter;hep0046"
    TIMEOUT 30)

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

layer2_require_staged_utils(test_layer2_hub_state_queries)

pylabhub_add_gtest(TARGET test_layer2_hub_state_queries
    LABELS "layer2;service;hubstate;query_layer"
    TIMEOUT 30)

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

layer2_require_staged_utils(test_layer2_role_handler)

pylabhub_add_gtest(TARGET test_layer2_role_handler
    LABELS "layer2;service;rolehandler"
    TIMEOUT 30)

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

layer2_require_staged_utils(test_layer2_shared_memory_spinlock)

pylabhub_add_gtest(TARGET test_layer2_shared_memory_spinlock
    LABELS "layer2;service;spinlock"
    TIMEOUT 30)

# ============================================================================
# CRITICAL Test: SMS Category 1 primitives (blocks schema validation)
# ============================================================================
#
# Post-2026-07-06 CryptoUtils merger (HEP-CORE-0043 §2.1): this binary
# tests the SecureSubsystem Category 1 surface (BLAKE2b hash, random,
# lifecycle).  File names retain the historical `crypto_*` prefix
# because the worker file name feeds subprocess dispatch strings; the
# fixture class was renamed `CryptoUtilsTest` → `SMS_PrimitivesTest`.

add_executable(test_layer2_sms_primitives
    test_crypto_utils.cpp
    workers/crypto_workers.cpp
)

target_include_directories(test_layer2_sms_primitives PRIVATE workers)

target_link_libraries(test_layer2_sms_primitives
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_sms_primitives)

layer2_require_staged_utils(test_layer2_sms_primitives)

pylabhub_add_gtest(TARGET test_layer2_sms_primitives
    LABELS "layer2;service;crypto;critical"
    TIMEOUT 60)

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

layer2_require_staged_utils(test_layer2_thread_manager_join_named)

pylabhub_add_gtest(TARGET test_layer2_thread_manager_join_named
    LABELS "layer2;service;thread_manager"
    TIMEOUT 30)

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

layer2_require_staged_utils(test_layer2_thread_manager_active_loop)

pylabhub_add_gtest(TARGET test_layer2_thread_manager_active_loop
    LABELS "layer2;service;thread_manager"
    TIMEOUT 30)

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

layer2_require_staged_utils(test_layer2_lifecycle)

pylabhub_add_gtest(TARGET test_layer2_lifecycle
    LABELS "layer2;service;lifecycle"
    TIMEOUT 60)

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

layer2_require_staged_utils(test_layer2_lifecycle_dynamic)

pylabhub_add_gtest(TARGET test_layer2_lifecycle_dynamic
    LABELS "layer2;service;lifecycle;dynamic"
    TIMEOUT 60)

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

layer2_require_staged_utils(test_layer2_filelock)

pylabhub_add_gtest(TARGET test_layer2_filelock
    LABELS "layer2;service;filelock;multiprocess"
    TIMEOUT 120)

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

layer2_require_staged_utils(test_layer2_filelock_singleprocess)

pylabhub_add_gtest(TARGET test_layer2_filelock_singleprocess
    LABELS "layer2;service;filelock;singleprocess"
    TIMEOUT 60)

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

layer2_require_staged_utils(test_layer2_logger)

pylabhub_add_gtest(TARGET test_layer2_logger
    LABELS "layer2;service;logger"
    TIMEOUT 60)

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

layer2_require_staged_utils(test_layer2_jsonconfig)

pylabhub_add_gtest(TARGET test_layer2_jsonconfig
    LABELS "layer2;service;jsonconfig"
    TIMEOUT 60)

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
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_hub_vault)

layer2_require_staged_utils(test_layer2_hub_vault)

pylabhub_add_gtest(TARGET test_layer2_hub_vault
    LABELS "layer2;service;vault;security"
    TIMEOUT 120)

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
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_role_vault)

layer2_require_staged_utils(test_layer2_role_vault)

pylabhub_add_gtest(TARGET test_layer2_role_vault
    LABELS "layer2;service;vault;security"
    TIMEOUT 120)

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

layer2_require_staged_utils(test_layer2_zmq_context)

pylabhub_add_gtest(TARGET test_layer2_zmq_context
    LABELS "layer2;service;zmq"
    TIMEOUT 30)

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

layer2_require_staged_utils(test_layer2_uid_utils)

pylabhub_add_gtest(TARGET test_layer2_uid_utils
    LABELS "layer2;service;uid"
    TIMEOUT 30)

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

layer2_require_staged_utils(test_layer2_interactive_signal_handler)

pylabhub_add_gtest(TARGET test_layer2_interactive_signal_handler
    LABELS "layer2;service;signal"
    TIMEOUT 30)

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

layer2_require_staged_utils(test_layer2_slot_view_helpers)

pylabhub_add_gtest(TARGET test_layer2_slot_view_helpers
    LABELS "layer2;service;scripting;slotview"
    TIMEOUT 60)

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

layer2_require_staged_utils(test_layer2_role_directory)

pylabhub_add_gtest(TARGET test_layer2_role_directory
    LABELS "layer2;service;role_directory"
    TIMEOUT 30)

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

layer2_require_staged_utils(test_layer2_hub_directory)

pylabhub_add_gtest(TARGET test_layer2_hub_directory
    LABELS "layer2;service;hub_directory"
    TIMEOUT 30)

# ============================================================================
# Test: HubHost (lifecycle owner — Pattern 2, in-process LifecycleGuard)
# HEP-CORE-0033 §4 / Phase 6.1b
# ============================================================================
# RESTORED 2026-07-21 (#71): was MASKED because HubHost::startup() throws on an
# empty pubkey (HEP-CORE-0035 §2) and the workers constructed HubHost without a
# hub keypair.  The workers now seed `kHubIdentityName` in the process KeyStore
# via the framework's canonical `add_curve_identity`/`gen_curve_keypair`
# (curve_test_setup.h) inside `make_config` — same path AdminService and
# broker_test_harness use — so the lifecycle FSM suite (startup/shutdown
# idempotency, busy-port, destructor cleanup, …) is under active coverage again.

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

layer2_require_staged_utils(test_layer2_hub_host)

pylabhub_add_gtest(TARGET test_layer2_hub_host
    LABELS "layer2;service;hub_host"
    TIMEOUT 30)

# ============================================================================
# Test: AdminService (HEP-CORE-0033 §11 — CURVE-secured admin plane)
# Pattern 1+ — in-process LifecycleGuard, CURVE REP round-trip via the shared
# ZMQ context.  Mirrors test_layer2_hub_host shape.
# ============================================================================
# RESTORED 2026-07-19 (was MASKED for HEP-0035 §2 empty-pubkey throw): the
# test now seeds the `hub_identity` KeyStore entry via
# `curve_test_setup.h::add_curve_identity` in SetUp (HEP-0035 §4.6.5 no-bypass
# discipline), so HubHost/AdminService find the hub identity the admin
# curve_server needs.

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

layer2_require_staged_utils(test_layer2_admin_service)

pylabhub_add_gtest(TARGET test_layer2_admin_service
    LABELS "layer2;service;admin"
    TIMEOUT 30)

# ---- Admin console session identity (HEP-CORE-0033 §11.0.5) ----------------
add_executable(test_layer2_admin_session
    test_admin_session.cpp
)

target_link_libraries(test_layer2_admin_session
    PRIVATE
        pylabhub::utils
        pylabhub::test_framework
        GTest::gtest
        GTest::gtest_main
)

pylabhub_ensure_sanitizer_for_executable(test_layer2_admin_session)

layer2_require_staged_utils(test_layer2_admin_session)

pylabhub_add_gtest(TARGET test_layer2_admin_session
    LABELS "layer2;service;admin"
    TIMEOUT 30)

# ============================================================================
# Test: role_cli::parse_role_args + ParseResult (in-process, no std::exit)
# ============================================================================

add_executable(test_layer2_role_cli test_role_cli.cpp)
target_link_libraries(test_layer2_role_cli PRIVATE
    pylabhub::utils GTest::gtest GTest::gtest_main)
pylabhub_ensure_sanitizer_for_executable(test_layer2_role_cli)
layer2_require_staged_utils(test_layer2_role_cli)
pylabhub_add_gtest(TARGET test_layer2_role_cli
    LABELS "layer2;service;role_cli"
    TIMEOUT 30)

# ============================================================================
# Test: hub_cli::parse_hub_args + ParseResult (HEP-CORE-0033 §15 Phase 2)
# ============================================================================

add_executable(test_layer2_hub_cli test_hub_cli.cpp)
target_link_libraries(test_layer2_hub_cli PRIVATE
    pylabhub::utils GTest::gtest GTest::gtest_main)
pylabhub_ensure_sanitizer_for_executable(test_layer2_hub_cli)
layer2_require_staged_utils(test_layer2_hub_cli)
pylabhub_add_gtest(TARGET test_layer2_hub_cli
    LABELS "layer2;service;hub_cli"
    TIMEOUT 30)

# ============================================================================
# Test: RoleRegistry + RuntimeBuilder
# ============================================================================

add_executable(test_layer2_role_registry test_role_registry.cpp)
target_link_libraries(test_layer2_role_registry PRIVATE
    pylabhub::utils GTest::gtest GTest::gtest_main)
pylabhub_ensure_sanitizer_for_executable(test_layer2_role_registry)
layer2_require_staged_utils(test_layer2_role_registry)
pylabhub_add_gtest(TARGET test_layer2_role_registry
    LABELS "layer2;service;role_registry"
    TIMEOUT 30)

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
layer2_require_staged_utils(test_layer2_engine_factory)
pylabhub_add_gtest(TARGET test_layer2_engine_factory
    LABELS "layer2;service;engine_factory"
    TIMEOUT 30)

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
layer2_require_staged_utils(test_layer2_role_init_directory)
pylabhub_add_gtest(TARGET test_layer2_role_init_directory
    LABELS "layer2;service;role_init_directory"
    TIMEOUT 60)

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
layer2_require_staged_utils(test_layer2_setup_infrastructure_translation)
pylabhub_add_gtest(TARGET test_layer2_setup_infrastructure_translation
    LABELS "layer2;service;setup_infrastructure_translation"
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
layer2_require_staged_utils(test_layer2_role_logging_roundtrip)
pylabhub_add_gtest(TARGET test_layer2_role_logging_roundtrip
    LABELS "layer2;service;role_logging_roundtrip"
    TIMEOUT 60)

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
layer2_require_staged_utils(test_layer2_configure_logger)
pylabhub_add_gtest(TARGET test_layer2_configure_logger
    LABELS "layer2;service;configure_logger"
    TIMEOUT 60)

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
layer2_require_staged_utils(test_layer2_role_host_base)
pylabhub_add_gtest(TARGET test_layer2_role_host_base
    LABELS "layer2;service;role_host_base"
    TIMEOUT 60)

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

layer2_require_staged_utils(test_layer2_loop_timing_policy)

pylabhub_add_gtest(TARGET test_layer2_loop_timing_policy
    LABELS "layer2;service;loop_timing"
    TIMEOUT 30)

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

layer2_require_staged_utils(test_layer2_lua_engine)

pylabhub_add_gtest(TARGET test_layer2_lua_engine
    LABELS "layer2;service;scripting;lua;engine"
    TIMEOUT 30)

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

layer2_require_staged_utils(test_layer2_python_engine)

pylabhub_add_gtest(TARGET test_layer2_python_engine
    LABELS "layer2;service;scripting;python;engine"
    TIMEOUT 60)

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

# L4 live-broker smoke plugin: loaded by the real plh_role binary in
# test_layer4_plh_hub (ZmqE2E_NativeProducer_LiveSchemaQueries).  Built
# here because all native test plugins live under test_plugins/; staged
# to the same tests/ dir so the L4 test resolves it next to its own
# binary.
add_library(test_l4_native_producer_plugin SHARED
    test_plugins/l4_native_producer_plugin.cpp
)
target_include_directories(test_l4_native_producer_plugin PRIVATE
    ${CMAKE_SOURCE_DIR}/src/include
)
target_link_libraries(test_l4_native_producer_plugin PRIVATE pylabhub::utils)
target_compile_features(test_l4_native_producer_plugin PRIVATE cxx_std_20)
set_target_properties(test_l4_native_producer_plugin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${PYLABHUB_STAGING_DIR}/tests"
)

# Compile-only TU pinning the v6 C++20 concept contracts (#194 Phase C).
# Built as an OBJECT library so we never link it — the build IS the test:
# all `static_assert`s inside fire at parse time.  If any future change
# weakens AllowedPeerVisitor / BandMemberVisitor (drops the noexcept
# requirement, accepts the wrong arg shape), this target fails to
# compile and the regression is caught at build time.
add_library(test_v6_concepts_compile OBJECT
    test_plugins/test_v6_concepts_compile.cpp
)
target_include_directories(test_v6_concepts_compile PRIVATE
    ${CMAKE_SOURCE_DIR}/src/include
)
target_compile_features(test_v6_concepts_compile PRIVATE cxx_std_20)

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
layer2_require_staged_utils(test_layer2_scriptengine_native_dylib)
pylabhub_add_gtest(TARGET test_layer2_scriptengine_native_dylib
    LABELS "layer2;service;scripting;native;engine"
    TIMEOUT 60)

message(STATUS "Layer 2 (Service) tests configured successfully")
message(STATUS "  - test_layer2_framework_selftest (CRITICAL - framework self-verification)")
message(STATUS "  - test_layer2_sms_primitives      (multi-process)")
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

layer2_require_staged_utils(test_layer2_slot_rw_coordinator)

pylabhub_add_gtest(TARGET test_layer2_slot_rw_coordinator
    LABELS "layer2;service;slotrw"
    TIMEOUT 60)

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

layer2_require_staged_utils(test_layer2_role_host_core)

pylabhub_add_gtest(TARGET test_layer2_role_host_core
    LABELS "layer2;service;scripting"
    TIMEOUT 30)

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

layer2_require_staged_utils(test_layer2_hub_api)

pylabhub_add_gtest(TARGET test_layer2_hub_api
    LABELS "layer2;service;hub_api"
    TIMEOUT 30)

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

layer2_require_staged_utils(test_layer2_log_capture_fixture)

pylabhub_add_gtest(TARGET test_layer2_log_capture_fixture
    LABELS "layer2;test_framework;log_capture"
    TIMEOUT 30)

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

layer2_require_staged_utils(test_layer2_zmq_poll_loop)

pylabhub_add_gtest(TARGET test_layer2_zmq_poll_loop
    LABELS "layer2;scripting;zmq_poll_loop"
    TIMEOUT 30)

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

layer2_require_staged_utils(test_layer2_role_config)

pylabhub_add_gtest(TARGET test_layer2_role_config
    LABELS "layer2;service;config"
    TIMEOUT 30)

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

layer2_require_staged_utils(test_layer2_hub_config)

pylabhub_add_gtest(TARGET test_layer2_hub_config
    LABELS "layer2;service;config"
    TIMEOUT 30)

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

layer2_require_staged_utils(test_layer2_schema_validation)

pylabhub_add_gtest(TARGET test_layer2_schema_validation
    LABELS "layer2;service;schema"
    TIMEOUT 30)

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

layer2_require_staged_utils(test_layer2_engine_module_params)

pylabhub_add_gtest(TARGET test_layer2_engine_module_params
    LABELS "layer2;service;engine_module_params"
    TIMEOUT 30)

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

layer2_require_staged_utils(test_layer2_metrics_api)

pylabhub_add_gtest(TARGET test_layer2_metrics_api
    LABELS "layer2;service;metrics"
    TIMEOUT 60
    DISCOVERY_TIMEOUT 60)

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

layer2_require_staged_utils(test_layer2_role_data_loop)

pylabhub_add_gtest(TARGET test_layer2_role_data_loop
    LABELS "layer2;service;dataloop"
    TIMEOUT 30)
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

layer2_require_staged_utils(test_layer2_hub_zmq_queue)

pylabhub_add_gtest(TARGET test_layer2_hub_zmq_queue
    LABELS "layer2;service;zmq_queue"
    TIMEOUT 60)

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

layer2_require_staged_utils(test_layer2_dispatch_notifications)

pylabhub_add_gtest(TARGET test_layer2_dispatch_notifications
    LABELS "layer2;service;notifications"
    TIMEOUT 30)
