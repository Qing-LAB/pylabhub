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
