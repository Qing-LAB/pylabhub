#pragma once
/**
 * @file role_config_workers.h
 * @brief Workers for the RoleConfig unit-test suite (Pattern 3).
 *
 * Each TEST_F in test_role_config.cpp runs as a worker subprocess. Each
 * worker is given a unique scratch dir by the parent (argv[2]); it writes
 * a JSON config inside that dir, calls RoleConfig::load*, and asserts on
 * the parsed fields. The worker owns its LifecycleGuard via
 * run_gtest_worker (Logger + FileLock + JsonConfig).
 */

#include <string>

namespace pylabhub::tests::worker
{
namespace role_config
{

// ── Producer ────────────────────────────────────────────────────────────────
int load_producer_identity(const std::string &dir);
int load_producer_timing(const std::string &dir);
int load_producer_script(const std::string &dir);
int load_producer_out_channel(const std::string &dir);
int load_producer_out_transport(const std::string &dir);
int load_producer_out_validation(const std::string &dir);
int load_producer_validation_stop_on_script_error(const std::string &dir);

// ── Consumer ────────────────────────────────────────────────────────────────
int load_consumer_identity(const std::string &dir);
int load_consumer_in_channel(const std::string &dir);
int load_consumer_in_validation(const std::string &dir);
int load_consumer_default_timing(const std::string &dir);

// ── Processor ───────────────────────────────────────────────────────────────
int load_processor_dual_channels(const std::string &dir);
int load_processor_dual_transport(const std::string &dir);
int load_processor_dual_validation(const std::string &dir);

// ── Role-specific data ──────────────────────────────────────────────────────
int role_data_producer_fields(const std::string &dir);
int role_data_no_parser(const std::string &dir);
int role_data_wrong_type_cast_throws(const std::string &dir);

// ── Auth ────────────────────────────────────────────────────────────────────
int auth_default_empty(const std::string &dir);
int load_keypair_no_keyfile_returns_false(const std::string &dir);

// ── Raw JSON / metadata ─────────────────────────────────────────────────────
int raw_json(const std::string &dir);
int role_tag(const std::string &dir);
int base_dir(const std::string &dir);
int load_from_directory(const std::string &dir);

// ── Validation errors ───────────────────────────────────────────────────────
int file_not_found_throws();
int invalid_script_type_throws(const std::string &dir);
int zmq_transport_missing_endpoint_throws(const std::string &dir);
int zmq_transport_valid(const std::string &dir);

// ── Move semantics ──────────────────────────────────────────────────────────
int move_construct(const std::string &dir);
int move_assign(const std::string &dir);

// ── Checksum ────────────────────────────────────────────────────────────────
int checksum_default_enforced(const std::string &dir);
int checksum_explicit_manual(const std::string &dir);
int checksum_explicit_none(const std::string &dir);
int checksum_invalid_throws(const std::string &dir);
int checksum_null_default_enforced(const std::string &dir);
int unknown_key_throws(const std::string &dir);

// ── Logging ─────────────────────────────────────────────────────────────────
int logging_default_all_defaults(const std::string &dir);
int logging_explicit_all_fields(const std::string &dir);
int logging_partial_mixes_defaults_and_explicit(const std::string &dir);
int logging_zero_backups_throws(const std::string &dir);
int logging_backups_negative_one_keeps_all_files(const std::string &dir);
int logging_negative_backups_other_throws(const std::string &dir);
int logging_zero_max_size_throws(const std::string &dir);
int logging_negative_max_size_throws(const std::string &dir);
int logging_unknown_sub_key_throws(const std::string &dir);
int logging_not_object_throws(const std::string &dir);
int logging_fractional_max_size_accepted(const std::string &dir);

} // namespace role_config
} // namespace pylabhub::tests::worker
