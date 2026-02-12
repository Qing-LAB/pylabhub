/**
 * @file test_phase_a_protocol.cpp
 * @brief Phase A – Protocol/API correctness (no broker).
 *
 * Flexible zone empty span when no zones; checksum false when zone undefined;
 * config/agreement (consumer with/without expected_config).
 */
#include "test_patterns.h"
#include "test_process_utils.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>

using namespace pylabhub::tests;
using namespace pylabhub::tests::helper;

class PhaseAProtocolTest : public IsolatedProcessTest
{
};

TEST_F(PhaseAProtocolTest, FlexibleZoneSpanEmptyWhenNoZones)
{
    auto proc = SpawnWorker("phase_a.flexible_zone_empty", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

TEST_F(PhaseAProtocolTest, FlexibleZoneSpanNonEmptyWhenZonesDefined)
{
    auto proc = SpawnWorker("phase_a.flexible_zone_non_empty", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

TEST_F(PhaseAProtocolTest, ChecksumFlexibleZoneFalseWhenNoZones)
{
    auto proc = SpawnWorker("phase_a.checksum_false_no_zones", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

TEST_F(PhaseAProtocolTest, ChecksumFlexibleZoneTrueWhenValid)
{
    auto proc = SpawnWorker("phase_a.checksum_true_valid", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

TEST_F(PhaseAProtocolTest, ConsumerWithoutExpectedConfigGetsEmptyZones)
{
    auto proc = SpawnWorker("phase_a.consumer_no_config", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

TEST_F(PhaseAProtocolTest, ConsumerWithExpectedConfigGetsZones)
{
    auto proc = SpawnWorker("phase_a.consumer_with_config", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

// Structured flexible zone: producer writes typed struct, consumer reads and verifies
TEST_F(PhaseAProtocolTest, StructuredFlexZoneDataPasses)
{
    auto proc = SpawnWorker("phase_a.structured_flex_zone_data_passes", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

// Error modes
TEST_F(PhaseAProtocolTest, ErrorFlexZoneTypeTooLargeThrows)
{
    auto proc = SpawnWorker("phase_a.error_flex_zone_type_too_large_throws", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

TEST_F(PhaseAProtocolTest, ErrorChecksumFlexZoneFailsAfterTampering)
{
    auto proc = SpawnWorker("phase_a.error_checksum_flex_zone_fails_after_tampering", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}
