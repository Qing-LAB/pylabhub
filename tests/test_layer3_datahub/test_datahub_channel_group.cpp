/**
 * @file test_datahub_channel_group.cpp
 * @brief L3 tests for channel pub/sub messaging (HEP-CORE-0030).
 */
#include "test_patterns.h"
#include "test_process_utils.h"
#include <gtest/gtest.h>

using namespace pylabhub::tests;

class ChannelGroupTest : public IsolatedProcessTest
{
};

TEST_F(ChannelGroupTest, JoinAndLeave)
{
    auto proc = SpawnWorker("channel_group.join_leave", {});
    ExpectWorkerOk(proc);
}

TEST_F(ChannelGroupTest, MessageFanout)
{
    auto proc = SpawnWorker("channel_group.msg_fanout", {});
    ExpectWorkerOk(proc);
}

TEST_F(ChannelGroupTest, JoinNotification)
{
    auto proc = SpawnWorker("channel_group.join_notify", {});
    ExpectWorkerOk(proc);
}

TEST_F(ChannelGroupTest, RoleAPIBaseChannelIntegration)
{
    auto proc = SpawnWorker("channel_group.roleapi_channel", {});
    ExpectWorkerOk(proc);
}
