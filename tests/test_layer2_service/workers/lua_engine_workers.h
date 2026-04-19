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

} // namespace lua_engine
} // namespace pylabhub::tests::worker
