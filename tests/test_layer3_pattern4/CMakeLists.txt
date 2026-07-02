# tests/test_layer3_pattern4/CMakeLists.txt
#
# Layer 3 Pattern-4 reference tests (task #220).
#
# Pattern 4 = multi-process wire protocol test (subprocess-per-role +
# observing parent).  See `docs/README/README_testing.md` § "Pattern 4"
# for the design pattern.
#
# This binary holds reference implementations of Pattern 4 — the first
# is a smoke test that proves the bootstrap path (broker subprocess
# binds + role subprocess connects via CURVE + ZAP across the process
# boundary).  Once the bootstrap path is green under -j 2, follow-ups
# layer in REG_REQ / heartbeat / channel notify / etc., and migrate
# the existing L3 broker tests that currently use the retired
# in-process HubHostBrokerHandle co-host pattern (HEP-CORE-0036 §7.4
# antipattern).

cmake_minimum_required(VERSION 3.28)

include(MsvcHelper)

message(STATUS "** Configuring Layer 3 Pattern-4 reference tests...")

add_executable(test_layer3_pattern4
    test_pattern4_smoke.cpp
    test_pattern4_broker_wire_smoke.cpp
    test_pattern4_registration.cpp
    test_pattern4_heartbeat.cpp
    test_pattern4_consumer_lifecycle.cpp
    workers/pattern4_smoke_workers.cpp
    workers/pattern4_registration_workers.cpp
    workers/pattern4_heartbeat_workers.cpp
    workers/pattern4_consumer_lifecycle_workers.cpp
)

target_include_directories(test_layer3_pattern4 PRIVATE
    workers
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/scripting
    ${CMAKE_SOURCE_DIR}/src/utils
    ${CMAKE_SOURCE_DIR}/src/utils/ipc
)

target_link_libraries(test_layer3_pattern4
    PRIVATE
        pylabhub::test_framework
        pylabhub::utils
        pylabhub::scripting
        GTest::gtest
        GTest::gmock
)

target_compile_features(test_layer3_pattern4 PRIVATE cxx_std_20)
target_compile_definitions(test_layer3_pattern4 PRIVATE
    LOGGER_COMPILE_LEVEL=0
)

pylabhub_ensure_sanitizer_for_executable(test_layer3_pattern4)

if(TARGET stage_pylabhub_utils)
    add_dependencies(test_layer3_pattern4 stage_pylabhub_utils)
endif()

pylabhub_register_test_for_staging(TARGET test_layer3_pattern4)

gtest_discover_tests(test_layer3_pattern4
    PROPERTIES
        LABELS "layer3;pattern4"
    DISCOVERY_TIMEOUT 60
)

message(STATUS "** Layer 3 Pattern-4 reference tests configured.")
