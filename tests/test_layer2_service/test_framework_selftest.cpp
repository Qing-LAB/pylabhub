/**
 * @file test_framework_selftest.cpp
 * @brief Self-verification tests for the worker process test framework.
 *
 * These tests MUST pass before any other worker-based tests are trusted.
 * They verify that when a worker process fails (assertion failure, exception),
 * the parent process correctly observes a non-zero exit code.
 *
 * If these tests fail, the run_gtest_worker / run_worker_bare mechanism is broken
 * and all worker-based tests in every other test binary would be producing
 * false-positive results.
 */
#include "shared_test_helpers.h"
#include "test_entrypoint.h"
#include "test_process_utils.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

using namespace pylabhub::tests::helper;
using namespace ::testing;

class FrameworkSelftestTest : public ::testing::Test
{
};

/**
 * Verifies that ASSERT_TRUE(false) inside a worker causes a non-zero exit code.
 * This ensures run_worker_bare properly propagates GTest assertion failures.
 */
TEST_F(FrameworkSelftestTest, AssertFailurePropagatesToParent)
{
    WorkerProcess proc(g_self_exe_path, "selftest.assert_fails", {});
    ASSERT_TRUE(proc.valid());
    int exit_code = proc.wait_for_exit();
    EXPECT_NE(exit_code, 0)
        << "ASSERT_TRUE(false) in worker should produce non-zero exit, got 0.\n"
        << "This means run_worker_bare is silently swallowing assertion failures.\n"
        << "Stderr: " << proc.get_stderr();
    EXPECT_THAT(proc.get_stderr(), HasSubstr("[WORKER"))
        << "Expected [WORKER...] failure message in stderr";
}

/**
 * Verifies that EXPECT_EQ(1, 2) inside a worker causes a non-zero exit code.
 * With throw_on_failure=true, EXPECT_* failures also throw and are caught.
 */
TEST_F(FrameworkSelftestTest, ExpectFailurePropagatesToParent)
{
    WorkerProcess proc(g_self_exe_path, "selftest.expect_fails", {});
    ASSERT_TRUE(proc.valid());
    int exit_code = proc.wait_for_exit();
    EXPECT_NE(exit_code, 0) << "EXPECT_EQ(1, 2) in worker should produce non-zero exit, got 0.\n"
                            << "Stderr: " << proc.get_stderr();
    EXPECT_THAT(proc.get_stderr(), HasSubstr("[WORKER"))
        << "Expected [WORKER...] failure message in stderr";
}

/**
 * Verifies that std::runtime_error thrown inside a worker causes a non-zero exit code.
 * Ensures run_worker_bare catches std::exception and returns non-zero.
 */
TEST_F(FrameworkSelftestTest, StdExceptionPropagatesToParent)
{
    WorkerProcess proc(g_self_exe_path, "selftest.exception_thrown", {});
    ASSERT_TRUE(proc.valid());
    int exit_code = proc.wait_for_exit();
    EXPECT_NE(exit_code, 0) << "std::runtime_error in worker should produce non-zero exit, got 0.\n"
                            << "Stderr: " << proc.get_stderr();
    EXPECT_THAT(proc.get_stderr(), HasSubstr("[WORKER"))
        << "Expected [WORKER...] failure message in stderr";
}

/**
 * Verifies that a passing worker exits with code 0.
 * Baseline test: confirms the mechanism itself is not always returning non-zero.
 */
TEST_F(FrameworkSelftestTest, PassingWorkerExitsZero)
{
    WorkerProcess proc(g_self_exe_path, "selftest.passes", {});
    ASSERT_TRUE(proc.valid());
    int exit_code = proc.wait_for_exit();
    EXPECT_EQ(exit_code, 0) << "Passing worker should exit 0.\n"
                            << "Stderr: " << proc.get_stderr();
}
