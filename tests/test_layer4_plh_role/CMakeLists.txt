# tests/test_layer4_plh_role/CMakeLists.txt
#
# Layer 4 plh_role CLI tests — the unified role binary introduced by
# HEP-0024 Phase 19.  These tests supersede the per-role
# test_layer4_{producer,consumer,processor} dirs (removed in Phase 20).
#
# Scope here is the no-hub tier: --init, --validate, --keygen, and
# CLI-level error paths.  Each test spawns `plh_role` as a subprocess
# via WorkerProcess and inspects exit code + stdout/stderr + file-system
# side effects.
#
# Run-mode / lifecycle / broker-round-trip tests are deliberately
# deferred — see docs/todo/TESTING_TODO.md "Deferred L4 broker tests"
# for the to-do list of tests that need a live hubshell to exercise.

message(STATUS "** Configuring Layer 4 (plh_role) tests...")

add_executable(test_layer4_plh_role
    test_plh_role_init.cpp
    test_plh_role_validate.cpp
    test_plh_role_keygen.cpp
    test_plh_role_errors.cpp
)

target_include_directories(test_layer4_plh_role PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(test_layer4_plh_role
    PRIVATE
        pylabhub::test_framework    # WorkerProcess, g_self_exe_path, custom main()
        pylabhub::utils             # role_directory, role_vault, nlohmann_json
        GTest::gtest
)

target_compile_features(test_layer4_plh_role PRIVATE cxx_std_20)
target_compile_definitions(test_layer4_plh_role PRIVATE LOGGER_COMPILE_LEVEL=0)

pylabhub_ensure_sanitizer_for_executable(test_layer4_plh_role)

# Ensure plh_role binary is staged before the test runs.
if(TARGET plh_role)
    add_dependencies(test_layer4_plh_role plh_role)
endif()

# Ensure pylabhub-utils.so is staged (RPATH $ORIGIN/../lib).
if(TARGET stage_pylabhub_utils)
    add_dependencies(test_layer4_plh_role stage_pylabhub_utils)
endif()

pylabhub_register_test_for_staging(TARGET test_layer4_plh_role)

gtest_discover_tests(test_layer4_plh_role
    PROPERTIES
        LABELS "layer4;plh_role"
        TIMEOUT 120
    DISCOVERY_TIMEOUT 60
)

message(STATUS "** Layer 4 (plh_role) tests configured.")
