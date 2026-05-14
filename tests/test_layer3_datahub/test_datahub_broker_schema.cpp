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

TEST_F(BrokerSchemaTest, SchemaId_StoredOnReg)
{
    auto w = SpawnWorker("broker_schema.schema_id_stored_on_reg");
    ExpectWorkerOk(w);
}

TEST_F(BrokerSchemaTest, ConsumerSchemaId_Match_Succeeds)
{
    auto w = SpawnWorker("broker_schema.consumer_schema_id_match_succeeds");
    ExpectWorkerOk(w);
}

TEST_F(BrokerSchemaTest, ConsumerSchemaId_Mismatch_Fails)
{
    auto w = SpawnWorker("broker_schema.consumer_schema_id_mismatch_fails");
    ExpectWorkerOk(w);
}

TEST_F(BrokerSchemaTest, ConsumerSchemaId_EmptyProducer_Fails)
{
    auto w = SpawnWorker("broker_schema.consumer_schema_id_empty_producer_fails");
    ExpectWorkerOk(w);
}
