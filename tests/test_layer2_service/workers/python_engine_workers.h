#pragma once
/**
 * @file python_engine_workers.h
 * @brief Workers for PythonEngine tests — Pattern 3 conversion.
 *
 * L2 scope (see docs/HEP/HEP-CORE-0001-hybrid-lifecycle-model.md §
 * "Testing implications"): each worker constructs PythonEngine +
 * RoleHostCore + RoleAPIBase in isolation.  No hub::ShmQueue /
 * hub::ZmqQueue / DataBlock / BrokerService / Messenger — those
 * belong to L3 and are exercised by `test_layer3_datahub/`.
 *
 * Mirrors the LuaEngine worker suite (lua_engine_workers.{h,cpp})
 * with Python-specific adjustments: scripts are Python __init__.py
 * packages under <dir>/script/python/; return values are True/False/
 * None; supports_multi_state() returns false; create_thread_state()
 * returns nullptr.
 *
 * Chunk 1 — Lifecycle, type registration, alias creation.
 */
#include <string>

namespace pylabhub::tests::worker
{
namespace python_engine
{

// ── Lifecycle ───────────────────────────────────────────────────────────────
int full_lifecycle(const std::string &dir);
int initialize_and_finalize_succeeds(const std::string &dir);

// ── Type registration ──────────────────────────────────────────────────────
int register_slot_type_sizeof_correct(const std::string &dir);
int register_slot_type_multi_field(const std::string &dir);
int register_slot_type_packed_packing(const std::string &dir);
int register_slot_type_has_schema_false_returns_false(const std::string &dir);
/// NEW: coverage fill — registers every scalar type the engine supports.
int register_slot_type_all_supported_types(const std::string &dir);

// ── Alias creation (SlotFrame / FlexFrame based on role_tag) ────────────────
int alias_slot_frame_producer(const std::string &dir);
int alias_slot_frame_consumer(const std::string &dir);
int alias_no_alias_processor(const std::string &dir);
int alias_flex_frame_producer(const std::string &dir);
int alias_producer_no_fz_no_flex_frame_alias(const std::string &dir);

// ── invoke_produce (chunk 2) ────────────────────────────────────────────────
//
// Cover the on_produce(tx, msgs, api) return-value contract.  Tests
// commit/discard happy paths, None-return error, None-slot handling,
// Python exception propagation, and wrong-return-type paths (int, str).
// Plus one gap-fill: NEW Discard-doesn't-rollback-script-writes
// contract.  See each body's docblock for strengthening rationale
// versus the pre-conversion V2 test.
int invoke_produce_commit_on_true(const std::string &dir);
int invoke_produce_discard_on_false(const std::string &dir);
int invoke_produce_none_return_is_error(const std::string &dir);
int invoke_produce_none_slot(const std::string &dir);
int invoke_produce_script_error(const std::string &dir);
int invoke_produce_wrong_return_type_is_error(const std::string &dir);
int invoke_produce_wrong_return_string_is_error(const std::string &dir);
int invoke_produce_discard_on_false_but_python_wrote_slot(const std::string &dir);

// ── invoke_consume (chunk 3) ────────────────────────────────────────────────
//
// Consumer callback contract.  Splits V2's single
// ReceivesReadOnlySlot test into two: a happy-path receives test, and
// a dedicated read-only enforcement test that actually attempts a
// write.  Plus strengthened None-slot and script-error tests.
int invoke_consume_receives_slot(const std::string &dir);
int invoke_consume_none_slot(const std::string &dir);
int invoke_consume_script_error_detected(const std::string &dir);
int invoke_consume_rx_slot_is_read_only(const std::string &dir);

// ── invoke_process (chunk 4) ────────────────────────────────────────────────
//
// Processor dual-slot callback.  Three V2 tests renamed to match
// the Lua chunk 4 naming (None vs Nil; both_slots vs NilInput;
// rx_present_tx_none vs InputOnlyNoOutput) so the contract is
// explicit and cross-engine parity easy to audit.  Plus one NEW
// gap-fill: the processor's dual-slot path must enforce the same
// rx-read-only contract as the consumer path.
int invoke_process_dual_slots(const std::string &dir);
int invoke_process_both_slots_none(const std::string &dir);
int invoke_process_rx_present_tx_none(const std::string &dir);
int invoke_process_rx_slot_is_read_only(const std::string &dir);

// ── Messages (chunk 5) ─────────────────────────────────────────────────────
//
// Two projection paths (build_messages_list_ vs
// build_messages_list_bare_).  Tests pin (a) event-message
// details-map promotion, (b) empty-vector edge case, (c) producer
// data-message (sender_hex, bytes) tuple shape, (d) consumer bare-
// bytes divergence.  Mirrors Lua chunk 5 with Python-specific shape
// assertions.
int invoke_produce_receives_messages_event_with_details(const std::string &dir);
int invoke_produce_receives_messages_empty_list(const std::string &dir);
int invoke_produce_receives_messages_data_message(const std::string &dir);
int invoke_consume_receives_messages_data_bare_format(const std::string &dir);

// ── API closures (chunk 6) ─────────────────────────────────────────────────
//
// pybind11-bound api.* methods + the module-level version_info().
// V2 had 6 tests; P3 collapses to 5 by merging the two stop_reason
// variants (Default + PeerDead) into one exhaustive enum-values
// coverage that also adds HubDead (never tested in V2).
int api_version_info_returns_json_string(const std::string &dir);
int wrong_role_module_import_raises_error(const std::string &dir);
int api_stop_sets_shutdown_requested(const std::string &dir);
int api_critical_error_set_and_read_and_stop_reason(const std::string &dir);
int api_stop_reason_reflects_all_enum_values(const std::string &dir);

// ── Metrics + error accumulation (chunk 7) ────────────────────────────────
//
// Individual accessors on api (out_slots_written etc.) and the
// hierarchical api.metrics() dict.  Covers the canonical 5-field
// loop group per PYLABHUB_LOOP_METRICS_FIELDS (role_host_core.hpp:
// 397) — V2 missed acquire_retry_count.  Plus stop_on_script_error
// error-path semantics and cross-link coverage between the two
// metrics surfaces.
int metrics_individual_accessors_read_core_counters_live(const std::string &dir);
int metrics_in_slots_received_works_consumer(const std::string &dir);
int multiple_errors_count_accumulates(const std::string &dir);
int stop_on_script_error_sets_shutdown_on_error(const std::string &dir);
int metrics_all_loop_fields_anchored_values(const std::string &dir);
int metrics_hierarchical_table_producer_full_shape(const std::string &dir);
int metrics_role_script_error_count_reflects_raised_error(const std::string &dir);

// ── Engine-internal dispatcher contract (chunk 1 gap-fill) ─────────────────
//
// Pins PythonEngine's supports_multi_state() returns false.
// Engine-to-caller C++ contract per script_engine.hpp:388-394:
// "True if the engine handles multi-threaded invoke() internally.
//  When false (Python): non-owner requests are queued to the owner
//  thread. The engine manages threading internally — callers just
//  call invoke()."
// Not script-visible — purely a C++ dispatch hint.  Pinned as a
// dedicated test so a regression flipping it to true (e.g., a
// subinterpreter refactor that didn't update the dispatcher)
// surfaces loudly.
int supports_multi_state_returns_false(const std::string &dir);

} // namespace python_engine
} // namespace pylabhub::tests::worker
