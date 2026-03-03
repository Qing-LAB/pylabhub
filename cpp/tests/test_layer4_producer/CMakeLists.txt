# tests/test_layer4_producer/CMakeLists.txt
#
# Layer 4 producer tests — config unit tests + CLI integration tests.
#
# Two test classes in one executable:
#   ProducerConfigTest (PureApiTest)   — tests ProducerConfig::from_json_file/from_directory
#   ProducerCliTest (IsolatedProcessTest) — spawns pylabhub-producer binary via WorkerProcess
#
# producer_config.cpp is compiled directly into this binary (it is private to the
# pylabhub-producer executable, not part of pylabhub-utils).

message(STATUS "** Configuring Layer 4 (Producer) tests...")

add_executable(test_layer4_producer
    test_producer_config.cpp
    test_producer_cli.cpp
    ${CMAKE_SOURCE_DIR}/src/producer/producer_config.cpp
)

target_include_directories(test_layer4_producer PRIVATE
    ${CMAKE_SOURCE_DIR}/src/producer          # producer_config.hpp
    ${CMAKE_SOURCE_DIR}/src/scripting         # scripting/python_script_host.hpp (transitive)
)

target_link_libraries(test_layer4_producer
    PRIVATE
        pylabhub::test_framework   # WorkerProcess, g_self_exe_path, custom main()
        pylabhub::utils            # actor_vault, uid_utils, nlohmann_json, etc.
        GTest::gtest
)

target_compile_features(test_layer4_producer PRIVATE cxx_std_20)

# Suppress logger noise in unit tests; config tests don't init Logger
target_compile_definitions(test_layer4_producer PRIVATE LOGGER_COMPILE_LEVEL=0)

pylabhub_ensure_sanitizer_for_executable(test_layer4_producer)

# Ensure the producer binary is staged (CLI tests invoke it at ../bin/pylabhub-producer)
if(TARGET pylabhub-producer)
    add_dependencies(test_layer4_producer pylabhub-producer)
endif()

# Ensure pylabhub-utils.so is staged before tests run (RPATH $ORIGIN/../lib)
if(TARGET stage_pylabhub_utils)
    add_dependencies(test_layer4_producer stage_pylabhub_utils)
endif()

pylabhub_register_test_for_staging(TARGET test_layer4_producer)

gtest_discover_tests(test_layer4_producer
    PROPERTIES
        LABELS "layer4;producer"
        TIMEOUT 120
    DISCOVERY_TIMEOUT 60
)

message(STATUS "** Layer 4 (Producer) tests configured.")
message(STATUS "  - test_layer4_producer: config unit tests + CLI integration tests")
