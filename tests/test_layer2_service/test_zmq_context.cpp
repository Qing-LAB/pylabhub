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

// ─────────────────────────────────────────────────────────────────────
// apply_socket_policy — audit S1 Phase A (2026-05-18).  Pins that the
// centralized helper in utils/zmq_socket_policy.hpp applies every
// option correctly per ZmqSocketRole.  Mutation localization: dropping
// or changing any single .set(...) in the helper triggers exactly one
// EXPECT_EQ failure in one of the three tests below.  See
// IMPLEMENTATION_GUIDANCE.md § "Role-side ZMQ socket policy" for the
// canonical contract.
// ─────────────────────────────────────────────────────────────────────
TEST_F(ZmqContextTest, ApplySocketPolicy_TcpConnect_SetsAll)
{
    auto w = SpawnWorker(
        "zmq_context.apply_socket_policy_tcp_connect_sets_all", {});
    ExpectWorkerOk(w);
}

TEST_F(ZmqContextTest, ApplySocketPolicy_TcpBind_Subset)
{
    auto w = SpawnWorker(
        "zmq_context.apply_socket_policy_tcp_bind_subset", {});
    ExpectWorkerOk(w);
}

TEST_F(ZmqContextTest, ApplySocketPolicy_InprocSignal_Minimal)
{
    auto w = SpawnWorker(
        "zmq_context.apply_socket_policy_inproc_signal_minimal", {});
    ExpectWorkerOk(w);
}

TEST_F(ZmqContextTest, MultiThread_GetContext_Safe)
{
    auto w = SpawnWorker("zmq_context.multithread_get_context_safe", {});
    ExpectWorkerOk(w);
}
