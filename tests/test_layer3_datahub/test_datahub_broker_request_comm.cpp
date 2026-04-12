/**
 * @file test_datahub_broker_request_comm.cpp
 * @brief L3 tests for BrokerRequestComm against a real BrokerService.
 *
 * Each test spawns a worker process that starts a broker and exercises
 * the BrokerRequestComm protocol.
 */
#include "test_patterns.h"
#include "test_process_utils.h"
#include <gtest/gtest.h>

using namespace pylabhub::tests;

class BrokerRequestCommTest : public IsolatedProcessTest
{
};

TEST_F(BrokerRequestCommTest, ConnectAndHeartbeat)
{
    auto proc = SpawnWorker("broker_req.connect_and_heartbeat", {});
    ExpectWorkerOk(proc);
}

TEST_F(BrokerRequestCommTest, RegisterAndDiscover)
{
    auto proc = SpawnWorker("broker_req.register_and_discover", {});
    ExpectWorkerOk(proc);
}

TEST_F(BrokerRequestCommTest, RolePresence)
{
    auto proc = SpawnWorker("broker_req.role_presence", {});
    ExpectWorkerOk(proc);
}

TEST_F(BrokerRequestCommTest, NotificationDispatch)
{
    auto proc = SpawnWorker("broker_req.notification_dispatch", {});
    ExpectWorkerOk(proc);
}
