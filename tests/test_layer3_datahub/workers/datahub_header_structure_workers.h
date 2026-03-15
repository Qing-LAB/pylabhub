// tests/test_layer3_datahub/workers/datahub_header_structure_workers.h
#pragma once

namespace pylabhub::tests::worker::header_structure
{

int schema_hashes_populated_with_template_api();
int schema_hashes_zero_without_schema();
int different_types_produce_different_hashes();

} // namespace pylabhub::tests::worker::header_structure
