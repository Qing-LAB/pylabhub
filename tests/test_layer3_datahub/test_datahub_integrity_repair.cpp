/**
 * @file test_datahub_integrity_repair.cpp
 * @brief Integrity validation tests: corruption detection and repair paths.
 *
 * **Scope — Facility layer.**  Tests exercise `datablock_validate_integrity` with
 * controlled corruption injected via DiagnosticHandle:
 *   - Baseline: fresh ChecksumPolicy::Enforced block passes validation.
 *   - Layout checksum mismatch: detected; not repairable (RECOVERY_FAILED with repair=true).
 *   - Magic number corruption: detected as RECOVERY_FAILED.
 *
 * Slot-checksum repair is deferred (the current repair path uses create_datablock_producer_impl
 * which reinitialises the header when called on an existing segment, making in-place repair
 * not testable at this layer).  See docs/todo/TESTING_TODO.md § Medium Priority.
 *
 * @see docs/todo/TESTING_TODO.md § "Coverage Gaps / High Priority: corrupted header/layout repair"
 */
#include "test_patterns.h"
#include "test_process_utils.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>

using namespace pylabhub::tests;
using namespace pylabhub::tests::helper;

class DatahubIntegrityRepairTest : public IsolatedProcessTest
{
};

// ─── Baseline: fresh checksum block ───────────────────────────────────────────

TEST_F(DatahubIntegrityRepairTest, FreshChecksumBlockPasses)
{
    auto proc = SpawnWorker("integrity_repair.validate_integrity_fresh_checksum_block_passes", {});
    // validate_integrity logs INFO; WARN was only emitted when DataBlock factory called
    // register_producer (old coupling, now removed).
    ExpectWorkerOk(proc, {"INFO"});
}

// ─── Layout checksum corruption ───────────────────────────────────────────────

TEST_F(DatahubIntegrityRepairTest, DetectsLayoutChecksumMismatch)
{
    auto proc = SpawnWorker("integrity_repair.validate_integrity_detects_layout_checksum_mismatch", {});
    // Corruption detection emits LOGGER_ERROR for layout checksum mismatch,
    // then a second LOGGER_ERROR when consumer creation for slot verification also fails.
    ExpectWorkerOk(proc, {}, {"INTEGRITY_CHECK: Layout checksum mismatch",
                              "INTEGRITY_CHECK: Could not create a consumer"});
}

// ─── Magic number corruption ──────────────────────────────────────────────────

TEST_F(DatahubIntegrityRepairTest, DetectsMagicNumberCorruption)
{
    auto proc = SpawnWorker("integrity_repair.validate_integrity_detects_magic_number_corruption", {});
    // Magic number corruption prevents open; recovery API logs LOGGER_ERROR "Failed to open".
    ExpectWorkerOk(proc, {}, {"recovery: Failed to open"});
}
