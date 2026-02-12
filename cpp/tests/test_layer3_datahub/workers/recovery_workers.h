// tests/test_layer3_datahub/workers/recovery_workers.h
#pragma once
/**
 * @file recovery_workers.h
 * @brief Worker functions for recovery_api, integrity_validator, slot_diagnostics, slot_recovery, heartbeat_manager.
 *
 * These workers run with full lifecycle (CryptoUtils, MessageHub) and create
 * DataBlocks to exercise the recovery and diagnostics APIs.
 */

namespace pylabhub::tests::worker::recovery
{

int datablock_is_process_alive_returns_true_for_self();
int integrity_validator_validate_succeeds_on_created_datablock();
int slot_diagnostics_refresh_succeeds_on_created_datablock();
int slot_recovery_release_zombie_readers_on_empty_slot();
int heartbeat_manager_registers_and_pulses();

} // namespace pylabhub::tests::worker::recovery
