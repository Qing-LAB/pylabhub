/**
 * @file test_datahub_broker_admin.cpp
 * @brief Pattern 3 driver — BrokerService admin API tests
 *        (list_channels_json_str / query_channel_snapshot /
 *        request_close_channel).
 *
 * Migrated 2026-05-13 from the in-process `SetUpTestSuite`-owned
 * `LifecycleGuard` antipattern.  Worker bodies live in
 * `workers/broker_admin_workers.cpp`.
 */

#include "test_patterns.h"
#include <gtest/gtest.h>

using pylabhub::tests::IsolatedProcessTest;

class BrokerAdminTest : public IsolatedProcessTest
{
};

TEST_F(BrokerAdminTest, ListChannels_Empty)
{
    auto w = SpawnWorker("broker_admin.list_channels_empty");
    ExpectWorkerOk(w);
}

TEST_F(BrokerAdminTest, ListChannels_OneChannel)
{
    auto w = SpawnWorker("broker_admin.list_channels_one_channel");
    ExpectWorkerOk(w);
}

TEST_F(BrokerAdminTest, ListChannels_FieldPresence)
{
    auto w = SpawnWorker("broker_admin.list_channels_field_presence");
    ExpectWorkerOk(w);
}

TEST_F(BrokerAdminTest, Snapshot_Empty)
{
    auto w = SpawnWorker("broker_admin.snapshot_empty");
    ExpectWorkerOk(w);
}

TEST_F(BrokerAdminTest, Snapshot_OneChannel)
{
    auto w = SpawnWorker("broker_admin.snapshot_one_channel");
    ExpectWorkerOk(w);
}

TEST_F(BrokerAdminTest, Snapshot_AfterConsumer)
{
    auto w = SpawnWorker("broker_admin.snapshot_after_consumer");
    ExpectWorkerOk(w);
}

TEST_F(BrokerAdminTest, CloseChannel_Existing)
{
    auto w = SpawnWorker("broker_admin.close_channel_existing");
    ExpectWorkerOk(w);
}

TEST_F(BrokerAdminTest, CloseChannel_NonExistent)
{
    auto w = SpawnWorker("broker_admin.close_channel_non_existent");
    ExpectWorkerOk(w);
}
