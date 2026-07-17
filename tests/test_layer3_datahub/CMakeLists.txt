# tests/test_layer3_datahub/CMakeLists.txt
#
# Layer 3 (DataHub) tests
# Tests for plh_datahub.hpp (depends on plh_service.hpp)
#
# Structure (ordered by abstraction level — facility → C API → primitive → RAII → integration):
#
#   Facility (individual class/subsystem unit tests):
#     test_datahub_schema_blds.cpp
#     test_datahub_schema_validation.cpp
#
#   C API layer (slot_rw_coordinator, checksum, recovery):
#     test_datahub_c_api_slot_protocol.cpp
#     test_datahub_c_api_checksum.cpp
#     test_datahub_c_api_recovery.cpp
#
#   C++ Primitive API (DataBlockProducer/Consumer, no RAII):
#     test_datahub_producer_consumer.cpp
#     test_datahub_mutex.cpp
#
#   C++ RAII layer (TransactionContext, SlotIterator, policies):
#     test_datahub_transaction_api.cpp
#     test_datahub_policy_enforcement.cpp
#     test_datahub_exception_safety.cpp
#     test_datahub_handle_semantics.cpp
#
#   Integration / Scenario (multi-module, end-to-end):
#     test_datahub_messagehub.cpp
#
#   workers/ — Worker process implementations (spawned as subprocesses)
#              Each worker file is named datahub_<module>_workers.{cpp,h}

message(STATUS "** Configuring Layer 3 (DataHub) tests...")

