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

class SchemaValidationTest : public IsolatedProcessTest
{
};

TEST_F(SchemaValidationTest, ConsumerConnectsWithMatchingSchema)
{
    auto proc = SpawnWorker("schema_validation.consumer_connects_matching", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}

TEST_F(SchemaValidationTest, ConsumerFailsToConnectWithMismatchedSchema)
{
    auto proc = SpawnWorker("schema_validation.consumer_fails_mismatched", {});
    ExpectWorkerOk(proc, {"DataBlock"});
}
