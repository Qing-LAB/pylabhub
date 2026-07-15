#pragma once
/**
 * @file broker_schema_workers.h
 * @brief Workers for broker named-schema protocol tests
 *        (HEP-CORE-0034 path B; Pattern 3).
 */

namespace pylabhub::tests::worker
{
namespace broker_schema
{

// schema_hash_stored_on_reg stays in L3 — the stored schema_hash is not
// exposed on any wire ACK, so verification needs the in-process channel
// snapshot (Round 3 RATIONALE).  schema_id_stored_on_reg +
// consumer_schema_id_* MIGRATED to tests/test_layer3_pattern4/
// test_pattern4_broker_schema.cpp (task #52 Round 2).
int schema_hash_stored_on_reg();

} // namespace broker_schema
} // namespace pylabhub::tests::worker
