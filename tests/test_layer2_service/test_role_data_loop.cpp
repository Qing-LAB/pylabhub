/**
 * @file test_role_data_loop.cpp
 * @brief Pattern 3 driver: run_data_loop + ThreadManager unit tests.
 *
 * Each TEST_F spawns a worker subprocess. The previous in-process layout
 * fabricated RoleAPIBase without a guard, half-registering the dynamic
 * ThreadManager module — exactly the failure mode HEP-CORE-0001 §
 * "Testing implications" warns against.
 */
#include "test_patterns.h"

#include <gtest/gtest.h>

using pylabhub::tests::IsolatedProcessTest;

class RunDataLoopTest : public IsolatedProcessTest
{
};

TEST_F(RunDataLoopTest, ShutdownStopsLoop)
{
    auto w = SpawnWorker("role_data_loop.shutdown_stops_loop", {});
    ExpectWorkerOk(w);
}

TEST_F(RunDataLoopTest, InvokeReturnsFalseStopsLoop)
{
    auto w = SpawnWorker("role_data_loop.invoke_returns_false_stops_loop", {});
    ExpectWorkerOk(w);
}

TEST_F(RunDataLoopTest, MetricsIncrement)
{
    auto w = SpawnWorker("role_data_loop.metrics_increment", {});
    ExpectWorkerOk(w);
}

TEST_F(RunDataLoopTest, NoDataSkipsDeadlineWait)
{
    auto w = SpawnWorker("role_data_loop.no_data_skips_deadline_wait", {});
    ExpectWorkerOk(w);
}

TEST_F(RunDataLoopTest, OverrunDetected)
{
    auto w = SpawnWorker("role_data_loop.overrun_detected", {});
    ExpectWorkerOk(w);
}

class ThreadManagerTest : public IsolatedProcessTest
{
};

TEST_F(ThreadManagerTest, SpawnAndJoin)
{
    auto w = SpawnWorker("role_data_loop.thread_manager_spawn_and_join", {});
    ExpectWorkerOk(w);
}

TEST_F(ThreadManagerTest, MultipleThreads)
{
    auto w = SpawnWorker("role_data_loop.thread_manager_multiple_threads", {});
    ExpectWorkerOk(w);
}

// Test was previously named `JoinInReverseOrder` but the worker only
// asserts `order.size() == 2` — i.e. that drain() waits for ALL
// spawned threads to complete (including a slow one).  It does NOT
// observe the order in which drain() invokes `join()` on the slots.
// Verifying actual reverse-spawn-order join from outside the
// ThreadManager would require exposing internal join-sequence state
// (e.g. a per-slot "joined_at" timestamp), which is not currently
// part of the public surface.  Renamed to match what's actually
// tested; reverse-order verification is a separate follow-up.
TEST_F(ThreadManagerTest, DrainJoinsAllThreads)
{
    auto w = SpawnWorker("role_data_loop.thread_manager_join_in_reverse_order",
                         {});
    ExpectWorkerOk(w);
}
