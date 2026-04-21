/**
 * @file test_python_engine.cpp
 * @brief Pattern 3 (isolated-subprocess) test wrappers for PythonEngine.
 *
 * This file is intentionally thin: every test here is a `SpawnWorker` +
 * `ExpectWorkerOk` call that delegates the actual engine driving to a
 * subprocess worker.  The worker functions live in
 * `workers/python_engine_workers.cpp` and handle:
 *   - Constructing PythonEngine + RoleHostCore + RoleAPIBase in isolation.
 *   - Loading a Python script package (`script/python/__init__.py`).
 *   - Driving invoke_produce / invoke_consume / invoke_process /
 *     invoke_on_inbox / invoke / eval with raw memory buffers.
 *   - Verifying script-side assertions via `script_error_count()` plus
 *     C++-side cross-checks (e.g., `core.custom_metrics_snapshot()`).
 *
 * No engine construction, no test fixture state, no helper that touches
 * engine internals lives in this file — all of that was retired
 * 2026-04-21 along with the V2 `PythonEngineTest` in-process fixture.
 *
 * ## Coverage (delegated to the worker suite)
 *
 * The worker-side docstring (python_engine_workers.h) is authoritative
 * for per-chunk coverage.  Chunks migrated:
 *   1   — lifecycle + type registration + alias creation
 *   2-5 — invoke_produce / _consume / _process / messages
 *   6-8 — API closures, metrics accumulation, load_script errors
 *   9   — state persistence + slot-only + typed inbox + missing-type
 *  10-11 — generic invoke (thread queue) + eval + shared data
 *  12   — API diagnostics + identity
 *  13   — custom metrics round-trip
 *  14   — queue-state + spinlock + channels + open_inbox + numpy
 *  15   — FullLifecycle + FullStartup (producer/consumer/processor)
 *  16   — SlotLogicalSize / FlexzoneLogicalSize + multifield + band
 */
#include <gtest/gtest.h>

#include "test_patterns.h"     // Pattern 3 (IsolatedProcessTest) base

#include <atomic>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace
{

// ============================================================================
// PythonEngineIsolatedTest — Pattern 3 test fixture
//
// All L2 PythonEngine tests run as subprocess workers via SpawnWorker.
// Bodies live in workers/python_engine_workers.cpp.  The older V2
// PythonEngineTest fixture (in-process gtest with a shared tmp_ dir,
// setup_engine helpers, and make_api) was removed 2026-04-21 after
// the last V2 test was migrated — all 103 engine tests now use
// Pattern 3 exclusively.
// ============================================================================

class PythonEngineIsolatedTest : public pylabhub::tests::IsolatedProcessTest
{
  protected:
    void TearDown() override
    {
        for (const auto &p : paths_to_clean_)
        {
            std::error_code ec;
            fs::remove_all(p, ec);
        }
        paths_to_clean_.clear();
    }

    std::string unique_dir(const char *label)
    {
        static std::atomic<int> ctr{0};
        fs::path p = fs::temp_directory_path() /
                     ("plh_l2_py_" + std::string(label) + "_" +
                      std::to_string(::getpid()) + "_" +
                      std::to_string(ctr.fetch_add(1)));
        fs::create_directories(p);
        paths_to_clean_.push_back(p);
        return p.string();
    }

    std::vector<fs::path> paths_to_clean_;
};

} // namespace

