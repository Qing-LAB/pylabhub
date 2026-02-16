/**
 * @file test_policy_enforcement.cpp
 * @brief DataHub policy enforcement tests — checksum, heartbeat, sync backpressure,
 *        and auto-heartbeat in iterator.
 *
 * Each test spawns an isolated worker process.
 * All assertions run inside the worker's run_gtest_worker() scope.
 */
#include "test_patterns.h"
#include "test_process_utils.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>

using namespace pylabhub::tests;
using namespace pylabhub::tests::helper;

class PolicyEnforcementTest : public IsolatedProcessTest
{
};

// ─── Checksum: Enforced ───────────────────────────────────────────────────────

TEST_F(PolicyEnforcementTest, ChecksumEnforcedWriteReadRoundtrip)
{
    auto proc = SpawnWorker("policy_enforcement.checksum_enforced_write_read_roundtrip", {});
    ExpectWorkerOk(proc, {"[policy_enforcement]"});
}

TEST_F(PolicyEnforcementTest, ChecksumEnforcedFlexzoneOnlyWrite)
{
    auto proc = SpawnWorker("policy_enforcement.checksum_enforced_flexzone_only_write", {});
    ExpectWorkerOk(proc, {"[policy_enforcement]"});
}

TEST_F(PolicyEnforcementTest, ChecksumEnforcedVerifyDetectsCorruption)
{
    auto proc = SpawnWorker("policy_enforcement.checksum_enforced_verify_detects_corruption", {});
    ExpectWorkerOk(proc, {"[policy_enforcement]"});
}

// ─── Checksum: None ───────────────────────────────────────────────────────────

TEST_F(PolicyEnforcementTest, ChecksumNoneSkipsUpdateVerify)
{
    auto proc = SpawnWorker("policy_enforcement.checksum_none_skips_update_verify", {});
    ExpectWorkerOk(proc, {"[policy_enforcement]"});
}

// ─── Checksum: Manual ─────────────────────────────────────────────────────────

TEST_F(PolicyEnforcementTest, ChecksumManualRequiresExplicitCall)
{
    auto proc = SpawnWorker("policy_enforcement.checksum_manual_requires_explicit_call", {});
    ExpectWorkerOk(proc, {"[policy_enforcement]"});
}

// ─── Heartbeat: Auto-register / Auto-unregister ───────────────────────────────

TEST_F(PolicyEnforcementTest, ConsumerAutoRegistersHeartbeatOnConstruction)
{
    auto proc = SpawnWorker("policy_enforcement.consumer_auto_registers_heartbeat_on_construction", {});
    ExpectWorkerOk(proc, {"[policy_enforcement]"});
}

TEST_F(PolicyEnforcementTest, ConsumerAutoUnregistersHeartbeatOnDestroy)
{
    auto proc = SpawnWorker("policy_enforcement.consumer_auto_unregisters_heartbeat_on_destroy", {});
    ExpectWorkerOk(proc, {"[policy_enforcement]"});
}

TEST_F(PolicyEnforcementTest, AllPolicyConsumersHaveHeartbeat)
{
    auto proc = SpawnWorker("policy_enforcement.all_policy_consumers_have_heartbeat", {});
    ExpectWorkerOk(proc, {"[policy_enforcement]"});
}

// ─── Sync_reader: Backpressure ────────────────────────────────────────────────

TEST_F(PolicyEnforcementTest, SyncReaderProducerRespectsConsumerPosition)
{
    auto proc = SpawnWorker("policy_enforcement.sync_reader_producer_respects_consumer_position", {});
    ExpectWorkerOk(proc, {"[policy_enforcement]"});
}

// ─── Auto-heartbeat in iterator ──────────────────────────────────────────────

TEST_F(PolicyEnforcementTest, ProducerOperatorIncrementUpdatesHeartbeat)
{
    auto proc = SpawnWorker("policy_enforcement.producer_operator_increment_updates_heartbeat", {});
    ExpectWorkerOk(proc, {"[policy_enforcement]"});
}

TEST_F(PolicyEnforcementTest, ConsumerOperatorIncrementUpdatesHeartbeat)
{
    auto proc = SpawnWorker("policy_enforcement.consumer_operator_increment_updates_heartbeat", {});
    ExpectWorkerOk(proc, {"[policy_enforcement]"});
}
