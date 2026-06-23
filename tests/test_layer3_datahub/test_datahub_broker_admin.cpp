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

// ── #281 (2026-06-23) — REG_REQ wire-contract pins ───────────────────────
//
// Six tests pinning the broker's `data_transport` enforcement on REG_REQ
// (HEP-CORE-0036 §6.1 + HEP-CORE-0041 §5.1).  Before #281 there was NO
// wire-level test for this contract: missing/empty/bogus `data_transport`
// was silently mis-classified as SHM by the broker's default fallback,
// and the §5.1 endpoint-required check shipped under #268 had no
// regression pin either.  These tests close both gaps.

TEST_F(BrokerAdminTest, RegValidation_MissingDataTransport_Rejected)
{
    auto w = SpawnWorker("broker_admin.reg_validation_missing_data_transport");
    ExpectWorkerOk(w);
}

TEST_F(BrokerAdminTest, RegValidation_EmptyDataTransport_Rejected)
{
    auto w = SpawnWorker("broker_admin.reg_validation_empty_data_transport");
    ExpectWorkerOk(w);
}

TEST_F(BrokerAdminTest, RegValidation_BogusDataTransport_Rejected)
{
    auto w = SpawnWorker("broker_admin.reg_validation_bogus_data_transport");
    ExpectWorkerOk(w);
}

TEST_F(BrokerAdminTest, RegValidation_ShmMissingEndpoint_Rejected)
{
    auto w = SpawnWorker("broker_admin.reg_validation_shm_missing_endpoint");
    ExpectWorkerOk(w);
}

TEST_F(BrokerAdminTest, RegValidation_ShmSuccess)
{
    auto w = SpawnWorker("broker_admin.reg_validation_shm_success");
    ExpectWorkerOk(w);
}

TEST_F(BrokerAdminTest, RegValidation_ZmqSuccess)
{
    auto w = SpawnWorker("broker_admin.reg_validation_zmq_success");
    ExpectWorkerOk(w);
}
