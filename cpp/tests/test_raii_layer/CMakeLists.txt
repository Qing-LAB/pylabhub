# /cpp/tests/test_raii_layer/CMakeLists.txt
#
# Tests for Phase 3: C++ RAII Layer
# Tests Result<T,E>, SlotRef, ZoneRef, TransactionContext, and with_transaction API
#
cmake_minimum_required(VERSION 3.29)

# Test executable
add_executable(test_raii_layer
    test_result.cpp
)

target_link_libraries(test_raii_layer
    PRIVATE
    pylabhub-utils
    GTest::gtest
    GTest::gtest_main
)

# Use C++20
target_compile_features(test_raii_layer PRIVATE cxx_std_20)

# Register with GoogleTest
gtest_discover_tests(test_raii_layer
    WORKING_DIRECTORY ${PYLABHUB_STAGING_DIR}/tests
    PROPERTIES
        LABELS "raii;unit"
)

# Register for staging
pylabhub_register_test_for_staging(TARGET test_raii_layer)
