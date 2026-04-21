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

// ── Load script + script errors (chunk 8) ─────────────────────────────────
//
// Covers load_script failure paths, register_slot_type bad-field
// (fixed from V2 which used a non-canonical name and silently
// short-circuited on the name check), has_callback contract
// including non-canonical names, and on_init/on_stop/on_inbox
// exception accounting (EXPECT_EQ instead of V2's EXPECT_GE).
int load_script_missing_file(const std::string &dir);
int load_script_missing_required_callback(const std::string &dir);
int register_slot_type_bad_field_type(const std::string &dir);
int load_script_syntax_error(const std::string &dir);
int has_callback(const std::string &dir);
int invoke_on_init_script_error(const std::string &dir);
int invoke_on_stop_script_error(const std::string &dir);
int invoke_on_inbox_script_error(const std::string &dir);

// ── Generic invoke + threading (chunk 10) ──────────────────────────────────
//
// Python's generic invoke() is dispatch-queued because
// supports_multi_state() == false.  Chunk 10 workers pin:
//   - direct (owner-thread) dispatch path
//   - queued (non-owner) dispatch path + FIFO drain via hot-path
//   - finalize() cancels ALL pending (not just the first)
//   - admin-side dispatch failures (NotFound, empty name) MUST NOT
//     increment script_error_count (reserved for script runtime errors)
//   - invoke(name, args) reaches the callee with kwargs expansion on
//     both non-empty and empty-dict args branches
int invoke_existing_function_returns_true(const std::string &dir);
int invoke_non_existent_function_returns_false(const std::string &dir);
int invoke_empty_name_returns_false(const std::string &dir);
int invoke_script_error_returns_false_and_increments_errors(const std::string &dir);
int invoke_from_non_owner_thread_queued(const std::string &dir);
int invoke_from_non_owner_thread_finalize_unblocks(const std::string &dir);
int invoke_concurrent_non_owner_threads(const std::string &dir);
int invoke_after_finalize_returns_false(const std::string &dir);
int invoke_with_args_calls_function(const std::string &dir);
int invoke_with_args_from_non_owner_thread(const std::string &dir);

// ── Eval + shared data (chunk 11) ──────────────────────────────────────────
//
// eval() return-value scalar + container coverage; eval() error paths
// (NameError, SyntaxError, ZeroDivisionError) all increment
// script_error_count; api.shared_data is a reference-backed py::dict
// (not a JSON copy) — identity, mutable-container propagation, and
// lifecycle (on_init → on_produce → on_stop) all pinned.
int eval_returns_scalar_result(const std::string &dir);
int eval_error_returns_empty(const std::string &dir);
int shared_data_persists_across_callbacks(const std::string &dir);

// ── API diagnostics + identity (chunk 12) ──────────────────────────────────
//
// Live-read pins for core-backed accessors (loop_overrun_count,
// last_cycle_work_us, critical_error, stop_reason), full identity
// surface including role_dir-derived logs_dir/run_dir, and exact-1
// error semantics for report_metrics type mismatch.
int api_loop_overrun_count_reads_from_core(const std::string &dir);
int api_last_cycle_work_us_reads_from_core(const std::string &dir);
// V2's Api_CriticalError_DefaultIsFalse dropped — covered by chunk 6's
// api_critical_error_set_and_read_and_stop_reason (same code path via
// pybind11 wrapper).
int api_identity_accessors_return_correct_values(const std::string &dir);
// Note: V2's Api_StopReason_AfterCriticalError is covered by chunk 6's
// api_stop_reason_reflects_all_enum_values (all 4 enum values) + chunk 6's
// api_critical_error_set_and_read_and_stop_reason (bundled atomicity).
int api_environment_strings_logs_dir_run_dir(const std::string &dir);
int api_report_metrics_non_dict_arg_is_error(const std::string &dir);

