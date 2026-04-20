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
