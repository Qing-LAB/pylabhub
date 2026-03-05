# tests/test_layer4_integration/CMakeLists.txt
#
# Layer 4 integration tests — admin shell, channel broadcast API, and pipeline
# round-trip.
#
# These tests spawn live binaries (pylabhub-hubshell, pylabhub-producer,
# pylabhub-consumer, pylabhub-processor) and verify end-to-end behaviour
# including ZMQ admin protocol, broadcast/notify control plane, Python exec(),
# and shared-memory data flow.

message(STATUS "** Configuring Layer 4 (Integration) tests...")

add_executable(test_layer4_integration
    test_admin_shell.cpp
    test_channel_broadcast.cpp
    test_pipeline_roundtrip.cpp
)

target_include_directories(test_layer4_integration PRIVATE
    ${CMAKE_SOURCE_DIR}/src                # hub_python/admin_shell.hpp (protocol docs)
)

target_link_libraries(test_layer4_integration
    PRIVATE
        pylabhub::test_framework   # WorkerProcess, IsolatedProcessTest, g_self_exe_path
        pylabhub::utils            # zmq_context, nlohmann_json, fmt
        GTest::gtest
)

target_compile_features(test_layer4_integration PRIVATE cxx_std_20)

# Suppress logger noise — these tests talk to external processes only
target_compile_definitions(test_layer4_integration PRIVATE LOGGER_COMPILE_LEVEL=0)

pylabhub_ensure_sanitizer_for_executable(test_layer4_integration)

# Ensure all binaries we spawn are staged
if(TARGET pylabhub-hubshell)
    add_dependencies(test_layer4_integration pylabhub-hubshell)
endif()
if(TARGET pylabhub-producer)
    add_dependencies(test_layer4_integration pylabhub-producer)
endif()
if(TARGET pylabhub-consumer)
    add_dependencies(test_layer4_integration pylabhub-consumer)
endif()
if(TARGET pylabhub-processor)
    add_dependencies(test_layer4_integration pylabhub-processor)
endif()

# Ensure pylabhub-utils.so is staged before tests run (RPATH $ORIGIN/../lib)
if(TARGET stage_pylabhub_utils)
    add_dependencies(test_layer4_integration stage_pylabhub_utils)
endif()

pylabhub_register_test_for_staging(TARGET test_layer4_integration)

gtest_discover_tests(test_layer4_integration
    PROPERTIES
        LABELS "layer4;integration"
        TIMEOUT 120
    DISCOVERY_TIMEOUT 60
)

message(STATUS "** Layer 4 (Integration) tests configured.")
message(STATUS "  - test_layer4_integration: admin shell + channel broadcast + pipeline round-trip")
