# tests/test_layer4_plh_hub/CMakeLists.txt
#
# Layer 4 plh_hub CLI tests — the single-kind hub binary
# (HEP-CORE-0033 §15).  Mirrors `tests/test_layer4_plh_role/` shape
# minus the parametrization (the hub is single-kind; no --role
# dispatch).
#
# Scope is the no-hub tier: --init, --validate, --keygen, and CLI
# error paths.  Each test spawns the staged `plh_hub` binary as a
# subprocess via WorkerProcess and inspects exit code + stdout/stderr
# + filesystem side effects.
#
# Run-mode / lifecycle / broker-coordination tests are deferred — see
# docs/todo/MESSAGEHUB_TODO.md "System-level L4 tests".

message(STATUS "** Configuring Layer 4 (plh_hub) tests...")

add_executable(test_layer4_plh_hub
    test_plh_hub_init.cpp
    test_plh_hub_validate.cpp
    test_plh_hub_keygen.cpp
    test_plh_hub_errors.cpp
    test_plh_hub_runmode.cpp
)

target_include_directories(test_layer4_plh_hub PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(test_layer4_plh_hub
    PRIVATE
        pylabhub::test_framework    # WorkerProcess, g_self_exe_path, custom main()
        pylabhub::utils             # nlohmann_json
        GTest::gtest
)

target_compile_features(test_layer4_plh_hub PRIVATE cxx_std_20)
target_compile_definitions(test_layer4_plh_hub PRIVATE LOGGER_COMPILE_LEVEL=0)

pylabhub_ensure_sanitizer_for_executable(test_layer4_plh_hub)

# Ensure the plh_hub binary is staged before the test runs.
if(TARGET plh_hub)
    add_dependencies(test_layer4_plh_hub plh_hub)
endif()

# Ensure pylabhub-utils.so is staged (RPATH $ORIGIN/../lib).
if(TARGET stage_pylabhub_utils)
    add_dependencies(test_layer4_plh_hub stage_pylabhub_utils)
endif()

pylabhub_register_test_for_staging(TARGET test_layer4_plh_hub)

gtest_discover_tests(test_layer4_plh_hub
    PROPERTIES
        LABELS "layer4;plh_hub"
        TIMEOUT 60
    DISCOVERY_TIMEOUT 60
)

message(STATUS "** Layer 4 (plh_hub) tests configured.")
