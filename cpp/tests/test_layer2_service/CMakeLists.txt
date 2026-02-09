# tests/test_layer2_service/CMakeLists.txt
#
# Layer 2: Service layer tests
# Tests for plh_service.hpp (depends on plh_base.hpp)

message(STATUS "** Configuring Layer 2 (Service) tests...")

add_executable(test_layer2_service
  # Placeholder during Phase 0
  test_service_placeholder.cpp

  # To be created/migrated in Phase 3
  # test_backoff_strategy.cpp       # NEW - CRITICAL
  # test_crypto_utils.cpp            # NEW - CRITICAL
  # test_lifecycle.cpp               # Migrate from test_pylabhub_utils
  # test_lifecycle_dynamic.cpp       # Migrate from test_pylabhub_utils
  # test_file_lock.cpp               # Migrate from test_pylabhub_utils
  # test_logger.cpp                  # Migrate from test_pylabhub_utils

  # Worker processes for multi-process tests
  # workers/filelock_workers.cpp
  # workers/lifecycle_workers.cpp
  # workers/worker_dispatcher.cpp
)

target_link_libraries(test_layer2_service
  PRIVATE
    pylabhub::test_framework
    pylabhub::utils
)

target_compile_features(test_layer2_service PRIVATE cxx_std_20)
target_compile_definitions(test_layer2_service PRIVATE LOGGER_COMPILE_LEVEL=0)

if(PYLABHUB_SANITIZER_FLAGS_SET)
  target_compile_options(test_layer2_service PRIVATE $<TARGET_PROPERTY:pylabhub::sanitizer_flags,INTERFACE_COMPILE_OPTIONS>)
  target_link_libraries(test_layer2_service PRIVATE pylabhub::sanitizer_flags)
endif()

pylabhub_register_test_for_staging(TARGET test_layer2_service)

set_property(TARGET test_layer2_service PROPERTY
  TEST_LAUNCHER "${CMAKE_COMMAND}" -E env --modify "PATH=path_list_append:${PYLABHUB_STAGING_DIR}/bin"
)

gtest_discover_tests(test_layer2_service
  WORKING_DIRECTORY "${PYLABHUB_STAGING_DIR}/tests"
  DISCOVERY_TIMEOUT 60
)

message(STATUS "** Layer 2 tests configured.")
