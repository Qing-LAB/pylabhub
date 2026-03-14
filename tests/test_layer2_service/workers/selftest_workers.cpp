/**
 * @file selftest_workers.cpp
 * @brief Worker scenarios for verifying the test framework's failure-propagation mechanism.
 *
 * These workers deliberately fail (assertion failures, exceptions) so that the parent
 * process can verify that run_gtest_worker / run_worker_bare correctly surface failures
 * as non-zero exit codes. If these workers are broken, no other worker-based test can
 * be trusted.
 */
#include "shared_test_helpers.h"
#include "test_entrypoint.h"
#include "gtest/gtest.h"

#include <stdexcept>
#include <string>

using namespace pylabhub::tests::helper;

static int dispatch_selftest(int argc, char **argv)
{
    if (argc < 2)
        return -1;

    std::string mode = argv[1];
    size_t dot_pos = mode.find('.');
    if (dot_pos == std::string::npos)
        return -1;

    std::string module = mode.substr(0, dot_pos);
    std::string scenario = mode.substr(dot_pos + 1);

    if (module != "selftest")
        return -1;

    // Scenario: ASSERT_TRUE(false) inside run_worker_bare — must return non-zero.
    if (scenario == "assert_fails")
    {
        return run_worker_bare(
            []() { ASSERT_TRUE(false) << "Intentional ASSERT_TRUE(false) in selftest"; },
            "selftest.assert_fails");
    }

    // Scenario: EXPECT_EQ(1, 2) inside run_worker_bare — must return non-zero.
    if (scenario == "expect_fails")
    {
        return run_worker_bare(
            []()
            {
                EXPECT_EQ(1, 2) << "Intentional EXPECT_EQ(1,2) in selftest";
                // EXPECT_* with throw_on_failure=true also throws.
            },
            "selftest.expect_fails");
    }

    // Scenario: std::runtime_error thrown inside run_worker_bare — must return non-zero.
    if (scenario == "exception_thrown")
    {
        return run_worker_bare(
            []() { throw std::runtime_error("Intentional std::runtime_error in selftest"); },
            "selftest.exception_thrown");
    }

    // Scenario: all assertions pass — must return zero (baseline).
    if (scenario == "passes")
    {
        return run_worker_bare(
            []()
            {
                ASSERT_TRUE(true);
                EXPECT_EQ(1, 1);
            },
            "selftest.passes");
    }

    return -1;
}

struct SelftestWorkerRegistrar
{
    SelftestWorkerRegistrar() { register_worker_dispatcher(dispatch_selftest); }
};
static SelftestWorkerRegistrar g_selftest_registrar;
