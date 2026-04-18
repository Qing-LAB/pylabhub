/**
 * @file test_zmq_context.cpp
 * @brief Pattern 3 driver: ZmqContext lifecycle singleton tests.
 *
 * Each TEST_F spawns a worker subprocess (IsolatedProcessTest) that owns
 * a LifecycleGuard for Logger + hub::ZMQContext. The framework's _exit()
 * inside run_gtest_worker bypasses libzmq's static destructors, which is
 * the original motivation for the test contract — see
 * docs/HEP/HEP-CORE-0001-hybrid-lifecycle-model.md § "Testing implications".
 */
#include "test_patterns.h"
#include <gtest/gtest.h>

using pylabhub::tests::IsolatedProcessTest;

class ZmqContextTest : public IsolatedProcessTest
{
};

TEST_F(ZmqContextTest, GetContext_ReturnsValid)
{
    auto w = SpawnWorker("zmq_context.get_context_returns_valid", {});
    ExpectWorkerOk(w);
}

TEST_F(ZmqContextTest, GetContext_SameInstance)
{
    auto w = SpawnWorker("zmq_context.get_context_same_instance", {});
    ExpectWorkerOk(w);
}

TEST_F(ZmqContextTest, CreateSocket_Works)
{
    auto w = SpawnWorker("zmq_context.create_socket_works", {});
    ExpectWorkerOk(w);
}

TEST_F(ZmqContextTest, MultiThread_GetContext_Safe)
{
    auto w = SpawnWorker("zmq_context.multithread_get_context_safe", {});
    ExpectWorkerOk(w);
}
