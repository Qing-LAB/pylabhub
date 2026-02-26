# tests/test_layer4_actor/CMakeLists.txt
#
# Layer 4 (Actor) tests — pylabhub-actor configuration and role API
#
# Structure:
#   test_actor_config.cpp          — ActorConfig::from_json_file() parsing
#   test_actor_role_metrics.cpp    — ActorRoleAPI metric/diagnostic methods (inline, no Python init)
#   test_actor_embedded_mode.cpp   — Producer/Consumer embedded-mode API (HEP-CORE-0010 Phase 2)
#                                    Uses an in-process BrokerService (no CURVE encryption).
#                                    zmq_context_startup/shutdown called directly; Logger silently
#                                    drops messages when not initialised (checked in logger.cpp).
#
# Design notes:
#   - actor_config.cpp is compiled directly into the test binary (it is not part of any library).
#   - actor_api.hpp is included for the inline metric methods; actor_api.cpp is not compiled
#     because the tested methods are all inline and no Python callbacks are exercised.
#   - LOGGER_COMPILE_LEVEL=0 makes all LOGGER_* macros no-ops, so the Logger lifecycle
#     module does not need to be started.
#   - pybind11_embed is linked for pybind11 header availability (actor_api.hpp includes it);
#     the Python interpreter is never initialised in these tests.

cmake_minimum_required(VERSION 3.29)

message(STATUS "** Configuring Layer 4 (Actor) tests...")

add_executable(test_layer4_actor
    test_actor_config.cpp
    test_actor_role_metrics.cpp
    test_actor_embedded_mode.cpp
    ${CMAKE_SOURCE_DIR}/src/actor/actor_config.cpp
)

target_include_directories(test_layer4_actor PRIVATE
    ${CMAKE_SOURCE_DIR}/src/actor
)

target_compile_features(test_layer4_actor PRIVATE cxx_std_20)
target_compile_definitions(test_layer4_actor PRIVATE LOGGER_COMPILE_LEVEL=0)

target_link_libraries(test_layer4_actor PRIVATE
    pylabhub::utils
    pylabhub::third_party::pybind11_embed
    GTest::gtest_main
    GTest::gmock
)

pylabhub_apply_sanitizer_to_target(test_layer4_actor)

# Ensure pylabhub-utils is staged before test discovery uses its RPATH.
if(TARGET stage_pylabhub_utils)
    add_dependencies(test_layer4_actor stage_pylabhub_utils)
endif()

pylabhub_register_test_for_staging(TARGET test_layer4_actor)

gtest_discover_tests(test_layer4_actor
    PROPERTIES
        LABELS "layer4;actor"
    DISCOVERY_TIMEOUT 60
)

message(STATUS "** Layer 4 (Actor) tests configured.")
message(STATUS "  - test_layer4_actor: actor_config parsing, role metrics API, embedded-mode API")
