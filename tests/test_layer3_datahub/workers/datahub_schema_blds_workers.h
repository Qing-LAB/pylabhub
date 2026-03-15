// tests/test_layer3_datahub/schema_blds_workers.h
#pragma once
/**
 * @file schema_blds_workers.h
 * @brief Worker function declarations for schema_blds isolated-process tests.
 *
 * These workers initialize the CryptoUtils lifecycle module and run
 * SchemaInfo/generate_schema_info assertions that require BLAKE2b hashing.
 */

namespace pylabhub::tests::worker::schema_blds
{

int schema_info_sets_name_version_size();
int schema_info_blds_format();
int schema_info_hash_is_deterministic();
int schema_info_different_struct_different_hash();
int schema_info_matches();
int schema_info_matches_hash();
int validate_schema_match_same_does_not_throw();
int validate_schema_match_different_throws();
int validate_schema_hash_matching_does_not_throw();
int validate_schema_hash_mismatch_throws();

} // namespace pylabhub::tests::worker::schema_blds
