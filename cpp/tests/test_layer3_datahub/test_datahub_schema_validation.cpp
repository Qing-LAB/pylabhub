/**
 * @file test_schema_validation.cpp
 * @brief Layer 3 DataHub schema validation tests (producer/consumer schema match).
 *
 * Spawns worker subprocesses that create DataBlocks and verify schema validation
 * on consumer attach.
 */
#include "test_patterns.h"
#include "test_process_utils.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>

using namespace pylabhub::tests;
using namespace pylabhub::tests::helper;

class DatahubSchemaValidationTest : public IsolatedProcessTest
{
};

TEST_F(DatahubSchemaValidationTest, ConsumerConnectsWithMatchingSchema)
{
    auto proc = SpawnWorker("schema_validation.consumer_connects_matching", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

TEST_F(DatahubSchemaValidationTest, ConsumerFailsToConnectWithMismatchedSchema)
{
    auto proc = SpawnWorker("schema_validation.consumer_fails_mismatched", {});
    // Consumer open fails with LOGGER_ERROR on schema hash mismatch.
    ExpectWorkerOk(proc, {}, {"DataBlock schema hash mismatch"});
}

TEST_F(DatahubSchemaValidationTest, FlexzoneMismatchRejected)
{
    auto proc = SpawnWorker("schema_validation.flexzone_mismatch_rejected", {});
    // FlexZone schema mismatch emits LOGGER_ERROR.
    ExpectWorkerOk(proc, {}, {"FlexZone schema hash mismatch"});
}

TEST_F(DatahubSchemaValidationTest, BothSchemasMismatchRejected)
{
    auto proc = SpawnWorker("schema_validation.both_schemas_mismatch_rejected", {});
    // FlexZone schema mismatch is checked first and emits LOGGER_ERROR.
    ExpectWorkerOk(proc, {}, {"FlexZone schema hash mismatch"});
}

TEST_F(DatahubSchemaValidationTest, ConsumerMismatchedCapacityRejected)
{
    auto proc = SpawnWorker("schema_validation.consumer_mismatched_capacity_rejected", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}