add_executable(test_layer3_datahub
    # ── Facility ──────────────────────────────────────────────────────────────
    test_datahub_schema_blds.cpp
    workers/datahub_schema_blds_workers.cpp
    test_datahub_schema_validation.cpp
    workers/datahub_schema_validation_workers.cpp
    test_datahub_config_validation.cpp
    workers/datahub_config_validation_workers.cpp
    test_datahub_header_structure.cpp
    workers/datahub_header_structure_workers.cpp

    # ── C API layer ───────────────────────────────────────────────────────────
    test_datahub_c_api_slot_protocol.cpp
    workers/datahub_c_api_slot_protocol_workers.cpp
    workers/datahub_c_api_draining_workers.cpp
    test_datahub_c_api_checksum.cpp
    workers/datahub_c_api_checksum_workers.cpp
    test_datahub_c_api_recovery.cpp
    workers/datahub_c_api_recovery_workers.cpp
    test_datahub_c_api_validation.cpp
    workers/datahub_c_api_validation_workers.cpp

    # ── C++ Primitive API ─────────────────────────────────────────────────────
    test_datahub_producer_consumer.cpp
    workers/datahub_producer_consumer_workers.cpp
    test_datahub_mutex.cpp
    workers/datahub_mutex_workers.cpp

    # ── C++ RAII layer ────────────────────────────────────────────────────────
    test_datahub_transaction_api.cpp
    workers/datahub_transaction_api_workers.cpp
    test_datahub_policy_enforcement.cpp
    workers/datahub_policy_enforcement_workers.cpp
    test_datahub_exception_safety.cpp
    workers/datahub_exception_safety_workers.cpp
    test_datahub_handle_semantics.cpp
    workers/datahub_handle_semantics_workers.cpp
    test_role_api_loop_policy.cpp
    workers/role_api_loop_policy_workers.cpp
    test_role_api_raii.cpp
    workers/role_api_raii_workers.cpp

    # ── Integration / Scenario (deleted — Step 7 Messenger removal) ─

    # ── WriteAttach RETIRED 2026-06-23 (HEP-CORE-0041 1i-cleanup / #275-S2) ──
    #
    # `test_datahub_write_attach.cpp` + `workers/datahub_write_attach_workers.cpp`
    # + `workers/datahub_write_attach_workers.h` deleted.  WriteAttach mode
    # (hub creates SHM segment, source process attaches as a writer via
    # `attach_datablock_as_writer_impl(name, secret, ...)`) is obsolete
    # under HEP-CORE-0041 capability transport: each producer mints its OWN
    # anonymous memfd via `memfd_create()` and owns it for the role's
    # lifetime — there is no "attach as writer to someone else's segment"
    # pattern anymore.  Zero production callers of
    # `attach_datablock_as_writer_impl` confirmed by grep before deletion;
    # the impl + signature retire under #275-S4.  Schema-validation
    # coverage that the WriteAttach tests doubled was redundant with
    # `test_datahub_schema_validation.cpp`; per-attach-mode coverage
    # gained nothing once the mode disappeared.

    # ── Recovery scenarios (facility: zombie detection, force-reset, dead consumer) ──
    test_datahub_recovery_scenarios.cpp
    workers/datahub_recovery_scenario_workers.cpp

    # ── Integrity validation and repair ───────────────────────────────────────────
    test_datahub_integrity_repair.cpp
    workers/datahub_integrity_repair_workers.cpp

    # ── Broker service integration tests ─────────────────────────────────────────
    # Unmasked 2026-06-29 under #154 batch-2a C2 — migrated to KeyStore-based
    # `start_broker_in_thread` + `known_roles.json` (HEP-CORE-0035 §4.8 +
    # HEP-CORE-0040 §172).
    test_datahub_broker.cpp
    workers/datahub_broker_workers.cpp

    # ── Role identity policy enforcement tests (placeholder — HEP-0035) ───────
    # MASKED — slated for HEP-CORE-0035 §8 Phase 6 deletion (legacy machinery).
    # test_datahub_role_identity_policy.cpp
    # workers/role_identity_policy_workers.cpp

    # ── DataBlock slot-allocation tests ──────────────────────────────────────
    # fd-source destination ② for the retired ShmQueue slot-allocation coverage
    # (#275-S2); asserts HEP-CORE-0002 layout invariants (cache-line rounding,
    # ring_buffer_capacity, total_block_size scaling).
    test_datahub_slot_allocation.cpp
    workers/datahub_slot_allocation_workers.cpp

    # ── Consumer registration protocol tests ─────────────────────────────────────
    # MIGRATED to tests/test_layer3_pattern4/test_pattern4_broker_consumer.cpp
    # (task #52 Round 2 — all 15 workers were wire-only; HubHostBrokerHandle
    # co-host harness retired for this concern).

    # ── e2e (broker in-thread + producer/consumer subprocesses) RETIRED ──
    # 2026-06-23 (HEP-CORE-0041 1i-cleanup / #275-S2 Batch 1)
    #
    # `test_datahub_e2e.cpp` + `workers/datahub_e2e_workers.cpp` + `.h`
    # deleted.  The test pre-dated HEP-CORE-0041 and drove the legacy
    # name+secret discovery path between separate subprocesses
    # (`find_datablock_consumer_impl(shm_name, kE2ESecret, ...)`).  Under
    # capability transport the cross-process attach requires the full L2
    # auth dance (broker pre-confirm + crypto_box challenge-response +
    # SCM_RIGHTS) — which is precisely what #258 1k L4 e2e test builds
    # against real producer + broker + consumer role hosts.  Reimplementing
    # that wire-up in this L3 test would duplicate #258's scope without
    # adding coverage.  Was already MASKED in CMakeLists (commented out)
    # pre-S2 awaiting this disposition.

    # ── ChannelHandle integration tests (deleted — Step 7 Messenger removal) ─

    # ── Hub Producer/Consumer unified API tests (deleted — Step 7 Messenger removal) ─

    # ── Broker/Producer/Consumer health and notification tests ────────────────────
    # Unmasked 2026-06-30 under #154 batch-2a C4 — migrated to shared
    # broker_test_harness (HubHostBrokerHandle + DirectBrokerHandle +
    # BrcHandle + make_reg_opts/make_cons_opts) + seed_curve_identities
    # per AUTH-6 audit.  10 single-broker L3 tests.
    test_datahub_broker_health.cpp
    workers/datahub_broker_health_workers.cpp

    # ── Broker role-liveness state machine (HEP-CORE-0023 §2.5) ──────────────────
    # Unmasked 2026-06-29 under #154 batch-2a C3 — migrated to shared
    # broker_test_harness (DirectBrokerHandle + BrcHandle + make_reg_opts /
    # make_cons_opts) + seed_curve_identities per AUTH-6 audit.
    test_datahub_role_state_machine.cpp
    workers/datahub_role_state_workers.cpp

    # ── RAII layer stress tests (multi-process, full-capacity, random delays) ─────
    test_datahub_stress_raii.cpp
    workers/datahub_stress_raii_workers.cpp

    # NOTE: test_datahub_hub_config_script.cpp deleted 2026-04-29 — it
    # tested the legacy `pylabhub::HubConfig` lifecycle singleton, which
    # was retired in favour of the HEP-CORE-0033 §6.1 `pylabhub::config::HubConfig`
    # composite (tests for the replacement live in test_layer2_service/).

    # ── Hub ShmQueue unit tests: RETIRED (HEP-CORE-0041 1i-cleanup S3b, #275) ──
    # The 25 TEST_F's + 25 workers / 45 legacy-factory call sites were
    # retired 2026-06-30 with the ShmQueue legacy public surface (S3c).
    # Coverage continuity per TESTING_TODO § "Test retirements /
    # cross-layer migrations" row dated 2026-06-30.  Physical file
    # deletion shipped in REVIEW-C (#276).

    # ── Role-API integration: flexzone round-trip through RoleAPIBase ────────
    test_role_api_flexzone.cpp
    workers/role_api_flexzone_workers.cpp

    # ── Hub ZmqQueue unit tests ──────────────────────────────────────────────
    # NOTE: test_datahub_hub_zmq_queue.cpp was relocated 2026-05-14 to
    # `tests/test_layer2_service/test_hub_zmq_queue.cpp` and given its
    # own executable `test_layer2_hub_zmq_queue` using Pattern 1+
    # (BinaryLifecycleEnvironment).  ZmqQueue is a single-class module
    # (no broker / multi-module / protocol) — L2 is the correct layer
    # per the test-layering purpose framework; previous L3 placement
    # was historical (shared aggregate lifecycle guard).  Same
    # correction the L1/L2 split applied to test_zmq_poll_loop.
    # test_datahub_hub_zmq_queue.cpp

    # ── Schema parser unit tests (HEP-CORE-0034 §2.4 I5; renamed from
    # test_datahub_schema_library.cpp in Phase 4c, 2026-04-28) ────────────────
    test_datahub_schema_loader.cpp
    workers/datahub_schema_loader_workers.cpp

    # ── Broker named schema protocol tests (HEP-CORE-0034 path B / inbox path A) ──
    # MIGRATED to tests/test_layer3_pattern4/test_pattern4_broker_schema.cpp
    # (task #52 — last hybrid schema_hash_stored reads the broker's
    # RegReqAccepted schema_hash trace instead of the in-process snapshot).

    # NOTE: test_datahub_schema_registry.cpp (HEP-0016 SchemaStore lifecycle)
    # was deleted in HEP-0034 Phase 4a — the SchemaStore singleton was
    # removed; runtime schema state now lives in HubState.schemas
    # (covered by test_hub_state.cpp + test_datahub_broker.cpp Sch_*).

    # ── BrokerService admin API tests ─────────────────────────────────────────
    # Unmasked 2026-06-07 under #154 — migrated to seed_curve_identities pattern.
    test_datahub_broker_admin.cpp
    workers/broker_admin_workers.cpp

    # ── Broker control-plane protocol tests (broadcast, notify, list_channels) ──
    # Unmasked 2026-06-28 under #154 batch-2a C1 — migrated to shared
    # broker_test_harness (HubHostBrokerHandle + BrcHandle) +
    # seed_curve_identities + §5b canonical builders per AUTH-6 audit.
    test_datahub_broker_protocol.cpp
    workers/datahub_broker_protocol_workers.cpp

    # ── HubHost ↔ BrokerService integration (HEP-CORE-0033 §4 / Phase 6.1b) ──
    # Unmasked 2026-06-27 under #154 — workers file already uses the
    # seed_curve_identities + `start_hubhost_broker` + `BrcHandle` migration
    # pattern (cf. tests/test_framework/broker_test_harness.h).  Pre-migration
    # MASK was a stale wrapper; file builds + runs against current lib.
    test_datahub_hub_host_integration.cpp
    workers/hub_host_integration_workers.cpp

    # ── HubHost + LuaEngine integration (HEP-CORE-0033 Phase 7 D3.3) ────────
    # Unmasked 2026-06-27 under #154 / #177 — workers now seed the
    # process KeyStore via seed_curve_identities (HEP-CORE-0040 §172 +
    # HEP-CORE-0035 §4.6.5 bypass discipline) before each HubHost::startup(),
    # so BrokerService's KeyStore["hub_identity"] check succeeds.
    test_hub_lua_integration.cpp
    workers/hub_lua_integration_workers.cpp

    # ── HubHost + PythonEngine integration (HEP-CORE-0033 Phase 7 D4.2) ────
    # Unmasked 2026-06-27 under #154 / #177 — worker now seeds the
    # process KeyStore via seed_curve_identities (HEP-CORE-0040 §172 +
    # HEP-CORE-0035 §4.6.5 bypass discipline) before HubHost::startup(),
    # so BrokerService's KeyStore["hub_identity"] check succeeds.
    test_hub_python_integration.cpp
    workers/hub_python_integration_workers.cpp

    # ── Messenger hex codec tests (deleted — Step 7 Messenger removal) ─

    # ── ZmqPollLoop + PeriodicTask unit tests ────────────────────────────────
    # Moved 2026-05-14 to `tests/test_layer2_service/test_zmq_poll_loop.cpp`
    # (own executable `test_layer2_zmq_poll_loop`).  The subjects
    # (`pylabhub::scripting::PeriodicTask` + `ZmqPollLoop`) are
    # utility classes — L1 / L2 by the test-layering purpose
    # framework — not L3 datahub protocol tests.  The placement here
    # was historical.  The new executable uses
    # `BinaryLifecycleEnvironment` (binary-wide LifecycleGuard via
    # gtest `::testing::Environment`) which requires being the only
    # `LifecycleGuard` owner in its binary — incompatible with the
    # antipattern SetUpTestSuite-owned guards still present in this
    # aggregate.

    # ── HEP-CORE-0019 Metrics Plane tests ────────────────────────────────
    test_datahub_metrics.cpp
    workers/datahub_metrics_workers.cpp

    # ── HEP-CORE-0021 ZMQ Endpoint Registry protocol tests ────────────
    # MIGRATED to tests/test_layer3_pattern4/test_pattern4_zmq_endpoint_registry.cpp
    # (task #52 Round 2 — all 8 workers wire-only; shm_and_zmq_coexist +
    # req_shape reframed off in-process reads to CHANNEL_LIST_REQ / wire drain).

    # ── HEP-CORE-0022 Hub Federation Broadcast protocol tests ────────────
    # DEFER (#154 AUTH-6 C6, 2026-06-30 — audit File 9): TEST_F driver
    # unmasked but every TEST_F is GTEST_SKIP-gated on task #105
    # (federation protocol design).  Workers file stays masked because
    # its bodies don't compile against the post-strict-CURVE broker
    # surface yet — re-enable when #105 ships and the federation broker
    # handlers land.  See
    # `docs/code_review/REVIEW_AUTH6_TestDisposition_2026-06-27.md`
    # File 9 disposition + § "Recommended action plan" Phase 3.
    test_datahub_hub_federation.cpp
    # workers/hub_federation_workers.cpp

    # ── Hub InboxQueue + InboxClient unit tests (Phase 3 Inbox Facility) ─
    test_datahub_hub_inbox_queue.cpp
    workers/hub_inbox_queue_workers.cpp

    # ── MonitoredQueue unit tests (ctrl-send queue with backpressure) ─────
    test_datahub_hub_monitored_queue.cpp

    # ── BrokerRequestComm integration tests ──
    # Unmasked 2026-06-07 under #154 — migrated to KeyStore-based
    # `start_hubhost_broker` (HEP-CORE-0040 §172 + HEP-CORE-0035 §4.6.5).
    test_datahub_broker_request_comm.cpp
    workers/datahub_broker_request_comm_workers.cpp
)

