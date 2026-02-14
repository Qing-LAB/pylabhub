# tests/test_layer3_datahub/CMakeLists.txt
#
# Layer 3 (DataHub) tests
# Tests for plh_datahub.hpp (depends on plh_service.hpp)
#
# Structure:
#   test_*.cpp    — GTest test suites
#   workers/      — Worker process implementations (spawned as subprocesses)

message(STATUS "** Configuring Layer 3 (DataHub) tests...")

# ============================================================================
# Test: Schema BLDS
# Pure API tests (types, builder, version) + isolated-process SchemaInfo tests
# ============================================================================

add_executable(test_layer3_datahub
    test_schema_blds.cpp
    workers/schema_blds_workers.cpp
    test_schema_validation.cpp
    workers/schema_validation_workers.cpp
    test_recovery_api.cpp
    workers/recovery_workers.cpp
    test_slot_protocol.cpp
    workers/slot_protocol_workers.cpp
    test_phase_a_protocol.cpp
    workers/phase_a_workers.cpp
    test_error_handling.cpp
    workers/error_handling_workers.cpp
    test_message_hub.cpp
    workers/messagehub_workers.cpp
    test_transaction_api.cpp
    workers/transaction_api_workers.cpp
    test_datablock_mutex.cpp
    workers/datablock_management_mutex_workers.cpp
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
        LABELS "layer3;datahub;schema"
    DISCOVERY_TIMEOUT 60
)

message(STATUS "** Layer 3 (DataHub) tests configured.")
message(STATUS "  - test_layer3_datahub (schema_blds, schema_validation, recovery_api, slot_protocol, phase_a_protocol, error_handling, message_hub, transaction_api)")
