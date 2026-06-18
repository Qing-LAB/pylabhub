/**
 * @file test_recovery_api.cpp
 * @brief Layer 3 tests for recovery_api, integrity_validator, slot_diagnostics, slot_recovery, heartbeat_manager.
 *
 * Spawns worker subprocesses that create DataBlocks and exercise the recovery/diagnostics APIs.
 */
#include "test_patterns.h"
#include "test_process_utils.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>

using namespace pylabhub::tests;
using namespace pylabhub::tests::helper;

class DatahubCApiRecoveryTest : public IsolatedProcessTest
{
};

TEST_F(DatahubCApiRecoveryTest, DatablockIsProcessAlive_ReturnsTrueForSelf)
{
    auto proc = SpawnWorker("recovery.datablock_is_process_alive", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubCApiRecoveryTest, IntegrityValidator_ValidateSucceedsOnCreatedDatablock)
{
    auto proc = SpawnWorker("recovery.integrity_validator_validate", {});
    ExpectWorkerOk(proc, {"INTEGRITY_CHECK: Finished"});
}

TEST_F(DatahubCApiRecoveryTest, SlotDiagnostics_RefreshSucceedsOnCreatedDatablock)
{
    auto proc = SpawnWorker("recovery.slot_diagnostics_refresh", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

TEST_F(DatahubCApiRecoveryTest, SlotRecovery_ReleaseZombieReadersOnEmptySlot)
{
    auto proc = SpawnWorker("recovery.slot_recovery_release_zombie_readers", {});
    ExpectWorkerOk(proc, {"datablock_release_zombie_readers"});
}

TEST_F(DatahubCApiRecoveryTest, HeartbeatManager_RegistersAndPulses)
{
    auto proc = SpawnWorker("recovery.heartbeat_manager_registers", {});
    // HEP-CORE-0041 substep 1f (#253) review B3 — the 1-arg consumer
    // DataBlock ctor's log line was unified with the 2-arg ctor's
    // wording when the consumer attach body was extracted into
    // `attach_consumer_state_`.  Old: "DataBlock '...' opened by
    // consumer.".  New: "DataBlock '...' attached (consumer) with
    // total size <N> bytes." (the uniform end-state form).
    ExpectWorkerOk(proc, {"attached (consumer)"});
}

TEST_F(DatahubCApiRecoveryTest, ProducerUpdateHeartbeat_ExplicitSucceeds)
{
    auto proc = SpawnWorker("recovery.producer_update_heartbeat_explicit", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubCApiRecoveryTest, ProducerHeartbeat_AndIsWriterAlive)
{
    auto proc = SpawnWorker("recovery.producer_heartbeat_and_is_writer_alive", {});
    ExpectWorkerOk(proc);
}
