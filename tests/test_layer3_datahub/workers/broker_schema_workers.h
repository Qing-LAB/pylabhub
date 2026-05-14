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

int schema_hash_stored_on_reg();
int schema_id_stored_on_reg();
int consumer_schema_id_match_succeeds();
int consumer_schema_id_mismatch_fails();
int consumer_schema_id_empty_producer_fails();

} // namespace broker_schema
} // namespace pylabhub::tests::worker
