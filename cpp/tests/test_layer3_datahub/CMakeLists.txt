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

    # ── Integration / Scenario ────────────────────────────────────────────────
    test_datahub_messagehub.cpp
    workers/datahub_messagehub_workers.cpp

    # ── WriteAttach mode (broker-owned shared memory) ─────────────────────────
    test_datahub_write_attach.cpp
    workers/datahub_write_attach_workers.cpp

    # ── Recovery scenarios (facility: zombie detection, force-reset, dead consumer) ──
    test_datahub_recovery_scenarios.cpp
    workers/datahub_recovery_scenario_workers.cpp

    # ── Integrity validation and repair ───────────────────────────────────────────
    test_datahub_integrity_repair.cpp
    workers/datahub_integrity_repair_workers.cpp
)

target_include_directories(test_layer3_datahub PRIVATE workers)

target_link_libraries(test_layer3_datahub
    PRIVATE
        pylabhub::test_framework
        pylabhub::utils
        GTest::gtest
        GTest::gmock
)

target_compile_features(test_layer3_datahub PRIVATE cxx_std_20)
target_compile_definitions(test_layer3_datahub PRIVATE LOGGER_COMPILE_LEVEL=0)

pylabhub_ensure_sanitizer_for_executable(test_layer3_datahub)

# Ensure pylabhub-utils is staged (soname symlink in stage-debug/lib/) before
# test discovery tries to load it via RPATH ($ORIGIN/../lib).
if(TARGET stage_pylabhub_utils)
    add_dependencies(test_layer3_datahub stage_pylabhub_utils)
endif()

pylabhub_register_test_for_staging(TARGET test_layer3_datahub)

gtest_discover_tests(test_layer3_datahub
    PROPERTIES
        LABELS "layer3;datahub"
    DISCOVERY_TIMEOUT 60
)

message(STATUS "** Layer 3 (DataHub) tests configured.")
message(STATUS "  - test_layer3_datahub: facility, c_api, primitive, raii, integration")
