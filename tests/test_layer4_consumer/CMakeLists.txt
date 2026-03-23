# tests/test_layer4_consumer/CMakeLists.txt
#
# Layer 4 consumer tests — CLI behavior tests.
#
# Spawns pylabhub-consumer binary via WorkerProcess.
# Tests: --init, --keygen, --validate, config errors.
# Config parsing is tested via L2 RoleConfigTest.
#
# CLI behavior tests (--init, --keygen, --validate, error handling).
# pylabhub-consumer executable, not part of pylabhub-utils).

message(STATUS "** Configuring Layer 4 (Consumer) tests...")

add_executable(test_layer4_consumer
    test_consumer_cli.cpp
)

target_include_directories(test_layer4_consumer PRIVATE
    ${CMAKE_SOURCE_DIR}/src/consumer
    ${CMAKE_SOURCE_DIR}/src/scripting
)

target_link_libraries(test_layer4_consumer
    PRIVATE
        pylabhub::test_framework   # WorkerProcess, g_self_exe_path, custom main()
        pylabhub::utils            # role_vault, uid_utils, nlohmann_json, etc.
        GTest::gtest
)

target_compile_features(test_layer4_consumer PRIVATE cxx_std_20)

target_compile_definitions(test_layer4_consumer PRIVATE LOGGER_COMPILE_LEVEL=0)

pylabhub_ensure_sanitizer_for_executable(test_layer4_consumer)

# Ensure the consumer binary is staged (CLI tests invoke it at ../bin/pylabhub-consumer)
if(TARGET pylabhub-consumer)
    add_dependencies(test_layer4_consumer pylabhub-consumer)
endif()

# Ensure pylabhub-utils.so is staged before tests run (RPATH $ORIGIN/../lib)
if(TARGET stage_pylabhub_utils)
    add_dependencies(test_layer4_consumer stage_pylabhub_utils)
endif()

pylabhub_register_test_for_staging(TARGET test_layer4_consumer)

gtest_discover_tests(test_layer4_consumer
    PROPERTIES
        LABELS "layer4;consumer"
        TIMEOUT 120
    DISCOVERY_TIMEOUT 60
)

message(STATUS "** Layer 4 (Consumer) tests configured.")
message(STATUS "  - test_layer4_consumer: config unit tests + CLI integration tests")
