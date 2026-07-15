/**
 * @file test_datahub_broker_schema.cpp
 * @brief Pattern 3 driver — broker schema metadata tests
 *        (HEP-CORE-0016 Phase 3 / HEP-CORE-0034 §10).
 *
 * Migrated 2026-05-13 from the in-process `SetUpTestSuite`-owned
 * `LifecycleGuard` antipattern.  Worker bodies live in
 * `workers/broker_schema_workers.cpp`.
 */

#include "test_patterns.h"
#include <gtest/gtest.h>

using pylabhub::tests::IsolatedProcessTest;

class BrokerSchemaTest : public IsolatedProcessTest
{
};

TEST_F(BrokerSchemaTest, SchemaHash_StoredOnReg)
{
    auto w = SpawnWorker("broker_schema.schema_hash_stored_on_reg");
    ExpectWorkerOk(w);
}

// SchemaId_StoredOnReg + ConsumerSchemaId_{Match,Mismatch,EmptyProducer}
// MIGRATED to tests/test_layer3_pattern4/test_pattern4_broker_schema.cpp
// (task #52 Round 2 — HubHostBrokerHandle antipattern sweep).
// SchemaHash_StoredOnReg stays: stored schema_hash has no wire ACK.
