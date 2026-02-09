# tests/test_layer3_datahub/CMakeLists.txt
#
# Layer 3: DataHub layer tests
# Tests for plh_datahub.hpp (depends on plh_service.hpp)

message(STATUS "** Configuring Layer 3 (DataHub) tests...")

add_executable(test_layer3_datahub
  # Placeholder during Phase 0
  test_datahub_placeholder.cpp

  # Schema validation (Phase 4 - CRITICAL for P9.2)
  # test_schema_blds.cpp             # NEW - CRITICAL
  # test_schema_validation.cpp       # REFACTOR - expand coverage

  # DataHub core (Phases 5-7)
  # test_json_config.cpp             # Migrate
  # test_message_hub.cpp             # Migrate
  # test_datablock_mutex.cpp         # Migrate + rename
  # test_datablock_spinlock.cpp      # NEW
  # test_datablock_core.cpp          # NEW - split from test_datablock.cpp
  # test_datablock_slot_coordination.cpp  # NEW - split
  # test_datablock_transactions.cpp  # MERGE test_datablock.cpp + test_transaction_api.cpp
  # test_datablock_ringbuffer.cpp    # NEW
  # test_datablock_checksum.cpp      # NEW
  # test_datablock_schema.cpp        # Integrated schema tests
  # test_datablock_multiprocess.cpp  # NEW - multi-process IPC
  # test_datablock_benchmarks.cpp    # Migrate + rename

  # Worker processes for multi-process DataBlock tests
  # workers/datablock_workers.cpp
  # workers/worker_dispatcher.cpp
)

target_link_libraries(test_layer3_datahub
  PRIVATE
    pylabhub::test_framework
    pylabhub::utils
)

target_compile_features(test_layer3_datahub PRIVATE cxx_std_20)
target_compile_definitions(test_layer3_datahub PRIVATE LOGGER_COMPILE_LEVEL=0)

if(PYLABHUB_SANITIZER_FLAGS_SET)
  target_compile_options(test_layer3_datahub PRIVATE $<TARGET_PROPERTY:pylabhub::sanitizer_flags,INTERFACE_COMPILE_OPTIONS>)
  target_link_libraries(test_layer3_datahub PRIVATE pylabhub::sanitizer_flags)
endif()

pylabhub_register_test_for_staging(TARGET test_layer3_datahub)

set_property(TARGET test_layer3_datahub PROPERTY
  TEST_LAUNCHER "${CMAKE_COMMAND}" -E env --modify "PATH=path_list_append:${PYLABHUB_STAGING_DIR}/bin"
)

gtest_discover_tests(test_layer3_datahub
  WORKING_DIRECTORY "${PYLABHUB_STAGING_DIR}/tests"
  DISCOVERY_TIMEOUT 60
)

message(STATUS "** Layer 3 tests configured.")
