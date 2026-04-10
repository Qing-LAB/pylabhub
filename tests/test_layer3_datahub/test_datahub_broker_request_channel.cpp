/**
 * @file test_datahub_broker_request_channel.cpp
 * @brief L3 tests for BrokerRequestChannel against a real BrokerService.
 *
 * Each test spawns a worker process that starts a broker and exercises
 * the BrokerRequestChannel protocol.
 */
#include "test_patterns.h"
#include "test_process_utils.h"
#include <gtest/gtest.h>

using namespace pylabhub::tests;

class BrokerRequestChannelTest : public IsolatedProcessTest
{
};

TEST_F(BrokerRequestChannelTest, ConnectAndHeartbeat)
{
    auto proc = SpawnWorker("broker_req.connect_and_heartbeat", {});
    ExpectWorkerOk(proc);
}

TEST_F(BrokerRequestChannelTest, RegisterAndDiscover)
{
    auto proc = SpawnWorker("broker_req.register_and_discover", {});
    ExpectWorkerOk(proc);
}

TEST_F(BrokerRequestChannelTest, RolePresence)
{
    auto proc = SpawnWorker("broker_req.role_presence", {});
    ExpectWorkerOk(proc);
}

TEST_F(BrokerRequestChannelTest, NotificationDispatch)
{
    auto proc = SpawnWorker("broker_req.notification_dispatch", {});
    ExpectWorkerOk(proc);
}
