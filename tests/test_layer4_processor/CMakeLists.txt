# tests/test_layer4_processor/CMakeLists.txt
#
# Layer 4 processor tests — CLI behavior tests.
#
# Spawns pylabhub-processor binary via WorkerProcess.
# Tests: --init, --keygen, --validate, config errors.
# Config parsing is tested via L2 RoleConfigTest.
#
# CLI behavior tests (--init, --keygen, --validate, error handling).
# pylabhub-processor executable, not part of pylabhub-utils).

message(STATUS "** Configuring Layer 4 (Processor) tests...")

add_executable(test_layer4_processor
    test_processor_cli.cpp
)

target_include_directories(test_layer4_processor PRIVATE
    ${CMAKE_SOURCE_DIR}/src/processor
    ${CMAKE_SOURCE_DIR}/src/scripting
)

target_link_libraries(test_layer4_processor
    PRIVATE
        pylabhub::test_framework   # WorkerProcess, g_self_exe_path, custom main()
        pylabhub::utils            # role_vault, uid_utils, nlohmann_json, etc.
        GTest::gtest
)

target_compile_features(test_layer4_processor PRIVATE cxx_std_20)

# Suppress logger noise in unit tests; config tests don't init Logger
target_compile_definitions(test_layer4_processor PRIVATE LOGGER_COMPILE_LEVEL=0)

pylabhub_ensure_sanitizer_for_executable(test_layer4_processor)

# Ensure the processor binary is staged (CLI tests invoke it at ../bin/pylabhub-processor)
if(TARGET pylabhub-processor)
    add_dependencies(test_layer4_processor pylabhub-processor)
endif()

# Ensure pylabhub-utils.so is staged before tests run (RPATH $ORIGIN/../lib)
if(TARGET stage_pylabhub_utils)
    add_dependencies(test_layer4_processor stage_pylabhub_utils)
endif()

pylabhub_register_test_for_staging(TARGET test_layer4_processor)

gtest_discover_tests(test_layer4_processor
    PROPERTIES
        LABELS "layer4;processor"
        TIMEOUT 120
    DISCOVERY_TIMEOUT 60
)

message(STATUS "** Layer 4 (Processor) tests configured.")
message(STATUS "  - test_layer4_processor: config unit tests + CLI integration tests")
