/**
 * @file test_datahub_channel_group.cpp
 * @brief L3 tests for band pub/sub messaging (HEP-CORE-0030).
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
    // Legacy worker bypass run_gtest_worker — opt out of [WORKER_*]
    // milestone check. Convert to Pattern 3 in the L3 sweep.
    ExpectLegacyWorkerOk(proc);
}

TEST_F(ChannelGroupTest, MessageFanout)
{
    auto proc = SpawnWorker("channel_group.msg_fanout", {});
    // Legacy worker bypass run_gtest_worker — opt out of [WORKER_*]
    // milestone check. Convert to Pattern 3 in the L3 sweep.
    ExpectLegacyWorkerOk(proc);
}

TEST_F(ChannelGroupTest, JoinNotification)
{
    auto proc = SpawnWorker("channel_group.join_notify", {});
    // Legacy worker bypass run_gtest_worker — opt out of [WORKER_*]
    // milestone check. Convert to Pattern 3 in the L3 sweep.
    ExpectLegacyWorkerOk(proc);
}

TEST_F(ChannelGroupTest, RoleAPIBaseChannelIntegration)
{
    auto proc = SpawnWorker("channel_group.roleapi_channel", {});
    // Legacy worker bypass run_gtest_worker — opt out of [WORKER_*]
    // milestone check. Convert to Pattern 3 in the L3 sweep.
    ExpectLegacyWorkerOk(proc);
}

TEST_F(ChannelGroupTest, LeaveNotification)
{
    auto proc = SpawnWorker("channel_group.leave_notify", {});
    // Legacy worker bypass run_gtest_worker — opt out of [WORKER_*]
    // milestone check. Convert to Pattern 3 in the L3 sweep.
    ExpectLegacyWorkerOk(proc);
}

TEST_F(ChannelGroupTest, SenderExcludedFromOwnMessage)
{
    auto proc = SpawnWorker("channel_group.self_excluded", {});
    // Legacy worker bypass run_gtest_worker — opt out of [WORKER_*]
    // milestone check. Convert to Pattern 3 in the L3 sweep.
    ExpectLegacyWorkerOk(proc);
}

TEST_F(ChannelGroupTest, MultipleChannelsIndependent)
{
    auto proc = SpawnWorker("channel_group.multi_channel", {});
    // Legacy worker bypass run_gtest_worker — opt out of [WORKER_*]
    // milestone check. Convert to Pattern 3 in the L3 sweep.
    ExpectLegacyWorkerOk(proc);
}
