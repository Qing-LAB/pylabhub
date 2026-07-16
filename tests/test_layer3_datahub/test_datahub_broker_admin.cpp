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

// ListChannels_* / Snapshot_* MIGRATED to Pattern 4 (task #52 Round 3 —
// via CHANNEL_LIST_REQ; the broker's producer_pid is now on that ACK).

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

// RegValidation_* MIGRATED to
// tests/test_layer3_pattern4/test_pattern4_broker_admin.cpp — error paths
// in Round 2; *_Success variants in Round 3 (verified via DISC_REQ
// data_transport instead of the in-process channel snapshot).
