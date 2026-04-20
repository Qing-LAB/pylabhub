#pragma once
/**
 * @file lua_engine_workers.h
 * @brief Workers for LuaEngine tests — Pattern 3 conversion.
 *
 * L2 scope (see docs/HEP/HEP-CORE-0001-hybrid-lifecycle-model.md §
 * "Testing implications"): each worker constructs LuaEngine +
 * RoleHostCore + RoleAPIBase in isolation.  No hub::ShmQueue /
 * hub::ZmqQueue / DataBlock / BrokerService / Messenger — those
 * belong to L3 and are exercised by `test_layer3_datahub/`.
 *
 * The worker uses `InvokeTx{&buf, sizeof(buf)}` /
 * `InvokeRx{&buf, sizeof(buf)}` at the engine boundary — the same
 * raw-buffer contract the engine actually sees in production,
 * without depending on how the data got there.
 *
 * Chunk 1 — Lifecycle, type registration, alias creation (11 tests
 * + 1 new coverage-gap fill for all supported types).
 */

#include <string>

namespace pylabhub::tests::worker
{
namespace lua_engine
{

// ── Lifecycle ───────────────────────────────────────────────────────────────
int full_lifecycle(const std::string &dir);
int initialize_and_finalize_succeeds(const std::string &dir);

// ── Type registration ──────────────────────────────────────────────────────
int register_slot_type_sizeof_correct(const std::string &dir);
int register_slot_type_multi_field(const std::string &dir);
int register_slot_type_packed_vs_aligned(const std::string &dir);
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
int invoke_produce_commit_on_true(const std::string &dir);
int invoke_produce_discard_on_false(const std::string &dir);
int invoke_produce_nil_return_is_error(const std::string &dir);
int invoke_produce_nil_slot(const std::string &dir);
int invoke_produce_script_error(const std::string &dir);
/// NEW: documents that Discard does NOT roll back Lua-side writes to tx.slot.
int invoke_produce_discard_on_false_but_lua_wrote_slot(const std::string &dir);

// ── invoke_consume (chunk 3) ────────────────────────────────────────────────
int invoke_consume_receives_slot(const std::string &dir);
int invoke_consume_nil_slot(const std::string &dir);
int invoke_consume_script_error_detected(const std::string &dir);
/// NEW: verifies rx.slot is read-only — Lua cannot mutate the source buffer.
int invoke_consume_rx_slot_is_read_only(const std::string &dir);

// ── Messages (chunk 5) ──────────────────────────────────────────────────────
//
// The Lua engine projects std::vector<IncomingMessage> into a Lua table
// with two different shapes depending on whether the message is an
// "event" (m.event non-empty) or "data" (m.event empty):
//
//   Event message (any role):
//     msgs[i] = { event = "...", <details_key> = <details_value>, ... }
//     (details map keys are promoted to table fields, NOT nested)
//
//   Data message:
//     Producer / Processor (push_messages_table_):
//       msgs[i] = { sender = "<hex string>", data = "<raw bytes>" }
//     Consumer (push_messages_table_bare_):
//       msgs[i] = "<raw bytes>"           -- no sender, bare byte string
//
// The tests below cover every shape, including the empty-msgs edge case
// and the consumer/non-consumer divergence for data messages.
int invoke_produce_receives_messages_event_with_details(const std::string &dir);
/// NEW: empty msgs vector round-trip.
int invoke_produce_receives_messages_empty_vector(const std::string &dir);
/// NEW: data message (empty event, with sender + bytes payload).
int invoke_produce_receives_messages_data_message(const std::string &dir);
/// NEW: consumer bare-format data message (plain byte string).
int invoke_consume_receives_messages_data_bare_format(const std::string &dir);

// ── API closures: introspection + control (chunk 6a) ──────────────────────
//
// These tests cover the api.* closures that don't require queue,
// broker, or inbox infrastructure — i.e., the closures that fit
// cleanly at L2. Closures deferred to L3 (because they need real
// queues/brokers): in_capacity, out_capacity, last_seq,
// slot_logical_size, flexzone_logical_size, spinlock_*, in_policy,
// out_policy, band_*, open_inbox, wait_for_role, clear_inbox_cache,
// set_verify_checksum, update_flexzone_checksum.
int api_version_info_returns_json_string(const std::string &dir);
int api_identity_uid_name_channel(const std::string &dir);
int api_log_dispatches_levels(const std::string &dir);
int api_stop_sets_shutdown_requested(const std::string &dir);
int api_critical_error_set_and_read_and_stop_reason(const std::string &dir);
/// NEW: exhaustive read of `api.stop_reason()` for every StopReason
/// enum value injected from C++ (Normal, PeerDead, HubDead,
/// CriticalError).  Closes the coverage gap left by chunk 6a's
/// Lua-set-Lua-read path (which only exercises Normal + CriticalError)
/// and subsumes two V2 tests that covered PeerDead / CriticalError
/// via separate scripts.  HubDead was never tested before this.
int api_stop_reason_reflects_all_enum_values(const std::string &dir);

// ── API closures: custom metrics (chunk 6b) ────────────────────────────────
//
// `api.report_metric(key, value)` / `api.report_metrics({...})` /
// `api.clear_custom_metrics()` / readback via `api.metrics().custom`.
// All closures route through RoleHostCore's custom_metrics_ map (see
// src/scripting/lua_engine.cpp:1622-1654).  L2-scoped: no queue / no
// broker / no SHM infrastructure needed.
int api_report_metric_appears_under_custom(const std::string &dir);
int api_report_metric_overwrite_same_key(const std::string &dir);
int api_report_metric_zero_value_preserved(const std::string &dir);
int api_report_metrics_batch_accepts_table(const std::string &dir);
int api_report_metrics_non_table_arg_is_error(const std::string &dir);
int api_clear_custom_metrics_empties_and_allows_rewrite(const std::string &dir);

// ── API closures: shared data (chunk 6c) ───────────────────────────────────
//
// `api.get_shared_data(key)` / `api.set_shared_data(key, value)` backed
// by RoleHostCore::shared_data_ (std::variant<int64_t, double, bool,
// std::string>).  L2-scoped: no threading, no broker.  The
// cross-thread variant is deferred to a later chunk that covers
// multi-state / thread-state plumbing.
int api_shared_data_round_trip_all_variant_types(const std::string &dir);
int api_shared_data_get_missing_key_returns_nil(const std::string &dir);
int api_shared_data_nil_removes_key(const std::string &dir);
int api_shared_data_overwrite_changes_type(const std::string &dir);
int api_shared_data_overwrite_changes_value_same_type(const std::string &dir);

// ── Error handling: runtime error surfacing (chunk 7a) ─────────────────────
//
// These bodies pin the engine's contract for how script-side errors
// are reflected to the C++ caller:
//   - script_error_count increments per error
//   - InvokeResult::Error (or InvokeStatus::ScriptError for eval)
//   - stop_on_script_error=true additionally flips shutdown_requested
//   - on_init / on_stop / on_inbox errors are observed the same way
//     as on_produce / on_consume / on_process errors
//   - the engine stays usable after an error (can invoke again
//     successfully with a good callback)
int invoke_multiple_errors_count_accumulates(const std::string &dir);
int invoke_produce_wrong_return_type_is_error(const std::string &dir);
int invoke_produce_wrong_return_string_is_error(const std::string &dir);
int invoke_produce_stop_on_script_error_sets_shutdown(const std::string &dir);
int invoke_on_init_or_stop_script_error_accumulates(const std::string &dir);
int invoke_on_inbox_script_error_increments_count(const std::string &dir);
int eval_syntax_error_returns_script_error(const std::string &dir);

// ── Error handling: setup-phase error paths (chunk 7b) ─────────────────────
//
// Setup-phase failures (load_script / register_slot_type bugs) must
// return false and leave the engine usable — the role host may retry
// with corrected inputs.  None of these failures should increment
// script_error_count (that counter is reserved for runtime/script
// errors per HEP-CORE-0019 semantics).  Finalize_DoubleCallIsSafe
// is a gap-fill for engine.finalize() idempotence.
int load_script_missing_file_returns_false(const std::string &dir);
int load_script_missing_required_callback_returns_false(const std::string &dir);
int load_script_syntax_error_returns_false(const std::string &dir);
int register_slot_type_bad_field_type_returns_false(const std::string &dir);
int finalize_double_call_is_safe(const std::string &dir);

// ── Generic engine.invoke() / engine.eval() (chunk 8a) ─────────────────────
//
// Separate from the role callbacks (invoke_produce / invoke_consume /
// invoke_process).  engine.invoke(name[, args]) dispatches to any named
// Lua function; engine.eval(code) runs an arbitrary Lua chunk and
// captures the top-of-stack result as JSON.  Both have their own
// contracts — lookup semantics, error counting, post-finalize behavior
// — that the per-role callbacks do not exercise.
int invoke_existing_function_returns_true(const std::string &dir);
int invoke_non_existent_function_returns_false(const std::string &dir);
int invoke_empty_name_returns_false(const std::string &dir);
int invoke_script_error_returns_false_and_increments_errors(const std::string &dir);
int invoke_with_args_returns_true(const std::string &dir);
int invoke_after_finalize_returns_false(const std::string &dir);
int eval_returns_scalar_result(const std::string &dir);

// ── Multi-state / thread-state (chunk 8b) ──────────────────────────────────
//
// LuaEngine's owner thread holds the primary lua_State. A non-owner
// thread that calls invoke()/eval() gets a thread-local child engine
// (see lua_engine.cpp:409 get_or_create_thread_state_).  Lua globals
// are PER-STATE — they do NOT leak between threads.  RoleHostCore's
// shared_data IS thread-shared (single C++ map).  These tests pin
// both sides of that contract.
int supports_multi_state_returns_true(const std::string &dir);
int state_persists_across_calls(const std::string &dir);
int invoke_from_non_owner_thread_works(const std::string &dir);
int invoke_non_owner_thread_uses_independent_state(const std::string &dir);
int invoke_concurrent_owner_and_non_owner(const std::string &dir);
int shared_data_cross_thread_visible(const std::string &dir);

// ── invoke_process (chunk 4) ────────────────────────────────────────────────
//
// Processor design note: a processor ALWAYS has an input channel (if a
// role has no input channel it is a consumer, not a processor). Therefore
// nil rx.slot inside on_process represents a runtime condition — the input
// queue timed out, or the upstream has no data this iteration — not a
// missing channel.  Nil tx.slot similarly represents "the output queue has
// no slot to write into right now" (backpressure), not "no output channel".
//
// The worker scenario names below describe the slot-state combination
// (BothSlotsNil, RxPresent_TxNil, RxPresent_TxPresent aka DualSlots) so
// the tests are unambiguous about what they actually exercise.
int invoke_process_dual_slots(const std::string &dir);
int invoke_process_both_slots_nil(const std::string &dir);
int invoke_process_rx_present_tx_nil(const std::string &dir);
/// NEW: rx read-only contract in the processor dual-slot code path.
int invoke_process_rx_slot_is_read_only(const std::string &dir);

} // namespace lua_engine
} // namespace pylabhub::tests::worker