TEST_F(PythonEngineIsolatedTest, FullLifecycle)
{
    auto w = SpawnWorker("python_engine.full_lifecycle",
                         {unique_dir("full_lifecycle")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, InitializeAndFinalize_Succeeds)
{
    auto w = SpawnWorker("python_engine.initialize_and_finalize_succeeds",
                         {unique_dir("init_finalize")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, RegisterSlotType_SizeofCorrect)
{
    auto w = SpawnWorker("python_engine.register_slot_type_sizeof_correct",
                         {unique_dir("reg_sizeof")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, RegisterSlotType_MultiField)
{
    auto w = SpawnWorker("python_engine.register_slot_type_multi_field",
                         {unique_dir("reg_multi")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, RegisterSlotType_PackedPacking)
{
    // Pins the packed-packing size anchor. The engine's internal
    // cross-validation (python_engine.cpp) catches any
    // silent packing-ignore regression — see worker's docblock.
    auto w = SpawnWorker("python_engine.register_slot_type_packed_packing",
                         {unique_dir("reg_packed_packing")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, RegisterSlotType_HasSchemaFalse_ReturnsFalse)
{
    auto w = SpawnWorker(
        "python_engine.register_slot_type_has_schema_false_returns_false",
        {unique_dir("reg_no_schema")});
    // register_slot_type logs an ERROR when called with has_schema=false
    // (python_engine.cpp). That log is expected.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/
                   {"called with has_schema=false"});
}

TEST_F(PythonEngineIsolatedTest, RegisterSlotType_AllSupportedTypes_Succeeds)
{
    auto w = SpawnWorker(
        "python_engine.register_slot_type_all_supported_types",
        {unique_dir("reg_all_types")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, Alias_SlotFrame_Producer)
{
    auto w = SpawnWorker("python_engine.alias_slot_frame_producer",
                         {unique_dir("alias_slot_prod")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, Alias_SlotFrame_Consumer)
{
    auto w = SpawnWorker("python_engine.alias_slot_frame_consumer",
                         {unique_dir("alias_slot_cons")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, Alias_NoAlias_Processor)
{
    auto w = SpawnWorker("python_engine.alias_no_alias_processor",
                         {unique_dir("alias_noalias_proc")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, Alias_FlexFrame_Producer)
{
    auto w = SpawnWorker("python_engine.alias_flex_frame_producer",
                         {unique_dir("alias_flex_prod")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, Alias_ProducerNoFz_NoFlexFrameAlias)
{
    auto w = SpawnWorker(
        "python_engine.alias_producer_no_fz_no_flex_frame_alias",
        {unique_dir("alias_nofz")});
    ExpectWorkerOk(w);
}

// NEW gap-fill: pin PythonEngine's supports_multi_state() returns false.
// Script-invisible engine-to-caller C++ contract (per script_engine.hpp:
// 388-394).  Checking it as a structural property both pre- and post-
// build_api so a regression flipping it based on state surfaces.
TEST_F(PythonEngineIsolatedTest, SupportsMultiState_ReturnsFalse)
{
    auto w = SpawnWorker("python_engine.supports_multi_state_returns_false",
                         {unique_dir("multi_state")});
    ExpectWorkerOk(w);
}

// ============================================================================
// (V2 chunk-1 tests removed — converted to Pattern 3 at top of file.)

// ============================================================================
// Chunk 2 — invoke_produce (Pattern 3)
//
// The on_produce(tx, msgs, api) return-value contract.  Bodies live in
// workers/python_engine_workers.cpp.  Python-specific vs Lua (beyond
// dict-vs-table surface):
//
//   - `return None` logs ERROR (unified with Lua's nil-return severity
//     in this session — python_engine.cpp; was previously WARN
//     which silently mismatched Lua).
//   - Non-boolean-non-None (int, str, …) logs ERROR with the Python
//     type name; isinstance(True, int) == True, so bool must be
//     checked BEFORE any int/numeric check (currently there is no int
//     check — falls through to generic non-boolean path).
//
// Strengthening highlights in the worker bodies:
//   - CommitOnTrue: asserts script_error_count == 0 post-invoke.
//   - DiscardOnFalse: sentinel buf = 777.0f (detects engine-internal
//     writes the V2 body missed with buf = 0.0f).
//   - NoneReturn: pins the ERROR log text via expected_error_substrings.
//   - NoneSlot: asserts script_error_count == 0 to confirm the Python
//     `assert tx.slot is None` actually held (V2 couldn't distinguish
//     assert-passed from assert-failed since both returned Discard).
//   - DiscardOnFalse_ButPythonWroteSlot (NEW): pins the "engine does
//     NOT roll back Python writes to tx.slot on Discard" contract —
//     mirrors Lua's gap-fill.
// ============================================================================

TEST_F(PythonEngineIsolatedTest, InvokeProduce_CommitOnTrue)
{
    auto w = SpawnWorker("python_engine.invoke_produce_commit_on_true",
                         {unique_dir("produce_commit")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, InvokeProduce_DiscardOnFalse)
{
    auto w = SpawnWorker("python_engine.invoke_produce_discard_on_false",
                         {unique_dir("produce_discard")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, InvokeProduce_NoneReturn_IsError)
{
    auto w = SpawnWorker("python_engine.invoke_produce_none_return_is_error",
                         {unique_dir("produce_none_return")});
    // Engine logs ERROR "on_produce returned None — explicit 'return
    // True' or 'return False' is required. Treating as error."
    // (python_engine.cpp).  A reworded diagnostic that
    // still returned Error would keep the worker body green — this
    // parent-level check catches text regressions.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/
                   {"on_produce returned None"});
}

TEST_F(PythonEngineIsolatedTest, InvokeProduce_NoneSlot)
{
    auto w = SpawnWorker("python_engine.invoke_produce_none_slot",
                         {unique_dir("produce_none_slot")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, InvokeProduce_ScriptError)
{
    auto w = SpawnWorker("python_engine.invoke_produce_script_error",
                         {unique_dir("produce_script_error")});
    // on_python_error_ logs ERROR "<tag> on_produce error: <exc msg>"
    // (python_engine.cpp).  Pin the RuntimeError text.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/{"intentional error"});
}

TEST_F(PythonEngineIsolatedTest, InvokeProduce_WrongReturnType_IsError)
{
    auto w = SpawnWorker(
        "python_engine.invoke_produce_wrong_return_type_is_error",
        {unique_dir("produce_wrong_type")});
    // Engine logs ERROR "<cb> returned non-boolean type '<typename>' —
    // expected 'return True' or 'return False'." (python_engine.cpp:
    // 1204-1207).  Pin both the diagnostic and the `'int'` type name
    // so a regression dropping the type name (or coercing int to bool
    // via isinstance(True, int)==True) would be caught.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/
                   {"returned non-boolean type 'int'"});
}

TEST_F(PythonEngineIsolatedTest, InvokeProduce_WrongReturnString_IsError)
{
    auto w = SpawnWorker(
        "python_engine.invoke_produce_wrong_return_string_is_error",
        {unique_dir("produce_wrong_str")});
    // Same diagnostic as WrongReturnType but with 'str'.  Covers the
    // truthy-string-but-not-bool case — a truthiness-based check would
    // incorrectly yield Commit.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/
                   {"returned non-boolean type 'str'"});
}

// NEW gap-fill — pins the "engine does NOT roll back Python writes to
// tx.slot on Discard" contract.  Mirrors Lua's
// InvokeProduce_DiscardOnFalse_ButLuaWroteSlot.
TEST_F(PythonEngineIsolatedTest,
       InvokeProduce_DiscardOnFalse_ButPythonWroteSlot)
{
    auto w = SpawnWorker(
        "python_engine.invoke_produce_discard_on_false_but_python_wrote_slot",
        {unique_dir("produce_discard_wrote")});
    ExpectWorkerOk(w);
}

// ============================================================================
// Chunk 3 — invoke_consume (Pattern 3)
//
// V2 had 3 consume tests (ReceivesReadOnlySlot, NoneSlot,
// ScriptErrorDetected).  Renames ReceivesReadOnlySlot to ReceivesSlot
// because V2's body never actually attempted a write — the
// "read-only" claim was untested.  Adds a dedicated RxSlot_IsReadOnly
// (gap-fill, mirrors Lua) that does attempt the write and pins the
// four observable channels (buf unchanged, result==Error,
// script_error_count==1, parent-level ERROR log fragment).
// ============================================================================

TEST_F(PythonEngineIsolatedTest, InvokeConsume_ReceivesSlot)
{
    auto w = SpawnWorker("python_engine.invoke_consume_receives_slot",
                         {unique_dir("consume_receives")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, InvokeConsume_NoneSlot)
{
    auto w = SpawnWorker("python_engine.invoke_consume_none_slot",
                         {unique_dir("consume_none_slot")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, InvokeConsume_ScriptErrorDetected)
{
    auto w = SpawnWorker("python_engine.invoke_consume_script_error_detected",
                         {unique_dir("consume_script_error")});
    // on_python_error_ logs ERROR "<tag> on_consume error: <exc msg>"
    // (python_engine.cpp).  Pin the RuntimeError text.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/{"consume error"});
}

// NEW gap-fill — pins the rx-read-only contract.  Mirrors Lua's
// InvokeConsume_RxSlot_IsReadOnly.  See worker docblock for the
// source-traced flow (register_slot_type → wrap_as_readonly_ctypes →
// __setattr__ override → AttributeError → on_python_error_).
TEST_F(PythonEngineIsolatedTest, InvokeConsume_RxSlot_IsReadOnly)
{
    auto w = SpawnWorker("python_engine.invoke_consume_rx_slot_is_read_only",
                         {unique_dir("consume_readonly")});
    // The AttributeError message from wrap_as_readonly_ctypes
    // (python_helpers.hpp) is "read-only slot: field
    // '<name>' cannot be written (in_slot is a zero-copy SHM view
    // -- use out_slot to write)".  The engine's catch path logs
    // this via on_python_error_.  Pin a stable substring so a
    // reworded error message is caught at the parent level.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/{"read-only slot"});
}

// ============================================================================
// Chunk 4 — invoke_process (Pattern 3)
//
// Processor dual-slot callback.  Renamed V2 tests to the semantic
// names used by Lua chunk 4 (None vs Nil; both_slots vs NilInput;
// rx_present_tx_none vs InputOnlyNoOutput) so the cross-engine
// contract is explicit.  Adds the rx-read-only gap-fill in the
// processor dual-slot path.  See worker docblocks for
// strengthening rationale.
// ============================================================================

TEST_F(PythonEngineIsolatedTest, InvokeProcess_DualSlots)
{
    auto w = SpawnWorker("python_engine.invoke_process_dual_slots",
                         {unique_dir("process_dual")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, InvokeProcess_BothSlotsNone)
{
    auto w = SpawnWorker("python_engine.invoke_process_both_slots_none",
                         {unique_dir("process_both_none")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, InvokeProcess_RxPresent_TxNone)
{
    auto w = SpawnWorker("python_engine.invoke_process_rx_present_tx_none",
                         {unique_dir("process_rx_only")});
    ExpectWorkerOk(w);
}

// NEW gap-fill — rx-read-only contract in the processor dual-slot
// path.  Mirrors Lua's InvokeProcess_RxSlot_IsReadOnly and the
// Python consumer-path InvokeConsume_RxSlot_IsReadOnly added in
// chunk 3.
TEST_F(PythonEngineIsolatedTest, InvokeProcess_RxSlot_IsReadOnly)
{
    auto w = SpawnWorker("python_engine.invoke_process_rx_slot_is_read_only",
                         {unique_dir("process_readonly")});
    // Same AttributeError message as the consumer path — see
    // python_helpers.hpp for wording.  Pin the "read-only
    // slot" fragment at parent level.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/{"read-only slot"});
}

// ============================================================================
// Chunk 5 — Messages (Pattern 3)
//
// Two projection paths: build_messages_list_ (producer/processor)
// and build_messages_list_bare_ (consumer).  See worker docblocks
// for the full contract and Python-vs-Lua shape divergence notes.
//
// V2 had 2 basic-shape tests; strengthened here + 2 new tests to
// cover the details-map promotion and the producer tuple-shape
// paths V2 left untested.  (InvokeProduce_EmptyMessagesList at V2
// line 1846 is also folded in.)
// ============================================================================

TEST_F(PythonEngineIsolatedTest, InvokeProduce_ReceivesMessages_EventWithDetails)
{
    auto w = SpawnWorker(
        "python_engine.invoke_produce_receives_messages_event_with_details",
        {unique_dir("msg_event_details")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, InvokeProduce_ReceivesMessages_EmptyList)
{
    auto w = SpawnWorker(
        "python_engine.invoke_produce_receives_messages_empty_list",
        {unique_dir("msg_empty")});
    ExpectWorkerOk(w);
}

// NEW gap-fill — pins Python's producer data-message 2-tuple shape
// (sender_hex_str, bytes).  V2 never tested this path, leaving the
// sender-hex formatting and bytes construction code untested.
TEST_F(PythonEngineIsolatedTest, InvokeProduce_ReceivesMessages_DataMessage)
{
    auto w = SpawnWorker(
        "python_engine.invoke_produce_receives_messages_data_message",
        {unique_dir("msg_data_tuple")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, InvokeConsume_ReceivesMessages_DataBareFormat)
{
    auto w = SpawnWorker(
        "python_engine.invoke_consume_receives_messages_data_bare_format",
        {unique_dir("msg_consumer_bare")});
    ExpectWorkerOk(w);
}

// ============================================================================
// Chunk 6 — API closures (Pattern 3)
//
// 5 P3 tests replace 6 V2 tests (the two stop_reason variants are
// merged into one exhaustive enum coverage that also adds HubDead
// — a real gap in V2).  See worker docblocks for strengthening
// rationale and cross-engine parity notes.
// ============================================================================

TEST_F(PythonEngineIsolatedTest, ApiVersionInfo_ReturnsJsonString)
{
    auto w = SpawnWorker("python_engine.api_version_info_returns_json_string",
                         {unique_dir("api_version_info")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, WrongRoleModuleImport_RaisesError)
{
    auto w = SpawnWorker("python_engine.wrong_role_module_import_raises_error",
                         {unique_dir("wrong_role_import")});
    // Two eval() calls produce two ERROR lines via on_python_error_
    // (python_engine.cpp).  The harness pairs substrings to
    // ERROR lines 1-to-1 in stderr order (test_process_utils.cpp:
    // 659-678), so the list must have exactly one entry per ERROR
    // line, each unique enough to identify that line.  Using the
    // blocked role-name strings gives us unambiguous per-line
    // identification.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/
                   {"pylabhub_consumer", "pylabhub_processor"});
}

TEST_F(PythonEngineIsolatedTest, ApiStop_SetsShutdownRequested)
{
    auto w = SpawnWorker("python_engine.api_stop_sets_shutdown_requested",
                         {unique_dir("api_stop")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, ApiCriticalError_SetAndReadAndStopReason)
{
    auto w = SpawnWorker(
        "python_engine.api_critical_error_set_and_read_and_stop_reason",
        {unique_dir("api_crit")});
    ExpectWorkerOk(w);
}

// Strengthened: exhaustive coverage of all 4 StopReason enum values
// (Normal, PeerDead, HubDead, CriticalError).  HubDead was never
// tested in V2 — clear gap.  Mirrors Lua chunk 6's same
// strengthening.
TEST_F(PythonEngineIsolatedTest, ApiStopReason_ReflectsAllEnumValues)
{
    auto w = SpawnWorker(
        "python_engine.api_stop_reason_reflects_all_enum_values",
        {unique_dir("api_stop_reason_all")});
    ExpectWorkerOk(w);
}

// ============================================================================
// Chunk 7 — Metrics + error accumulation (Pattern 3)
//
// 7 P3 tests replace 4 V2 tests (5 after moving Metrics_AllLoopFields
// from its separate section).  Strengthening includes:
//   - before/after transition coverage on individual accessors
//   - cross-link between api.metrics() and engine.script_error_count()
//   - full-shape inventory of the metrics dict at L2 scope
//   - the 5th loop field acquire_retry_count that V2 missed
//   - pinning stop_on_script_error is distinct from critical_error
// ============================================================================

TEST_F(PythonEngineIsolatedTest, Metrics_IndividualAccessors_ReadCoreCounters_Live)
{
    auto w = SpawnWorker(
        "python_engine.metrics_individual_accessors_read_core_counters_live",
        {unique_dir("metrics_indiv_live")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, Metrics_InSlotsReceived_Works_Consumer)
{
    auto w = SpawnWorker(
        "python_engine.metrics_in_slots_received_works_consumer",
        {unique_dir("metrics_inrx_consumer")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, MultipleErrors_CountAccumulates)
{
    auto w = SpawnWorker(
        "python_engine.multiple_errors_count_accumulates",
        {unique_dir("multi_errors_accum")});
    // 5 raised RuntimeErrors via on_python_error_ → 5 ERROR lines.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/
                   {"oops", "oops", "oops", "oops", "oops"});
}

TEST_F(PythonEngineIsolatedTest, StopOnScriptError_SetsShutdownOnError)
{
    auto w = SpawnWorker(
        "python_engine.stop_on_script_error_sets_shutdown_on_error",
        {unique_dir("stop_on_script_err")});
    // Raised RuntimeError routed through on_python_error_ →
    // one "intentional error" ERROR line.  Followed by an additional
    // "stop_on_script_error: requesting shutdown after on_produce
    // error" ERROR line when the stop_on_script_error_ flag is on
    // (python_engine.cpp on_python_error_ branch).
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/
                   {"intentional error",
                    "stop_on_script_error: requesting shutdown"});
}

TEST_F(PythonEngineIsolatedTest, Metrics_AllLoopFields_AnchoredValues)
{
    auto w = SpawnWorker(
        "python_engine.metrics_all_loop_fields_anchored_values",
        {unique_dir("metrics_all_loop_anchored")});
    ExpectWorkerOk(w);
}

// NEW gap-fill — L2 metrics top-level group inventory.  V2 never
// pinned the shape; a regression adding a phantom group or
// reshuffling the role sub-dict would slip through all V2 tests.
TEST_F(PythonEngineIsolatedTest, Metrics_HierarchicalTable_Producer_FullShape)
{
    auto w = SpawnWorker(
        "python_engine.metrics_hierarchical_table_producer_full_shape",
        {unique_dir("metrics_ht_prod_full")});
    ExpectWorkerOk(w);
}

// NEW gap-fill — cross-link the engine.script_error_count() and
// api.metrics()["role"]["script_error_count"] surfaces.  A regression
// where one surface reads a different (e.g. stale) counter would
// slip through individual-accessor tests.
TEST_F(PythonEngineIsolatedTest, Metrics_RoleScriptErrorCount_ReflectsRaisedError)
{
    auto w = SpawnWorker(
        "python_engine.metrics_role_script_error_count_reflects_raised_error",
        {unique_dir("metrics_serc_reflects")});
    // 2 seed phases raise "seed phase N".
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/
                   {"seed phase 1", "seed phase 2"});
}

// ============================================================================
// Chunk 8 — Load script + script errors (Pattern 3)
//
// 8 P3 tests convert 8 V2 tests + fix a silently-broken V2 test
// (RegisterSlotType_BadFieldType was passing via the wrong branch
// after the canonical-name tightening).  Strengthening:
//   - parent-level ERROR-log pinning for load_script paths
//   - EXPECT_EQ (not GE) on script_error_count for callback errors
//   - has_callback: pin non-canonical name behaviour (Python's
//     probe-arbitrary-attributes contract, resolved no-action
//     2026-04-20 per TESTING_TODO Finding #14)
// ============================================================================

TEST_F(PythonEngineIsolatedTest, LoadScript_MissingFile)
{
    auto w = SpawnWorker("python_engine.load_script_missing_file",
                         {unique_dir("load_missing")});
    // Python's load_script catch path logs ERROR "Failed to load
    // script from '<dir>': <exc>" at python_engine.cpp.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/
                   {"Failed to load script"});
}

TEST_F(PythonEngineIsolatedTest, LoadScript_MissingRequiredCallback)
{
    auto w = SpawnWorker(
        "python_engine.load_script_missing_required_callback",
        {unique_dir("load_missing_cb")});
    // python_engine.cpp: "Script has no 'on_produce' function".
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/
                   {"Script has no 'on_produce' function"});
}

// IMPORTANT: V2's RegisterSlotType_BadFieldType used "BadFrame" (non-
// canonical) and silently short-circuited on the canonical-name
// check after the 2026-04-20 tightening — passing via the WRONG
// branch.  This body uses "OutSlotFrame" (canonical) with a bad
// field type to actually exercise the build_ctypes_type_ exception
// path at python_engine.cpp.
TEST_F(PythonEngineIsolatedTest, RegisterSlotType_BadFieldType)
{
    auto w = SpawnWorker("python_engine.register_slot_type_bad_field_type",
                         {unique_dir("reg_bad_field")});
    // ERROR "register_slot_type('OutSlotFrame') failed: <exc>".
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/
                   {"register_slot_type('OutSlotFrame') failed"});
}

TEST_F(PythonEngineIsolatedTest, LoadScript_SyntaxError)
{
    auto w = SpawnWorker("python_engine.load_script_syntax_error",
                         {unique_dir("load_syntax")});
    // SyntaxError surfaces in the "Failed to load script" ERROR.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/
                   {"SyntaxError"});
}

TEST_F(PythonEngineIsolatedTest, HasCallback)
{
    auto w = SpawnWorker("python_engine.has_callback",
                         {unique_dir("has_cb")});
    ExpectWorkerOk(w);
}

// ============================================================================
// (V2 chunk-9 bodies — StatePersistsAcrossCalls,
// InvokeProduce_SlotOnly_NoFlexzoneOnInvoke, InvokeOnInbox_TypedData,
// TypeSizeof_InboxFrame_ReturnsCorrectSize,
// InvokeOnInbox_MissingType_ReportsError — migrated to Pattern 3,
// wrappers above in the chunk-9 section.  Cleanup 2026-04-20.
//
// Earlier V2 SupportsMultiState_ReturnsFalse duplicate was also removed
// at the same time (P3 replacement landed in chunk 1).)
// ============================================================================

// ============================================================================
// Chunk 10 — Generic invoke() + threading (migrated to Pattern 3)
//
// V2 bodies removed 2026-04-21; P3 workers at
// workers/python_engine_workers.cpp (chunk 10 section) cover the
// same behaviour with Python-specific strengthening: side-effect
// verification via eval(), queue-blocking pins, multi-pending
// finalize-cancellation, dual-path (owner + non-owner) after-finalize
// guards, and kwargs-expansion for both non-empty and empty args.
// ============================================================================

TEST_F(PythonEngineIsolatedTest, Invoke_ExistingFunction_ReturnsTrue)
{
    auto w = SpawnWorker("python_engine.invoke_existing_function_returns_true",
                         {unique_dir("invoke_existing")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, Invoke_NonExistentFunction_ReturnsFalse)
{
    auto w = SpawnWorker(
        "python_engine.invoke_non_existent_function_returns_false",
        {unique_dir("invoke_not_found")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, Invoke_EmptyName_ReturnsFalse)
{
    auto w = SpawnWorker("python_engine.invoke_empty_name_returns_false",
                         {unique_dir("invoke_empty")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, Invoke_ScriptError_ReturnsFalseAndIncrementsErrors)
{
    // The script raises RuntimeError("intentional test error"); the
    // engine logs the exception text via on_python_error_ at
    // python_engine.cpp:644.  Pinned at the parent level so a
    // regression that silently swallowed the log (but still
    // incremented the count) would still fail here.
    auto w = SpawnWorker(
        "python_engine.invoke_script_error_returns_false_and_increments_errors",
        {unique_dir("invoke_script_err")});
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/
                   {"intentional test error"});
}

TEST_F(PythonEngineIsolatedTest, Invoke_FromNonOwnerThread_Queued)
{
    auto w = SpawnWorker("python_engine.invoke_from_non_owner_thread_queued",
                         {unique_dir("invoke_non_owner")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, Invoke_FromNonOwnerThread_FinalizeUnblocks)
{
    auto w = SpawnWorker(
        "python_engine.invoke_from_non_owner_thread_finalize_unblocks",
        {unique_dir("invoke_finalize_unblock")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, Invoke_ConcurrentNonOwnerThreads)
{
    auto w = SpawnWorker("python_engine.invoke_concurrent_non_owner_threads",
                         {unique_dir("invoke_concurrent")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, Invoke_AfterFinalize_ReturnsFalse)
{
    auto w = SpawnWorker("python_engine.invoke_after_finalize_returns_false",
                         {unique_dir("invoke_after_finalize")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, Invoke_WithArgs_CallsFunction)
{
    auto w = SpawnWorker("python_engine.invoke_with_args_calls_function",
                         {unique_dir("invoke_with_args")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, Invoke_WithArgs_FromNonOwnerThread)
{
    auto w = SpawnWorker(
        "python_engine.invoke_with_args_from_non_owner_thread",
        {unique_dir("invoke_args_non_owner")});
    ExpectWorkerOk(w);
}

// ============================================================================
// Chunk 11 — Eval + shared data (migrated to Pattern 3)
//
// V2 bodies removed 2026-04-21; P3 workers at
// workers/python_engine_workers.cpp (chunk 11 section) cover the
// same behaviour with Python-specific strengthening: container
// conversion (list/dict) + None + float + module namespace
// reachability; 3 distinct eval error paths all ticking
// script_error_count by exactly 1; api.shared_data identity pinned
// by id() across callbacks plus on_stop persistence.
// ============================================================================

TEST_F(PythonEngineIsolatedTest, Eval_ReturnsScalarResult)
{
    auto w = SpawnWorker("python_engine.eval_returns_scalar_result",
                         {unique_dir("eval_scalar")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, Eval_ErrorReturnsEmpty)
{
    // Parent pins one distinctive substring per error type so the
    // engine's on_python_error_ translator (python_engine.cpp:693)
    // is producing Python-side exception names — not a generic
    // "eval failed" placeholder.
    auto w = SpawnWorker("python_engine.eval_error_returns_empty",
                         {unique_dir("eval_error")});
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/
                   {"NameError", "SyntaxError", "ZeroDivisionError"});
}

TEST_F(PythonEngineIsolatedTest, SharedData_PersistsAcrossCallbacks)
{
    auto w = SpawnWorker(
        "python_engine.shared_data_persists_across_callbacks",
        {unique_dir("shared_data")});
    ExpectWorkerOk(w);
}

// ============================================================================
// API parity tests — diagnostics, custom metrics, environment, queue-state
// ============================================================================

// ============================================================================
// Chunk 12 — API diagnostics + identity (migrated to Pattern 3)
//
// V2 bodies removed 2026-04-21; P3 workers at
// workers/python_engine_workers.cpp (chunk 12 section) strengthen the
// originals: live-read pins for core-backed accessors (loop_overrun,
// last_cycle_work_us, critical_error → second invoke sees LIVE state,
// not a snapshot cached at build_api time), exhaustive stop_reason
// enum coverage (all 4 values, not just CriticalError), full identity
// surface with role_dir-derived logs_dir/run_dir, and strict-1 error
// semantics for report_metrics type mismatch.
// ============================================================================

TEST_F(PythonEngineIsolatedTest, Api_LoopOverrunCount_ReadsFromCore)
{
    auto w = SpawnWorker("python_engine.api_loop_overrun_count_reads_from_core",
                         {unique_dir("api_overrun")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, Api_LastCycleWorkUs_ReadsFromCore)
{
    auto w = SpawnWorker("python_engine.api_last_cycle_work_us_reads_from_core",
                         {unique_dir("api_cycle_us")});
    ExpectWorkerOk(w);
}

// V2's Api_CriticalError_DefaultIsFalse removed — chunk 6's
// Api_CriticalError_SetAndReadAndStopReason covers the same transition
// (api.set_critical_error() → pybind11 → core::set_critical_error() is the
// same underlying path as the V2 test's core.set_critical_error() direct
// call), and chunk 6 also pins is_shutdown_requested() bundled-atomicity.

TEST_F(PythonEngineIsolatedTest, Api_IdentityAccessors_ReturnCorrectValues)
{
    auto w = SpawnWorker(
        "python_engine.api_identity_accessors_return_correct_values",
        {unique_dir("api_identity")});
    ExpectWorkerOk(w);
}

// ============================================================================
// Chunk 13 — Custom metrics (migrated to Pattern 3)
//
// V2 bodies removed 2026-04-21; P3 workers at
// workers/python_engine_workers.cpp (chunk 13 section) strengthen with
// C++-side core.custom_metrics_snapshot() cross-checks (pin the
// pybind11 → RoleHostCore round-trip, not just the Python-side accessor
// echoing its own storage), cross-invoke persistence, overwrite-not-
// accumulate semantics, and edge-value coverage (pos_zero, neg_zero,
// negative, tiny/large magnitudes).
// ============================================================================

TEST_F(PythonEngineIsolatedTest, Api_CustomMetrics_ReportAndReadInMetrics)
{
    auto w = SpawnWorker(
        "python_engine.api_custom_metrics_report_and_read_in_metrics",
        {unique_dir("api_cm_read")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, Api_CustomMetrics_BatchAndClear)
{
    auto w = SpawnWorker(
        "python_engine.api_custom_metrics_batch_and_clear",
        {unique_dir("api_cm_clear")});
    ExpectWorkerOk(w);
}

// ============================================================================
// Chunk 8 — on_init / on_stop / on_inbox script-error paths (Pattern 3)
//
// All three strengthened from V2's EXPECT_GE(count, 1u) → EXPECT_EQ
// (count, 1u).  Also pin the parent-level ERROR text for the
// raised RuntimeError so a reworded diagnostic is caught.
// ============================================================================

TEST_F(PythonEngineIsolatedTest, InvokeOnInit_ScriptError)
{
    auto w = SpawnWorker("python_engine.invoke_on_init_script_error",
                         {unique_dir("on_init_err")});
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/{"init exploded"});
}

TEST_F(PythonEngineIsolatedTest, InvokeOnStop_ScriptError)
{
    auto w = SpawnWorker("python_engine.invoke_on_stop_script_error",
                         {unique_dir("on_stop_err")});
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/{"stop exploded"});
}

TEST_F(PythonEngineIsolatedTest, InvokeOnInbox_ScriptError)
{
    auto w = SpawnWorker("python_engine.invoke_on_inbox_script_error",
                         {unique_dir("on_inbox_err")});
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/{"inbox exploded"});
}

// ============================================================================
// Chunk 9 — State + slot + inbox
// ============================================================================

TEST_F(PythonEngineIsolatedTest, StatePersistsAcrossCalls)
{
    auto w = SpawnWorker("python_engine.state_persists_across_calls",
                         {unique_dir("state_persists")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, InvokeProduce_SlotOnly_NoFlexzoneOnInvoke)
{
    auto w = SpawnWorker(
        "python_engine.invoke_produce_slot_only_no_flexzone_on_invoke",
        {unique_dir("slot_only_no_fz")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, InvokeOnInbox_TypedData)
{
    auto w = SpawnWorker("python_engine.invoke_on_inbox_typed_data",
                         {unique_dir("inbox_typed")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, TypeSizeof_InboxFrame_ReturnsCorrectSize)
{
    auto w = SpawnWorker(
        "python_engine.type_sizeof_inbox_frame_returns_correct_size",
        {unique_dir("inbox_sizeof")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, InvokeOnInbox_MissingType_ReportsError)
{
    // Parent pins the engine's documented error text
    // (python_engine.cpp: "InboxFrame type not registered") so a
    // regression that silently fails (no ERROR log) would be caught at
    // the ExpectWorkerOk layer even if script_error_count happened to
    // be exactly 1 for a different reason.
    auto w = SpawnWorker(
        "python_engine.invoke_on_inbox_missing_type_reports_error",
        {unique_dir("inbox_missing_type")});
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/
                   {"InboxFrame type not registered"});
}

// ============================================================================
// 16. Queue-state API defaults (no queue connected)
// ============================================================================

// ============================================================================
// Chunk 14 — Queue state / spinlock / channels / open_inbox / numpy
// (migrated to Pattern 3)
//
// V2 bodies removed 2026-04-21; P3 workers at
// workers/python_engine_workers.cpp (chunk 14 section) strengthen with
// type assertions (int/str), expanded arg-shape coverage for
// open_inbox (empty + unicode uids), ValueError-message pin for
// spinlock error path, and C++-side buffer-offset verification for
// the as_numpy round-trip.
//
// NOTE: V2's Api_Flexzone_WithoutSHM_ReturnsNone is not migrated —
// chunk 9's invoke_produce_slot_only_no_flexzone_on_invoke already
// covers api.flexzone() is None + api.flexzone(api.Tx) is None +
// api.update_flexzone_checksum() is False.
// ============================================================================

TEST_F(PythonEngineIsolatedTest, Api_ProducerQueueState_WithoutQueue)
{
    auto w = SpawnWorker(
        "python_engine.api_producer_queue_state_without_queue",
        {unique_dir("api_prod_qs")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, Api_ProcessorQueueState_DualDefaults)
{
    auto w = SpawnWorker(
        "python_engine.api_processor_queue_state_dual_defaults",
        {unique_dir("api_proc_qs")});
    ExpectWorkerOk(w);
}

// ============================================================================
// (V2 Metrics_AllLoopFields_Present removed — superseded by chunk 7's
// Metrics_AllLoopFields_AnchoredValues, which adds the 5th loop field
// acquire_retry_count that V2 missed.)
// ============================================================================

// ============================================================================
// 19. Custom metrics edge cases
// ============================================================================

TEST_F(PythonEngineIsolatedTest, Api_CustomMetrics_OverwriteSameKey)
{
    auto w = SpawnWorker(
        "python_engine.api_custom_metrics_overwrite_same_key",
        {unique_dir("api_cm_overwrite")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, Api_CustomMetrics_ZeroValue)
{
    auto w = SpawnWorker("python_engine.api_custom_metrics_zero_value",
                         {unique_dir("api_cm_zero")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, Api_CustomMetrics_ReportTypeErrors)
{
    // Error-path coverage: each report_metric / report_metrics type
    // mismatch triggers pybind11 TypeError caught in-script.  Also
    // pins side-effect containment (a rejected dict update must NOT
    // partially apply to the core snapshot).
    auto w = SpawnWorker(
        "python_engine.api_custom_metrics_report_type_errors",
        {unique_dir("api_cm_typerr")});
    ExpectWorkerOk(w);
}

// ============================================================================
// (V2 InvokeProduce_EmptyMessagesList removed — superseded by
// chunk 5's InvokeProduce_ReceivesMessages_EmptyList.)
// ============================================================================

// ============================================================================
// 21. stop_reason after critical error
// ============================================================================

// V2's Api_StopReason_AfterCriticalError was removed — chunk 6's
// Api_StopReason_ReflectsAllEnumValues already covers all four
// StopReason values (Normal, PeerDead, HubDead, CriticalError),
// and chunk 6's Api_CriticalError_SetAndReadAndStopReason covers
// the api.set_critical_error() bundled-atomicity path that V2's
// test name implied.

// ============================================================================
// 22. Processor channels (in_channel / out_channel)
// ============================================================================

TEST_F(PythonEngineIsolatedTest, Api_ProcessorChannels_InOut)
{
    auto w = SpawnWorker("python_engine.api_processor_channels_in_out",
                         {unique_dir("api_proc_ch")});
    ExpectWorkerOk(w);
}

// ============================================================================
// 23. open_inbox without broker returns None
// ============================================================================

TEST_F(PythonEngineIsolatedTest, Api_OpenInbox_WithoutBroker)
{
    auto w = SpawnWorker("python_engine.api_open_inbox_without_broker",
                         {unique_dir("api_open_inbox")});
    ExpectWorkerOk(w);
}

// ============================================================================
// 24. report_metrics with non-dict argument produces error
// ============================================================================

TEST_F(PythonEngineIsolatedTest, Api_ReportMetrics_NonDictArg_IsError)
{
    // Parent pins the pybind11 type-mismatch ERROR text — confirms
    // the error came from the type guard, not some cascading follow-on.
    auto w = SpawnWorker(
        "python_engine.api_report_metrics_non_dict_arg_is_error",
        {unique_dir("api_report_non_dict")});
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/{"report_metrics"});
}

// ============================================================================
// 25. Full lifecycle with verified callback execution
// ============================================================================

// ============================================================================
// Chunk 15 — FullLifecycle + FullStartup (migrated to Pattern 3)
//
// V2 bodies removed 2026-04-21; P3 workers at
// workers/python_engine_workers.cpp (chunk 15 section) strengthen with
// callback-order pins (init-before-stop via list), SlotFrame/FlexFrame
// alias size cross-checks against compute_schema_size, processor's
// explicit "SlotFrame is NOT an alias" property, and a real data-
// transform round-trip on the processor.
// ============================================================================

TEST_F(PythonEngineIsolatedTest, FullLifecycle_VerifiesCallbackExecution)
{
    auto w = SpawnWorker(
        "python_engine.full_lifecycle_verifies_callback_execution",
        {unique_dir("full_lifecycle")});
    ExpectWorkerOk(w);
}

// ============================================================================
// Parity tests — match Lua coverage
// ============================================================================

TEST_F(PythonEngineIsolatedTest, Api_ConsumerQueueState_WithoutQueue)
{
    auto w = SpawnWorker(
        "python_engine.api_consumer_queue_state_without_queue",
        {unique_dir("api_cons_qs")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, Api_EnvironmentStrings_LogsDirRunDir)
{
    auto w = SpawnWorker(
        "python_engine.api_environment_strings_logs_dir_run_dir",
        {unique_dir("api_env_strs")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, Api_SpinlockCount_WithoutSHM)
{
    auto w = SpawnWorker("python_engine.api_spinlock_count_without_shm",
                         {unique_dir("api_spinlock_count")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, Api_Spinlock_WithoutSHM_IsError)
{
    auto w = SpawnWorker("python_engine.api_spinlock_without_shm_is_error",
                         {unique_dir("api_spinlock_err")});
    ExpectWorkerOk(w);
}

// V2's Api_Flexzone_WithoutSHM_ReturnsNone removed — chunk 9's
// InvokeProduce_SlotOnly_NoFlexzoneOnInvoke already covers
// api.flexzone() is None + api.flexzone(api.Tx) is None +
// api.update_flexzone_checksum() is False.

TEST_F(PythonEngineIsolatedTest, Api_AsNumpy_ArrayField)
{
    auto w = SpawnWorker("python_engine.api_as_numpy_array_field",
                         {unique_dir("api_as_numpy_arr")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, Api_AsNumpy_NonArrayField_Throws)
{
    auto w = SpawnWorker(
        "python_engine.api_as_numpy_non_array_field_throws",
        {unique_dir("api_as_numpy_scalar")});
    ExpectWorkerOk(w);
}

// ============================================================================
// Full engine startup — tests the EngineModuleParams startup/shutdown path
// ============================================================================

TEST_F(PythonEngineIsolatedTest, FullStartup_Producer_SlotOnly)
{
    auto w = SpawnWorker("python_engine.full_startup_producer_slot_only",
                         {unique_dir("full_prod_slot")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, FullStartup_Producer_SlotAndFlexzone)
{
    auto w = SpawnWorker(
        "python_engine.full_startup_producer_slot_and_flexzone",
        {unique_dir("full_prod_slotfz")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, FullStartup_Consumer)
{
    auto w = SpawnWorker("python_engine.full_startup_consumer",
                         {unique_dir("full_cons")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, FullStartup_Processor)
{
    auto w = SpawnWorker("python_engine.full_startup_processor",
                         {unique_dir("full_proc")});
    ExpectWorkerOk(w);
}

// ============================================================================
// Schema logical size — complex schemas, aligned vs packed, slot + flexzone
// ============================================================================

// ============================================================================
// Chunk 16 — SlotLogicalSize + FlexzoneLogicalSize + multifield + band
// (migrated to Pattern 3)
//
// V2 bodies removed 2026-04-21; P3 workers at
// workers/python_engine_workers.cpp (chunk 16 section) strengthen with
// live-read pins (two-invoke for aligned slot_logical_size), slot-vs-fz
// distinct-size assertion, pad-byte corruption guards on multifield
// producer/processor tests, and a scoped try/except around each band_*
// method so regressions are attributed to the right call.
// ============================================================================

TEST_F(PythonEngineIsolatedTest, SlotLogicalSize_Aligned_PaddingSensitive)
{
    auto w = SpawnWorker(
        "python_engine.slot_logical_size_aligned_padding_sensitive",
        {unique_dir("slot_ls_aligned")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, SlotLogicalSize_Packed_NoPadding)
{
    auto w = SpawnWorker("python_engine.slot_logical_size_packed_no_padding",
                         {unique_dir("slot_ls_packed")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, SlotLogicalSize_ComplexMixed_Aligned)
{
    auto w = SpawnWorker(
        "python_engine.slot_logical_size_complex_mixed_aligned",
        {unique_dir("slot_ls_complex")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, FlexzoneLogicalSize_ArrayFields)
{
    auto w = SpawnWorker("python_engine.flexzone_logical_size_array_fields",
                         {unique_dir("fz_ls_array")});
    ExpectWorkerOk(w);
}

// ============================================================================
// FullStartup — multifield schema data round-trip (all three roles)
// ============================================================================

TEST_F(PythonEngineIsolatedTest, FullStartup_Producer_Multifield)
{
    auto w = SpawnWorker("python_engine.full_startup_producer_multifield",
                         {unique_dir("full_prod_mf")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, FullStartup_Consumer_Multifield)
{
    auto w = SpawnWorker("python_engine.full_startup_consumer_multifield",
                         {unique_dir("full_cons_mf")});
    ExpectWorkerOk(w);
}

TEST_F(PythonEngineIsolatedTest, FullStartup_Processor_Multifield)
{
    auto w = SpawnWorker("python_engine.full_startup_processor_multifield",
                         {unique_dir("full_proc_mf")});
    ExpectWorkerOk(w);
}

// ============================================================================
// Band pub/sub API — L2 (no broker; the 4 api methods must be callable
// and return gracefully: None / False / no-op without any broker attached).
// ============================================================================

TEST_F(PythonEngineIsolatedTest, Api_Band_AllMethodsGraceful_NoBroker)
{
    auto w = SpawnWorker(
        "python_engine.api_band_all_methods_graceful_no_broker",
        {unique_dir("api_band_no_broker")});
    ExpectWorkerOk(w);
}
