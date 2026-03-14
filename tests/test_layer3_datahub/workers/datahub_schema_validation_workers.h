// tests/test_layer3_datahub/workers/schema_validation_workers.h
#pragma once

namespace pylabhub::tests::worker::schema_validation
{

int consumer_connects_with_matching_schema();
int consumer_fails_to_connect_with_mismatched_schema();
int flexzone_mismatch_rejected();
int both_schemas_mismatch_rejected();
int consumer_mismatched_capacity_rejected();

} // namespace pylabhub::tests::worker::schema_validation