// ── Custom metrics (chunk 13) ──────────────────────────────────────────────
//
// report_metric / report_metrics / clear_custom_metrics semantics:
// round-trip through RoleHostCore snapshot (not a Python-side local),
// cross-invoke persistence, overwrite-not-accumulate on same key,
// zero/negative/edge values storable and distinguishable from absent.
int api_custom_metrics_report_and_read_in_metrics(const std::string &dir);
int api_custom_metrics_batch_and_clear(const std::string &dir);
int api_custom_metrics_overwrite_same_key(const std::string &dir);
int api_custom_metrics_zero_value(const std::string &dir);
int api_custom_metrics_report_type_errors(const std::string &dir);

// ── Queue state / spinlock / channels / open_inbox / numpy (chunk 14) ─────
//
// Producer/Consumer/Processor queue-state defaults (all zero/empty when
// no queue wired), processor dual-channel accessors, open_inbox without
// broker, spinlock count + error-without-SHM, api.as_numpy round-trip
// for array fields + TypeError on scalar fields.
//
// Note: V2's Api_Flexzone_WithoutSHM_ReturnsNone is fully covered by
// chunk 9's invoke_produce_slot_only_no_flexzone_on_invoke (which
// pins api.flexzone() is None, api.flexzone(api.Tx) is None, and
// update_flexzone_checksum() is False) — not migrated separately.
int api_producer_queue_state_without_queue(const std::string &dir);
int api_consumer_queue_state_without_queue(const std::string &dir);
int api_processor_queue_state_dual_defaults(const std::string &dir);
int api_processor_channels_in_out(const std::string &dir);
int api_open_inbox_without_broker(const std::string &dir);
int api_spinlock_count_without_shm(const std::string &dir);
int api_spinlock_without_shm_is_error(const std::string &dir);
int api_as_numpy_array_field(const std::string &dir);
int api_as_numpy_non_array_field_throws(const std::string &dir);

// ── FullLifecycle + FullStartup (chunk 15) ─────────────────────────────────
//
// Full engine lifecycle via EngineModuleParams + engine_lifecycle_startup/
// shutdown — the production setup path.  Pins SlotFrame/FlexFrame alias
// semantics (single-direction roles only), callback ordering, data-transform
// round-trip for processor, and shutdown idempotency.
int full_lifecycle_verifies_callback_execution(const std::string &dir);
int full_startup_producer_slot_only(const std::string &dir);
int full_startup_producer_slot_and_flexzone(const std::string &dir);
int full_startup_consumer(const std::string &dir);
int full_startup_processor(const std::string &dir);

// ── SlotLogicalSize + FlexzoneLogicalSize + multifield + band (chunk 16) ──
//
// api.slot_logical_size() / api.flexzone_logical_size() round-trip for
// aligned vs packed vs complex-mixed schemas; multifield data round-trip
// through padded offsets (producer/consumer/processor) with pad-byte
// corruption guards; band_join/leave/broadcast/members all return
// gracefully without a broker wired.
int slot_logical_size_aligned_padding_sensitive(const std::string &dir);
int slot_logical_size_packed_no_padding(const std::string &dir);
int slot_logical_size_complex_mixed_aligned(const std::string &dir);
int flexzone_logical_size_array_fields(const std::string &dir);
int full_startup_producer_multifield(const std::string &dir);
int full_startup_consumer_multifield(const std::string &dir);
int full_startup_processor_multifield(const std::string &dir);
int api_band_all_methods_graceful_no_broker(const std::string &dir);

// ── State + slot + inbox (chunk 9) ─────────────────────────────────────────
//
// State persistence across calls, slot-only producer without flexzone,
// and three inbox contracts (typed payload, canonical type_sizeof,
// missing-type error path).  Mirrors lua chunk 9; each body is
// strengthened vs the V2 original — details in per-body docblocks.
int state_persists_across_calls(const std::string &dir);
int invoke_produce_slot_only_no_flexzone_on_invoke(const std::string &dir);
int invoke_on_inbox_typed_data(const std::string &dir);
int type_sizeof_inbox_frame_returns_correct_size(const std::string &dir);
int invoke_on_inbox_missing_type_reports_error(const std::string &dir);

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
