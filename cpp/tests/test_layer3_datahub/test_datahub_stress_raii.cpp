/**
 * @file test_datahub_stress_raii.cpp
 * @brief Multi-process stress tests for the DataHub RAII abstraction layer.
 *
 * **Scope — RAII layer (C++ Phase 3).** Tests exercise `with_transaction`,
 * `SlotIterator`, full-capacity 4096-byte slot payloads, enforced BLAKE2b
 * checksums, and random read/write timing across separate processes.
 *
 * **Test 1: MultiProcessFullCapacityStress**
 *   - 1 producer + 2 concurrent consumers, Latest_only policy.
 *   - 500 × 4KB slots → ≈15 full ring-buffer wraparounds (ring capacity = 32).
 *   - Each 4KB slot is filled with a deterministic byte pattern; consumers verify
 *     every byte (byte-level) and an independent XOR-fold checksum (app_checksum),
 *     on top of the BLAKE2b that release_consume_slot() checks automatically.
 *   - Random inter-operation delays (0–5 ms write, 0–10 ms read) ensure the test
 *     runs under realistic scheduling jitter rather than tight-loop conditions.
 *
 * **Test 2: SingleReaderBackpressure**
 *   - 1 producer + 1 consumer, Single_reader policy; ring capacity = 8.
 *   - Consumer adds 0–20 ms delays to force producer to block when ring is full.
 *   - All 100 slots must be delivered in exact sequence order (no loss guaranteed
 *     by Single_reader). Every slot's payload and checksum is verified.
 */
#include "test_patterns.h"
#include "test_process_utils.h"
#include "shared_test_helpers.h"
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <unistd.h>

using namespace pylabhub::tests;
namespace fs = std::filesystem;

class DatahubStressRaiiTest : public IsolatedProcessTest
{
};

// ─── Test 1: Full-capacity racing ring buffer ─────────────────────────────────

TEST_F(DatahubStressRaiiTest, MultiProcessFullCapacityStress)
{
    // Channel name includes PID so concurrent test runs don't conflict.
    const std::string channel =
        "stress_raii_full_" + std::to_string(static_cast<unsigned long>(getpid()));

    // Orchestrator spawns producer + 2 consumers; coordinates via DataBlock ready signal.
    auto proc = SpawnWorker("stress_raii.multi_process_stress_orchestrator", {channel});
    ExpectWorkerOk(proc);
}

// ─── Test 2: Single-reader back-pressure ─────────────────────────────────────

TEST_F(DatahubStressRaiiTest, SingleReaderBackpressure)
{
    const std::string channel =
        "stress_raii_bp_" + std::to_string(static_cast<unsigned long>(getpid()));

    auto proc = SpawnWorker("stress_raii.backpressure_orchestrator", {channel});
    ExpectWorkerOk(proc);
}
