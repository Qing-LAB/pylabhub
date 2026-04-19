#pragma once
/**
 * @file scriptengine_native_dylib_workers.h
 * @brief Workers for NativeEngine test suite (Pattern 3).
 *
 * Each worker constructs a NativeEngine + RoleHostCore + RoleAPIBase,
 * loads the test .so plugin, exercises one callback path, and asserts
 * on the observable outcome. The parent passes the plugin search
 * directory (TEST_PLUGIN_DIR at build time) as argv[2] so the
 * subprocess can locate the same .so artefacts the build staged.
 */

#include <string>

namespace pylabhub::tests::worker
{
namespace native_engine
{

// ── Core lifecycle + callback paths ─────────────────────────────────────────
int full_lifecycle_produce_commits_and_writes_slot(const std::string &plugin_dir);
int has_callback_reflects_plugin_symbols(const std::string &plugin_dir);
int schema_validation_matching_schema_succeeds(const std::string &plugin_dir);
int schema_validation_has_schema_false_returns_false(const std::string &plugin_dir);
int schema_validation_mismatched_schema_fails(const std::string &plugin_dir);
int load_script_missing_file_returns_false();
int load_script_missing_required_callback_returns_false(const std::string &plugin_dir);
int eval_returns_not_found(const std::string &plugin_dir);
int generic_invoke_known_callback_returns_true(const std::string &plugin_dir);
int supports_multi_state_default_false(const std::string &plugin_dir);
int context_fields_passed_to_plugin(const std::string &plugin_dir);

// ── Checksum ────────────────────────────────────────────────────────────────
int checksum_wrong_hash_rejects_plugin(const std::string &plugin_dir);
int checksum_empty_hash_skips_verification(const std::string &plugin_dir);

// ── Native API counters / metrics ───────────────────────────────────────────
int api_counters_and_schema_size_through_native_module(const std::string &plugin_dir);

// ── FullStartup (simple schema) ─────────────────────────────────────────────
int full_startup_producer_slot_only(const std::string &plugin_dir);
int full_startup_consumer(const std::string &plugin_dir);
int full_startup_processor(const std::string &plugin_dir);

// ── FullStartup (multifield) ────────────────────────────────────────────────
int full_startup_producer_multifield(const std::string &plugin_dir);
int full_startup_consumer_multifield(const std::string &plugin_dir);
int full_startup_processor_multifield(const std::string &plugin_dir);

// ── FullStartup (slot + flexzone) ───────────────────────────────────────────
int full_startup_producer_slot_and_flexzone(const std::string &plugin_dir);

// ── Inbox + channel ─────────────────────────────────────────────────────────
int invoke_on_inbox_typed_data(const std::string &plugin_dir);
int api_band_pub_sub_no_broker_graceful_return(const std::string &plugin_dir);

} // namespace native_engine
} // namespace pylabhub::tests::worker
