/**
 * @file test_datahub_recovery_scenarios.cpp
 * @brief Recovery scenario tests: zombie detection, force-reset, dead consumer cleanup.
 *
 * **Scope — Facility layer only.**  These tests verify that each recovery API function
 * (datablock_release_zombie_writer, datablock_release_zombie_readers,
 * datablock_force_reset_slot, datablock_cleanup_dead_consumers) behaves correctly
 * when the relevant broken state is injected directly via DiagnosticHandle.
 *
 * The **full broker-integrated recovery flow** (broker detects dead heartbeat → triggers
 * recovery → notifies consumers) requires the broker protocol and is tracked in
 * docs/todo/TESTING_TODO.md § "Phase C: Integration".
 *
 * @see docs/todo/TESTING_TODO.md § "Phase C: Cross-process recovery scenarios"
 */
#include "test_patterns.h"
#include "test_process_utils.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>

using namespace pylabhub::tests;
using namespace pylabhub::tests::helper;

class DatahubRecoveryScenariosTest : public IsolatedProcessTest
{
};

// ─── Zombie writer ────────────────────────────────────────────────────────────

TEST_F(DatahubRecoveryScenariosTest, ZombieWriterDetectedAndReleased)
{
    auto proc = SpawnWorker("recovery_scenarios.zombie_writer_detected_and_released", {});
    ExpectWorkerOk(proc, {"RECOVERY"});
}

// ─── Zombie readers ───────────────────────────────────────────────────────────

TEST_F(DatahubRecoveryScenariosTest, ZombieReadersForceCleared)
{
    auto proc = SpawnWorker("recovery_scenarios.zombie_readers_force_cleared", {});
    ExpectWorkerOk(proc, {"RECOVERY"});
}

// ─── Force reset on dead writer slot ─────────────────────────────────────────

TEST_F(DatahubRecoveryScenariosTest, ForceResetSlotOnDeadWriter)
{
    auto proc = SpawnWorker("recovery_scenarios.force_reset_slot_on_dead_writer", {});
    ExpectWorkerOk(proc, {"RECOVERY"});
}

// ─── Dead consumer heartbeat cleanup ─────────────────────────────────────────

TEST_F(DatahubRecoveryScenariosTest, DeadConsumerCleanup)
{
    auto proc = SpawnWorker("recovery_scenarios.dead_consumer_cleanup", {});
    ExpectWorkerOk(proc, {"RECOVERY"});
}

// ─── is_process_alive sentinel ────────────────────────────────────────────────

TEST_F(DatahubRecoveryScenariosTest, IsProcessAliveFalseForNonexistent)
{
    auto proc = SpawnWorker("recovery_scenarios.is_process_alive_false_for_nonexistent", {});
    ExpectWorkerOk(proc);
}

// ─── Safety guard: refuses force-reset when writer is alive ──────────────────

TEST_F(DatahubRecoveryScenariosTest, ForceResetUnsafeWhenWriterAlive)
{
    auto proc = SpawnWorker("recovery_scenarios.force_reset_unsafe_when_writer_alive", {});
    // Recovery API logs LOGGER_ERROR when refusing force-reset (writer is alive).
    ExpectWorkerOk(proc, {}, {"write lock held by ALIVE"});
}
