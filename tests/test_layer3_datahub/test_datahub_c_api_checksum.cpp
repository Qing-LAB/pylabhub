/**
 * @file test_c_api_checksum.cpp
 * @brief C API checksum policy tests: Enforced auto-update/verify, corruption detection,
 *        and None policy bypass.
 *
 * Each test spawns an isolated worker process.
 * Tests verify checksum behavior at the DataBlockProducer/Consumer level (C++ wrappers).
 */
#include "test_patterns.h"
#include "test_process_utils.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>

using namespace pylabhub::tests;
using namespace pylabhub::tests::helper;

class DatahubCApiChecksumTest : public IsolatedProcessTest
{
};

// ─── ChecksumPolicy::Enforced ─────────────────────────────────────────────────

TEST_F(DatahubCApiChecksumTest, EnforcedRoundtripPasses)
{
    auto proc = SpawnWorker("c_api_checksum.enforced_roundtrip_passes", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

TEST_F(DatahubCApiChecksumTest, EnforcedCorruptionDetected)
{
    auto proc = SpawnWorker("c_api_checksum.enforced_corruption_detected", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

// ─── ChecksumPolicy::None ─────────────────────────────────────────────────────

TEST_F(DatahubCApiChecksumTest, NoneSkipsVerification)
{
    auto proc = SpawnWorker("c_api_checksum.none_skips_verification", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

// ─── ChecksumPolicy::Manual ──────────────────────────────────────────────────

TEST_F(DatahubCApiChecksumTest, ManualNoAutoChecksum)
{
    // Manual policy: DataBlock does NOT auto-checksum on write or auto-verify on read.
    // Corruption goes undetected at DataBlock level.
    auto proc = SpawnWorker("c_api_checksum.manual_no_auto_checksum", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

TEST_F(DatahubCApiChecksumTest, ManualExplicitChecksumRoundtrip)
{
    // Manual policy: caller explicitly computes and verifies checksum. End-to-end integrity.
    auto proc = SpawnWorker("c_api_checksum.manual_explicit_checksum_roundtrip", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

TEST_F(DatahubCApiChecksumTest, InvalidateChecksumZeroHashRejected)
{
    // invalidate_checksum_slot() zeros hash. verify_checksum_slot() detects and rejects.
    auto proc = SpawnWorker("c_api_checksum.invalidate_checksum_zero_hash_rejected", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}