target_include_directories(test_layer3_datahub PRIVATE
    workers
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/scripting
    ${CMAKE_SOURCE_DIR}/src/utils
    ${CMAKE_SOURCE_DIR}/src/utils/ipc
)

target_link_libraries(test_layer3_datahub
    PRIVATE
        pylabhub::test_framework
        pylabhub::utils
        pylabhub::scripting
        pylabhub::third_party::pybind11_embed
        GTest::gtest
        GTest::gmock
)

target_compile_features(test_layer3_datahub PRIVATE cxx_std_20)
target_compile_definitions(test_layer3_datahub PRIVATE
    LOGGER_COMPILE_LEVEL=0
    TEST_PLUGIN_DIR="${PYLABHUB_STAGING_DIR}/tests"
)
# GCC false-positive: -Wmissing-field-initializers fires for C++20 designated
# initializers even when omitted fields have in-class defaults.  Not applicable
# to ChannelRegistrationOptions, which provides a default for every member.
target_compile_options(test_layer3_datahub PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wno-missing-field-initializers>)

pylabhub_ensure_sanitizer_for_executable(test_layer3_datahub)

# Ensure pylabhub-utils is staged (soname symlink in stage-debug/lib/) before
# test discovery tries to load it via RPATH ($ORIGIN/../lib).
if(TARGET stage_pylabhub_utils)
    add_dependencies(test_layer3_datahub stage_pylabhub_utils)
endif()
if(TARGET test_native_multifield_module)
    add_dependencies(test_layer3_datahub test_native_multifield_module)
endif()

pylabhub_register_test_for_staging(TARGET test_layer3_datahub)

gtest_discover_tests(test_layer3_datahub
    PROPERTIES
        FIXTURES_REQUIRED Guardrails
        LABELS "layer3;datahub"
    DISCOVERY_TIMEOUT 60
)

message(STATUS "** Layer 3 (DataHub) tests configured.")
message(STATUS "  - test_layer3_datahub: facility, c_api, primitive, raii, integration, broker_consumer, e2e")
