/**
 * @file test_python_engine.cpp
 * @brief Unit tests for PythonEngine -- the ScriptEngine implementation for CPython.
 *
 * Tests the engine in isolation: no broker, no queues, no role host.
 * Uses raw memory buffers and temporary Python script files.
 *
 * Mirrors the LuaEngine test suite (test_lua_engine.cpp) with Python-specific
 * adjustments:
 *   - Scripts are Python (__init__.py in script/python/ package)
 *   - Return values: True/False/None instead of true/false/nil
 *   - Messages: dicts for events, tuples (sender_hex, bytes) for data (producer)
 *   - Consumer messages: bare bytes for data
 *   - supports_multi_state() returns false
 *   - create_thread_state() returns nullptr
 *
 * Covers:
 *   1. Lifecycle: initialize -> load -> register -> build_api -> invoke -> finalize
 *   2. Type registration: ctypes sizeof validation
 *   3. invoke_produce: commit, discard, None slot, error paths
 *   4. invoke_consume: read-only slot, no return value, error detection
 *   5. invoke_process: dual slots, nil combinations
 *   6. Messages: dict format for producer/consumer
 *   7. API closures: version_info, stop, set_critical_error, stop_reason
 *   8. Error handling: script_error_count, stop_on_script_error
 *   9. Metrics closures from RoleHostCore
 *  10. create_thread_state: returns nullptr
 */
#include <gtest/gtest.h>

#include "python_engine.hpp"
#include "utils/engine_module_params.hpp"
#include "utils/role_host_core.hpp"

#include "utils/schema_utils.hpp"
#include "test_patterns.h"     // Pattern 3 (IsolatedProcessTest) base
#include "test_schema_helpers.h"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using pylabhub::scripting::PythonEngine;
using pylabhub::scripting::ScriptEngine;
using pylabhub::scripting::InvokeResult;
using pylabhub::scripting::InvokeStatus;
using pylabhub::scripting::InvokeResponse;
using pylabhub::scripting::RoleAPIBase;
using pylabhub::scripting::RoleHostCore;
using pylabhub::scripting::IncomingMessage;
using pylabhub::hub::SchemaSpec;
using pylabhub::hub::FieldDef;
using pylabhub::scripting::InvokeRx;
using pylabhub::scripting::InvokeTx;
using pylabhub::scripting::InvokeInbox;

using pylabhub::tests::simple_schema;
using pylabhub::tests::padding_schema;
using pylabhub::tests::complex_mixed_schema;
using pylabhub::tests::fz_array_schema;
using pylabhub::tests::multifield_schema;

namespace
{

// ============================================================================
// Test fixture
// ============================================================================

class PythonEngineTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        tmp_ = fs::temp_directory_path() /
               ("python_engine_test_" +
                std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(tmp_ / "script" / "python");
    }

    void TearDown() override
    {
        fs::remove_all(tmp_);
    }

    /// Write a Python script to <tmp>/script/python/__init__.py
    void write_script(const std::string &content)
    {
        std::ofstream f(tmp_ / "script" / "python" / "__init__.py");
        f << content;
    }

    RoleHostCore default_core_; ///< Default core for tests that don't provide one.

    /// Build a minimal RoleAPIBase with role-appropriate identity.
    std::unique_ptr<RoleAPIBase> make_api(RoleHostCore &core,
                                          const std::string &tag = "prod")
    {
        // role_tag + uid required at ctor time.
        std::string uid;
        if      (tag == "prod") uid = "PROD-TestEngine-00000001";
        else if (tag == "cons") uid = "CONS-TestEngine-00000001";
        else if (tag == "proc") uid = "PROC-TestEngine-00000001";
        else                    uid = "TEST-" + tag + "-00000001";
        auto api = std::make_unique<RoleAPIBase>(core, tag, uid);
        api->set_name("TestEngine");
        api->set_channel("test.channel");
        api->set_log_level("error");
        api->set_stop_on_script_error(false);
        return api;
    }

    /// Initialize engine, load script, register type, build API.
    bool setup_engine(PythonEngine &engine,
                      const std::string &required_cb = "on_produce")
    {
        return setup_engine_with_core(engine, default_core_, required_cb);
    }

    /// Setup engine with a specific RoleHostCore.
    bool setup_engine_with_core(PythonEngine &engine, RoleHostCore &core,
                                const std::string &required_cb = "on_produce")
    {
        engine.set_python_venv("");
        if (!engine.initialize("test", &core))
            return false;
        if (!engine.load_script(tmp_ / "script" / "python",
                                "__init__.py", required_cb.c_str()))
            return false;

        auto spec = simple_schema();
        if (!engine.register_slot_type(spec, "OutSlotFrame", "aligned"))
            return false;

        test_api_ = make_api(core);
        return engine.build_api(*test_api_);
    }

    fs::path tmp_;
    std::unique_ptr<RoleAPIBase> test_api_;  ///< Held alive for engine's lifetime.
};

// ============================================================================
// Chunk 1 — Lifecycle / Type registration / Alias creation (Pattern 3)
//
// These tests used to live under the V2 PythonEngineTest fixture above;
// they are being migrated to Pattern 3 chunk-by-chunk to mirror the
// completed Lua sweep.  Each spawns a subprocess where the engine is
// constructed and driven; bodies live in workers/python_engine_workers.cpp.
//
// The V2 fixture wrapper stays until its last test is converted; the
// final commit of the sweep removes it along with unused includes.
// ============================================================================

namespace
{

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
    // cross-validation (python_engine.cpp:800-807) catches any
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
    // (python_engine.cpp:787-789). That log is expected.
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
//     in this session — python_engine.cpp:1190; was previously WARN
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
    // (python_engine.cpp:1190-1192).  A reworded diagnostic that
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
    // (python_engine.cpp:1222).  Pin the RuntimeError text.
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
    // (python_engine.cpp:1222).  Pin the RuntimeError text.
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
    // (python_helpers.hpp:101-104) is "read-only slot: field
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
    // python_helpers.hpp:101-104 for wording.  Pin the "read-only
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
    // (python_engine.cpp:1198).  The harness pairs substrings to
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
// 8. Metrics closures read from RoleHostCore counters
// ============================================================================

TEST_F(PythonEngineTest, MetricsClosures_ReadFromRoleHostCounters)
{
    // Python API uses out_slots_written() / out_drop_count() (not out_written/drops).
    write_script(
        "def on_produce(tx, msgs, api):\n"
        "    ow = api.out_slots_written()\n"
        "    dr = api.out_drop_count()\n"
        "    assert ow == 42, f'expected out_slots_written=42, got {ow}'\n"
        "    assert dr == 7, f'expected out_drop_count=7, got {dr}'\n"
        "    return False\n");

    RoleHostCore core;
    core.test_set_out_slots_written(42);
    core.test_set_out_drop_count(7);

    PythonEngine engine;
    ASSERT_TRUE(setup_engine_with_core(engine, core));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce({&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "Script assertion failed -- metrics values incorrect";

    engine.finalize();
}

TEST_F(PythonEngineTest, MetricsClosures_InReceivedWorks)
{
    // Python API uses in_slots_received() (not in_received).
    // Consumer role: ctx.consumer must be non-null, ctx.producer null.
    write_script(
        "def on_consume(rx, msgs, api):\n"
        "    ir = api.in_slots_received()\n"
        "    assert ir == 15, f'expected in_slots_received=15, got {ir}'\n"
        "    return True\n");

    RoleHostCore core;
    core.test_set_in_slots_received(15);

    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &core));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_consume"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "InSlotFrame", "aligned"));

    auto test_api = make_api(core, "cons");
    ASSERT_TRUE(engine.build_api(*test_api));

    std::vector<IncomingMessage> msgs;
    engine.invoke_consume(InvokeRx{nullptr, 0}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "Script assertion failed -- in_received value incorrect";

    engine.finalize();
}

TEST_F(PythonEngineTest, MultipleErrors_CountAccumulates)
{
    write_script(
        "def on_produce(tx, msgs, api):\n"
        "    raise RuntimeError('oops')\n");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;

    for (int i = 0; i < 5; ++i)
    {
        engine.invoke_produce({&buf, sizeof(buf)}, msgs);
    }
    EXPECT_EQ(engine.script_error_count(), 5u);

    engine.finalize();
}

// ============================================================================
// 9. Error handling
// ============================================================================

TEST_F(PythonEngineTest, StopOnScriptError_SetsShutdownOnError)
{
    write_script(
        "def on_produce(tx, msgs, api):\n"
        "    raise RuntimeError('intentional error')\n");

    RoleHostCore core;
    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &core));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_produce"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame", "aligned"));

    auto test_api = make_api(core);
    test_api->set_stop_on_script_error(true);
    ASSERT_TRUE(engine.build_api(*test_api));

    EXPECT_FALSE(core.is_shutdown_requested());

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    auto result =
        engine.invoke_produce({&buf, sizeof(buf)}, msgs);

    EXPECT_EQ(result, InvokeResult::Error);
    EXPECT_TRUE(core.is_shutdown_requested())
        << "stop_on_script_error should set shutdown_requested on error";

    engine.finalize();
}

TEST_F(PythonEngineTest, LoadScript_MissingFile)
{
    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    // Point to a nonexistent directory.
    EXPECT_FALSE(
        engine.load_script(tmp_ / "nonexistent" / "path", "__init__.py", "on_produce"));
    engine.finalize();
}

TEST_F(PythonEngineTest, LoadScript_MissingRequiredCallback)
{
    write_script(
        "def on_init(api):\n"
        "    pass\n"
        "# on_produce intentionally absent\n");

    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    EXPECT_FALSE(engine.load_script(tmp_ / "script" / "python",
                                    "__init__.py", "on_produce"));
    engine.finalize();
}

TEST_F(PythonEngineTest, RegisterSlotType_BadFieldType)
{
    write_script("def on_produce(tx, msgs, api):\n    return True\n");

    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_produce"));

    SchemaSpec bad_spec;
    bad_spec.has_schema = true;
    FieldDef f;
    f.name     = "x";
    f.type_str = "complex128"; // unsupported type
    f.count    = 1;
    f.length   = 0;
    bad_spec.fields.push_back(f);

    EXPECT_FALSE(engine.register_slot_type(bad_spec, "BadFrame", "aligned"));
    engine.finalize();
}

TEST_F(PythonEngineTest, LoadScript_SyntaxError)
{
    // Write a script with a Python syntax error.
    write_script("def on_produce(tx, msgs, api)\n"  // missing colon
                 "    return True\n");

    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    EXPECT_FALSE(engine.load_script(tmp_ / "script" / "python",
                                    "__init__.py", "on_produce"));
    engine.finalize();
}

// ============================================================================
// 10. has_callback
// ============================================================================

TEST_F(PythonEngineTest, HasCallback)
{
    write_script(
        "def on_produce(tx, msgs, api):\n"
        "    return True\n"
        "\n"
        "def on_init(api):\n"
        "    pass\n"
        "# on_stop intentionally absent\n");

    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_produce"));

    EXPECT_TRUE(engine.has_callback("on_produce"));
    EXPECT_TRUE(engine.has_callback("on_init"));
    EXPECT_FALSE(engine.has_callback("on_stop"));
    EXPECT_FALSE(engine.has_callback("on_consume"));

    engine.finalize();
}

// ============================================================================
// 11. State persistence across calls
// ============================================================================

TEST_F(PythonEngineTest, StatePersistsAcrossCalls)
{
    // Module-level counter persists across invocations.
    write_script(
        "call_count = 0\n"
        "\n"
        "def on_produce(tx, msgs, api):\n"
        "    global call_count\n"
        "    call_count += 1\n"
        "    if tx.slot is not None:\n"
        "        tx.slot.value = float(call_count)\n"
        "    return True\n");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    std::vector<IncomingMessage> msgs;

    // Call 3 times -- counter should increment.
    float buf1 = 0.0f;
    engine.invoke_produce(InvokeTx{&buf1, sizeof(buf1)}, msgs);
    EXPECT_FLOAT_EQ(buf1, 1.0f);

    float buf2 = 0.0f;
    engine.invoke_produce(InvokeTx{&buf2, sizeof(buf2)}, msgs);
    EXPECT_FLOAT_EQ(buf2, 2.0f);

    float buf3 = 0.0f;
    engine.invoke_produce(InvokeTx{&buf3, sizeof(buf3)}, msgs);
    EXPECT_FLOAT_EQ(buf3, 3.0f);

    EXPECT_EQ(engine.script_error_count(), 0u);

    engine.finalize();
}

// ============================================================================
// 12. Flexzone
// ============================================================================

TEST_F(PythonEngineTest, InvokeProduce_SlotOnly_NoFlexzoneOnInvoke)
{
    // Post-L3.ζ: flexzone is accessed via api.flexzone(side), not via tx.fz.
    // This test verifies that slot writing works when no fz is on the invoke struct.
    write_script(
        "def on_produce(tx, msgs, api):\n"
        "    assert tx.slot is not None, 'expected slot'\n"
        "    tx.slot.value = 10.0\n"
        "    return True\n");

    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_produce"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame", "aligned"));

    auto test_api = make_api(default_core_);
    ASSERT_TRUE(engine.build_api(*test_api));

    float slot_buf = 0.0f;
    std::vector<IncomingMessage> msgs;

    auto result = engine.invoke_produce(
        InvokeTx{&slot_buf, sizeof(slot_buf)}, msgs);
    EXPECT_EQ(result, InvokeResult::Commit);
    EXPECT_FLOAT_EQ(slot_buf, 10.0f);

    engine.finalize();
}

// ============================================================================
// 13. invoke_on_inbox
// ============================================================================

TEST_F(PythonEngineTest, InvokeOnInbox_TypedData)
{
    // Script receives a typed ctypes struct (from_buffer_copy) and sender UID.
    write_script(
        "def on_produce(tx, msgs, api):\n"
        "    return False\n"
        "\n"
        "def on_inbox(msg, api):\n"
        "    assert msg.data is not None, 'expected inbox data'\n"
        "    assert hasattr(msg.data, 'value'), f'expected typed struct, got {type(msg.data)}'\n"
        "    assert abs(msg.data.value - 77.0) < 0.01, (\n"
        "        f'expected value ~77.0, got {msg.data.value}')\n"
        "    assert msg.sender_uid == 'PROD-SENDER-00000001', (\n"
        "        f'expected sender UID, got {msg.sender_uid}')\n"
        "    return True\n");

    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_produce"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame", "aligned"));
    ASSERT_TRUE(engine.register_slot_type(spec, "InboxFrame", "aligned"));

    auto test_api = make_api(default_core_);
    ASSERT_TRUE(engine.build_api(*test_api));

    float inbox_data = 77.0f;
    engine.invoke_on_inbox({&inbox_data, sizeof(inbox_data), "PROD-SENDER-00000001", 1});
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "Script assertion failed -- inbox data or sender incorrect";

    engine.finalize();
}

TEST_F(PythonEngineTest, TypeSizeof_InboxFrame_ReturnsCorrectSize)
{
    // type_sizeof("InboxFrame") must return the correct struct size after registration.
    // This is used by role hosts to validate against InboxQueue::item_size().
    // Use a multi-type schema that exercises alignment padding:
    //   uint8 flag (1) + pad(3) + float64 value (8) + uint16 count (2) +
    //   pad(2) + int32 status (4) + char[5] label (5) + pad(3) = 28 bytes aligned
    write_script(
        "def on_produce(tx, msgs, api):\n"
        "    return False\n");

    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_produce"));

    SchemaSpec spec;
    spec.has_schema = true;
    spec.fields.push_back({"flag",   "uint8",   1, 0});
    spec.fields.push_back({"value",  "float64", 1, 0});
    spec.fields.push_back({"count",  "uint16",  1, 0});
    spec.fields.push_back({"status", "int32",   1, 0});
    spec.fields.push_back({"label",  "string",  1, 5});

    ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame", "aligned"));
    ASSERT_TRUE(engine.register_slot_type(spec, "InboxFrame", "aligned"));

    auto test_api = make_api(default_core_);
    ASSERT_TRUE(engine.build_api(*test_api));

    size_t slot_sz  = engine.type_sizeof("OutSlotFrame");
    size_t inbox_sz = engine.type_sizeof("InboxFrame");
    EXPECT_GT(slot_sz, 0u) << "OutSlotFrame size must be > 0";
    EXPECT_GT(inbox_sz, 0u) << "InboxFrame size must be > 0";
    EXPECT_EQ(slot_sz, inbox_sz) << "Same schema → same size";
    // Aligned: 1 + 7pad + 8 + 2 + 2pad + 4 + 5 + 3pad = 32
    EXPECT_EQ(inbox_sz, 32u) << "Expected 28 bytes with alignment padding";

    engine.finalize();
}

TEST_F(PythonEngineTest, InvokeOnInbox_MissingType_ReportsError)
{
    // If InboxFrame is not registered, invoke_on_inbox must report an error.
    write_script(
        "def on_produce(tx, msgs, api):\n"
        "    return False\n"
        "\n"
        "def on_inbox(msg, api):\n"
        "    return True\n");

    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_produce"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame", "aligned"));
    // Deliberately NOT registering "InboxFrame".

    auto test_api = make_api(default_core_);
    ASSERT_TRUE(engine.build_api(*test_api));

    float raw = 1.0f;
    engine.invoke_on_inbox({&raw, sizeof(raw), "CONS-SENDER-00000001", 1});
    EXPECT_GE(engine.script_error_count(), 1u)
        << "Missing InboxFrame type should increment error count";

    engine.finalize();
}

// ============================================================================
// (V2 SupportsMultiState_ReturnsFalse stray duplicate removed — the P3
// replacement landed in chunk 1 but the V2 body was not deleted at the
// time.  Cleanup 2026-04-20.)
// ============================================================================

// ============================================================================
// Generic invoke() tests
// ============================================================================

TEST_F(PythonEngineTest, Invoke_ExistingFunction_ReturnsTrue)
{
    write_script(R"(
def on_produce(tx, msgs, api):
    return True

def on_heartbeat():
    pass
)");
    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    EXPECT_TRUE(engine.invoke("on_heartbeat"));
    engine.finalize();
}

TEST_F(PythonEngineTest, Invoke_NonExistentFunction_ReturnsFalse)
{
    write_script("def on_produce(tx, msgs, api):\n    return True\n");
    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    EXPECT_FALSE(engine.invoke("no_such_function"));
    engine.finalize();
}

TEST_F(PythonEngineTest, Invoke_EmptyName_ReturnsFalse)
{
    write_script("def on_produce(tx, msgs, api):\n    return True\n");
    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    EXPECT_FALSE(engine.invoke(""));
    engine.finalize();
}

TEST_F(PythonEngineTest, Invoke_ScriptError_ReturnsFalseAndIncrementsErrors)
{
    write_script(R"(
def on_produce(tx, msgs, api):
    return True

def bad_func():
    raise RuntimeError("intentional test error")
)");
    RoleHostCore core;
    PythonEngine engine;
    ASSERT_TRUE(setup_engine_with_core(engine, core));

    EXPECT_FALSE(engine.invoke("bad_func"));
    EXPECT_EQ(core.script_error_count(), 1u);
    engine.finalize();
}

TEST_F(PythonEngineTest, Invoke_FromNonOwnerThread_Queued)
{
    write_script(R"(
def on_produce(tx, msgs, api):
    return True

def on_heartbeat():
    pass
)");
    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    // Non-owner thread: enqueues request, blocks on future.
    // Owner must process via process_pending_() which is called
    // at the end of each hot-path invoke.
    std::atomic<bool> done{false};
    bool result = false;

    std::thread t([&]
    {
        result = engine.invoke("on_heartbeat");
        done.store(true, std::memory_order_release);
    });

    // Owner thread: trigger process_pending_() by calling hot-path invokes.
    // Use a bounded loop with timeout to detect deadlock.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done.load(std::memory_order_acquire))
    {
        ASSERT_LT(std::chrono::steady_clock::now(), deadline)
            << "Timeout: non-owner invoke() was never processed by process_pending_()";
        std::vector<IncomingMessage> msgs;
        engine.invoke_produce({nullptr, 0}, msgs);
        std::this_thread::yield();
    }

    t.join();
    EXPECT_TRUE(result);
    engine.finalize();
}

TEST_F(PythonEngineTest, Invoke_FromNonOwnerThread_FinalizeUnblocks)
{
    write_script(R"(
def on_produce(tx, msgs, api):
    return True

def slow_func():
    pass
)");
    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    // Non-owner thread: enqueues request, blocks on future.
    std::atomic<bool> started{false};
    bool result = true; // expect false after finalize cancels

    std::thread t([&]
    {
        started.store(true, std::memory_order_release);
        result = engine.invoke("slow_func");
    });

    // Wait for non-owner thread to enter invoke().
    while (!started.load(std::memory_order_acquire))
        std::this_thread::yield();

    // finalize() cancels pending promises OR the thread sees accepting_=false.
    // Either way, the non-owner thread's invoke() must return false.
    engine.finalize();
    t.join();

    EXPECT_FALSE(result) << "Non-owner invoke must return false after finalize()";
}

TEST_F(PythonEngineTest, Invoke_ConcurrentNonOwnerThreads)
{
    write_script(R"(
def on_produce(tx, msgs, api):
    return True

def on_heartbeat():
    pass
)");
    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    constexpr int kThreads = 4;
    constexpr int kCallsPerThread = 10;
    std::atomic<int> success_count{0};

    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i)
    {
        threads.emplace_back([&]
        {
            for (int j = 0; j < kCallsPerThread; ++j)
            {
                if (engine.invoke("on_heartbeat"))
                    success_count.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // Owner: drain the queue by calling hot-path invokes until all threads finish.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (success_count.load(std::memory_order_relaxed) < kThreads * kCallsPerThread)
    {
        ASSERT_LT(std::chrono::steady_clock::now(), deadline)
            << "Timeout: only " << success_count.load() << " of "
            << kThreads * kCallsPerThread << " invokes completed";
        std::vector<IncomingMessage> msgs;
        engine.invoke_produce({nullptr, 0}, msgs);
        std::this_thread::yield();
    }

    for (auto &t : threads)
        t.join();

    EXPECT_EQ(success_count.load(), kThreads * kCallsPerThread);
    engine.finalize();
}

TEST_F(PythonEngineTest, Invoke_AfterFinalize_ReturnsFalse)
{
    write_script(R"(
def on_produce(tx, msgs, api):
    return True

def on_heartbeat():
    pass
)");
    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));
    engine.finalize();

    // After finalize, invoke should return false immediately.
    EXPECT_FALSE(engine.invoke("on_heartbeat"));
}

// ============================================================================
// invoke(name, args) + eval(code)
// ============================================================================

TEST_F(PythonEngineTest, Invoke_WithArgs_CallsFunction)
{
    write_script(R"(
def on_produce(tx, msgs, api):
    return True

def greet(**kwargs):
    pass
)");
    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    nlohmann::json args = {{"name", "test"}};
    EXPECT_TRUE(engine.invoke("greet", args));
    engine.finalize();
}

TEST_F(PythonEngineTest, Eval_ReturnsScalarResult)
{
    write_script("def on_produce(tx, msgs, api):\n    return True\n");
    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    auto r1 = engine.eval("42");
    EXPECT_EQ(r1.status, InvokeStatus::Ok);
    EXPECT_EQ(r1.value, 42);

    auto r2 = engine.eval("'hello'");
    EXPECT_EQ(r2.status, InvokeStatus::Ok);
    EXPECT_EQ(r2.value, "hello");

    auto r3 = engine.eval("True");
    EXPECT_EQ(r3.status, InvokeStatus::Ok);
    EXPECT_EQ(r3.value, true);

    engine.finalize();
}

TEST_F(PythonEngineTest, Eval_ErrorReturnsEmpty)
{
    write_script("def on_produce(tx, msgs, api):\n    return True\n");
    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    auto r = engine.eval("undefined_variable");
    EXPECT_EQ(r.status, InvokeStatus::ScriptError);
    engine.finalize();
}

// ============================================================================
// Python shared_data (api.shared_data dict)
// ============================================================================

TEST_F(PythonEngineTest, SharedData_PersistsAcrossCallbacks)
{
    // Test that api.shared_data dict persists across callbacks.
    // on_init sets counter=0, on_produce increments, get_counter returns it.
    // We verify the ACTUAL VALUE (5 after 5 calls), not just "no errors".
    write_script(R"(
_api_ref = None

def on_init(api):
    global _api_ref
    _api_ref = api
    api.shared_data["counter"] = 0

def on_produce(tx, msgs, api):
    api.shared_data["counter"] += 1
    return True

def get_counter():
    return _api_ref.shared_data["counter"]
)");
    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    engine.invoke_on_init();
    ASSERT_EQ(engine.script_error_count(), 0u)
        << "on_init failed";

    std::vector<IncomingMessage> msgs;
    for (int i = 0; i < 5; ++i)
        engine.invoke_produce({nullptr, 0}, msgs);

    ASSERT_EQ(engine.script_error_count(), 0u)
        << "on_produce failed";

    // Verify actual value via generic invoke + eval.
    auto result = engine.eval("get_counter()");
    EXPECT_EQ(result.status, InvokeStatus::Ok);
    EXPECT_EQ(result.value, 5) << "Counter should be 5 after 5 on_produce calls";

    engine.finalize();
}

// ============================================================================
// API parity tests — diagnostics, custom metrics, environment, queue-state
// ============================================================================

TEST_F(PythonEngineTest, Api_LoopOverrunCount_ReadsFromCore)
{
    write_script(R"(
def on_produce(tx, msgs, api):
    v = api.loop_overrun_count()
    assert v == 3, f"expected 3, got {v}"
    return False
)");

    RoleHostCore core;
    core.inc_loop_overrun();
    core.inc_loop_overrun();
    core.inc_loop_overrun();

    PythonEngine engine;
    ASSERT_TRUE(setup_engine_with_core(engine, core));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce({&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u);
    engine.finalize();
}

TEST_F(PythonEngineTest, Api_LastCycleWorkUs_ReadsFromCore)
{
    write_script(R"(
def on_produce(tx, msgs, api):
    v = api.last_cycle_work_us()
    assert v == 12345, f"expected 12345, got {v}"
    return False
)");

    RoleHostCore core;
    core.set_last_cycle_work_us(12345);

    PythonEngine engine;
    ASSERT_TRUE(setup_engine_with_core(engine, core));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce({&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u);
    engine.finalize();
}

TEST_F(PythonEngineTest, Api_CriticalError_DefaultIsFalse)
{
    write_script(R"(
def on_produce(tx, msgs, api):
    assert api.critical_error() == False, "critical_error should be False by default"
    return False
)");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce({&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u);
    engine.finalize();
}

TEST_F(PythonEngineTest, Api_IdentityAccessors_ReturnCorrectValues)
{
    write_script(R"(
def on_produce(tx, msgs, api):
    assert api.uid() == "PROD-TestEngine-00000001", f"uid: {api.uid()}"
    assert api.name() == "TestEngine", f"name: {api.name()}"
    assert api.channel() == "test.channel", f"channel: {api.channel()}"
    assert api.log_level() == "error", f"log_level: {api.log_level()}"
    assert isinstance(api.script_dir(), str), "script_dir must be str"
    assert isinstance(api.role_dir(), str), "role_dir must be str"
    return False
)");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce({&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u);
    engine.finalize();
}

TEST_F(PythonEngineTest, Api_CustomMetrics_ReportAndReadInMetrics)
{
    write_script(R"(
def on_produce(tx, msgs, api):
    api.report_metric("latency_ms", 42.5)
    api.report_metric("throughput", 100)

    m = api.metrics()
    assert "custom" in m, "custom metrics group must exist"
    assert m["custom"]["latency_ms"] == 42.5, f"got {m['custom']['latency_ms']}"
    assert m["custom"]["throughput"] == 100, f"got {m['custom']['throughput']}"
    return False
)");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce({&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u);
    engine.finalize();
}

TEST_F(PythonEngineTest, Api_CustomMetrics_BatchAndClear)
{
    write_script(R"(
def on_produce(tx, msgs, api):
    api.report_metrics({"a": 1.0, "b": 2.0, "c": 3.0})
    m = api.metrics()
    assert m["custom"]["a"] == 1.0
    assert m["custom"]["b"] == 2.0
    assert m["custom"]["c"] == 3.0

    api.clear_custom_metrics()
    m2 = api.metrics()
    assert "custom" not in m2, "custom should be gone after clear"
    return False
)");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce({&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u);
    engine.finalize();
}

// ============================================================================
// 15. Error paths for on_init / on_stop / on_inbox
// ============================================================================

TEST_F(PythonEngineTest, InvokeOnInit_ScriptError)
{
    write_script(
        "def on_produce(tx, msgs, api):\n"
        "    return True\n"
        "\n"
        "def on_init(api):\n"
        "    raise RuntimeError('init exploded')\n");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    EXPECT_EQ(engine.script_error_count(), 0u);
    engine.invoke_on_init();
    EXPECT_GE(engine.script_error_count(), 1u)
        << "on_init raising RuntimeError should increment script_error_count";

    engine.finalize();
}

TEST_F(PythonEngineTest, InvokeOnStop_ScriptError)
{
    write_script(
        "def on_produce(tx, msgs, api):\n"
        "    return True\n"
        "\n"
        "def on_stop(api):\n"
        "    raise RuntimeError('stop exploded')\n");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    EXPECT_EQ(engine.script_error_count(), 0u);
    engine.invoke_on_stop();
    EXPECT_GE(engine.script_error_count(), 1u)
        << "on_stop raising RuntimeError should increment script_error_count";

    engine.finalize();
}

TEST_F(PythonEngineTest, InvokeOnInbox_ScriptError)
{
    write_script(
        "def on_produce(tx, msgs, api):\n"
        "    return False\n"
        "\n"
        "def on_inbox(msg, api):\n"
        "    raise RuntimeError('inbox exploded')\n");

    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_produce"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame", "aligned"));
    ASSERT_TRUE(engine.register_slot_type(spec, "InboxFrame", "aligned"));

    auto test_api = make_api(default_core_);
    ASSERT_TRUE(engine.build_api(*test_api));

    EXPECT_EQ(engine.script_error_count(), 0u);
    float inbox_data = 1.0f;
    engine.invoke_on_inbox({&inbox_data, sizeof(inbox_data), "SENDER-00000001", 1});
    EXPECT_GE(engine.script_error_count(), 1u)
        << "on_inbox raising RuntimeError should increment script_error_count";

    engine.finalize();
}

// ============================================================================
// 16. Queue-state API defaults (no queue connected)
// ============================================================================

TEST_F(PythonEngineTest, Api_ProducerQueueState_WithoutQueue)
{
    write_script(R"(
def on_produce(tx, msgs, api):
    cap = api.out_capacity()
    pol = api.out_policy()
    assert cap == 0, f"expected out_capacity==0, got {cap}"
    assert pol == "", f"expected out_policy=='', got '{pol}'"
    return False
)");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce({&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "Script assertion failed -- producer queue-state defaults incorrect";

    engine.finalize();
}

TEST_F(PythonEngineTest, Api_ProcessorQueueState_DualDefaults)
{
    write_script(R"(
def on_process(rx, tx, msgs, api):
    assert api.in_capacity() == 0, f"in_capacity={api.in_capacity()}"
    assert api.in_policy() == "", f"in_policy='{api.in_policy()}'"
    assert api.out_capacity() == 0, f"out_capacity={api.out_capacity()}"
    assert api.out_policy() == "", f"out_policy='{api.out_policy()}'"
    assert api.last_seq() == 0, f"last_seq={api.last_seq()}"
    return False
)");

    RoleHostCore core;
    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &core));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_process"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "InSlotFrame", "aligned"));
    ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame", "aligned"));

    auto test_api = make_api(core, "proc");
    test_api->set_channel("test.in.channel");
    test_api->set_out_channel("test.out.channel");
    ASSERT_TRUE(engine.build_api(*test_api));

    std::vector<IncomingMessage> msgs;
    auto result = engine.invoke_process(
        InvokeRx{nullptr, 0},
        InvokeTx{nullptr, 0}, msgs);
    EXPECT_EQ(result, InvokeResult::Discard);
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "Script assertion failed -- processor queue-state defaults incorrect";

    engine.finalize();
}

// ============================================================================
// 18. Metrics loop group completeness
// ============================================================================

TEST_F(PythonEngineTest, Metrics_AllLoopFields_Present)
{
    write_script(R"(
def on_produce(tx, msgs, api):
    m = api.metrics()
    assert "loop" in m, "loop group must exist in metrics"
    loop = m["loop"]
    assert isinstance(loop["iteration_count"], int), "iteration_count must be int"
    assert isinstance(loop["loop_overrun_count"], int), "loop_overrun_count must be int"
    assert isinstance(loop["last_cycle_work_us"], int), "last_cycle_work_us must be int"
    assert isinstance(loop["configured_period_us"], int), "configured_period_us must be int"

    # Verify non-zero values from core
    assert loop["iteration_count"] == 5, f"iteration_count={loop['iteration_count']}"
    assert loop["loop_overrun_count"] == 2, f"loop_overrun_count={loop['loop_overrun_count']}"
    assert loop["last_cycle_work_us"] == 999, f"last_cycle_work_us={loop['last_cycle_work_us']}"
    assert loop["configured_period_us"] == 10000, f"configured_period_us={loop['configured_period_us']}"
    return False
)");

    RoleHostCore core;
    for (int i = 0; i < 5; ++i)
        core.inc_iteration_count();
    core.inc_loop_overrun();
    core.inc_loop_overrun();
    core.set_last_cycle_work_us(999);
    core.set_configured_period(10000);

    PythonEngine engine;
    ASSERT_TRUE(setup_engine_with_core(engine, core));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce({&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "Script assertion failed -- loop metrics fields incorrect";

    engine.finalize();
}

// ============================================================================
// 19. Custom metrics edge cases
// ============================================================================

TEST_F(PythonEngineTest, Api_CustomMetrics_OverwriteSameKey)
{
    write_script(R"(
def on_produce(tx, msgs, api):
    api.report_metric("x", 1)
    api.report_metric("x", 2)
    m = api.metrics()
    assert "custom" in m, "custom group must exist"
    assert m["custom"]["x"] == 2, f"expected x==2 after overwrite, got {m['custom']['x']}"
    return False
)");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce({&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "Script assertion failed -- custom metric overwrite incorrect";

    engine.finalize();
}

TEST_F(PythonEngineTest, Api_CustomMetrics_ZeroValue)
{
    write_script(R"(
def on_produce(tx, msgs, api):
    api.report_metric("x", 0.0)
    m = api.metrics()
    assert "custom" in m, "custom group must exist even with zero value"
    assert "x" in m["custom"], "key 'x' must be present"
    assert m["custom"]["x"] == 0.0, f"expected x==0.0, got {m['custom']['x']}"
    return False
)");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce({&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "Script assertion failed -- zero-value custom metric incorrect";

    engine.finalize();
}

// ============================================================================
// (V2 InvokeProduce_EmptyMessagesList removed — superseded by
// chunk 5's InvokeProduce_ReceivesMessages_EmptyList.)
// ============================================================================

// ============================================================================
// 21. stop_reason after critical error
// ============================================================================

TEST_F(PythonEngineTest, Api_StopReason_AfterCriticalError)
{
    write_script(R"(
def on_produce(tx, msgs, api):
    reason = api.stop_reason()
    assert reason == "critical_error", f"expected 'critical_error', got '{reason}'"
    return False
)");

    RoleHostCore core;
    core.set_critical_error(); // sets StopReason::CriticalError(3) + shutdown_requested

    PythonEngine engine;
    ASSERT_TRUE(setup_engine_with_core(engine, core));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce({&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "stop_reason should be 'critical_error' after set_critical_error()";

    engine.finalize();
}

// ============================================================================
// 22. Processor channels (in_channel / out_channel)
// ============================================================================

TEST_F(PythonEngineTest, Api_ProcessorChannels_InOut)
{
    write_script(R"(
def on_process(rx, tx, msgs, api):
    ic = api.in_channel()
    oc = api.out_channel()
    assert ic == "sensor.input", f"in_channel mismatch: {ic}"
    assert oc == "sensor.output", f"out_channel mismatch: {oc}"
    return False
)");

    RoleHostCore core;
    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &core));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_process"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "InSlotFrame", "aligned"));
    ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame", "aligned"));

    auto test_api = make_api(core, "proc");
    test_api->set_channel("sensor.input");
    test_api->set_out_channel("sensor.output");
    ASSERT_TRUE(engine.build_api(*test_api));

    float in_data  = 1.0f;
    float out_data = 0.0f;
    std::vector<IncomingMessage> msgs;
    auto result = engine.invoke_process(
        InvokeRx{&in_data, sizeof(in_data)},
        InvokeTx{&out_data, sizeof(out_data)}, msgs);
    EXPECT_EQ(result, InvokeResult::Discard);
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "Script assertion failed -- processor channels do not match context";

    engine.finalize();
}

// ============================================================================
// 23. open_inbox without broker returns None
// ============================================================================

TEST_F(PythonEngineTest, Api_OpenInbox_WithoutBroker)
{
    write_script(R"(
def on_produce(tx, msgs, api):
    result = api.open_inbox("some-uid")
    assert result is None, f"expected None, got {result}"
    return False
)");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    auto result =
        engine.invoke_produce({&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(result, InvokeResult::Discard);
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "api.open_inbox() without broker should return None, not raise";

    engine.finalize();
}

// ============================================================================
// 24. report_metrics with non-dict argument produces error
// ============================================================================

TEST_F(PythonEngineTest, Api_ReportMetrics_NonDictArg_IsError)
{
    write_script(R"(
def on_produce(tx, msgs, api):
    api.report_metrics(42)  # wrong type
    return False
)");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce({&buf, sizeof(buf)}, msgs);
    EXPECT_GE(engine.script_error_count(), 1u)
        << "report_metrics(42) should produce a script error (type mismatch)";

    engine.finalize();
}

// ============================================================================
// 25. Full lifecycle with verified callback execution
// ============================================================================

TEST_F(PythonEngineTest, FullLifecycle_VerifiesCallbackExecution)
{
    write_script(R"(
_api_ref = None

def on_init(api):
    global _api_ref
    _api_ref = api
    api.shared_data['init_ran'] = True

def on_produce(tx, msgs, api):
    return False

def on_stop(api):
    api.shared_data['stop_ran'] = True

def get_init_ran():
    return _api_ref.shared_data.get('init_ran')

def get_stop_ran():
    return _api_ref.shared_data.get('stop_ran')
)");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    // Verify on_init sets the flag.
    engine.invoke_on_init();
    ASSERT_EQ(engine.script_error_count(), 0u) << "on_init failed";

    auto r1 = engine.eval("get_init_ran()");
    EXPECT_EQ(r1.status, InvokeStatus::Ok);
    EXPECT_EQ(r1.value, true) << "on_init should have set init_ran=True";

    // Verify on_stop sets the flag.
    engine.invoke_on_stop();
    ASSERT_EQ(engine.script_error_count(), 0u) << "on_stop failed";

    auto r2 = engine.eval("get_stop_ran()");
    EXPECT_EQ(r2.status, InvokeStatus::Ok);
    EXPECT_EQ(r2.value, true) << "on_stop should have set stop_ran=True";

    engine.finalize();
}

// ============================================================================
// Parity tests — match Lua coverage
// ============================================================================

TEST_F(PythonEngineTest, Api_ConsumerQueueState_WithoutQueue)
{
    write_script(R"(
def on_consume(rx, msgs, api):
    assert api.in_capacity() == 0, f"in_capacity: {api.in_capacity()}"
    assert api.in_policy() == "", f"in_policy: {api.in_policy()}"
    assert api.last_seq() == 0, f"last_seq: {api.last_seq()}"
    return True
)");

    RoleHostCore core;
    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &core));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_consume"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame", "aligned"));

    auto test_api = make_api(core, "cons");
    ASSERT_TRUE(engine.build_api(*test_api));

    float buf = 1.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_consume(InvokeRx{&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u);
    engine.finalize();
}

TEST_F(PythonEngineTest, Api_EnvironmentStrings_LogsDirRunDir)
{
    write_script(R"(
def on_produce(tx, msgs, api):
    assert isinstance(api.logs_dir(), str), "logs_dir must be str"
    assert isinstance(api.run_dir(), str), "run_dir must be str"
    # When role_dir is empty, logs_dir and run_dir are empty
    return False
)");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce({&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u);
    engine.finalize();
}

TEST_F(PythonEngineTest, Api_SpinlockCount_WithoutSHM)
{
    write_script(R"(
def on_produce(tx, msgs, api):
    assert api.spinlock_count() == 0, f"spinlock_count: {api.spinlock_count()}"
    return False
)");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce({&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u);
    engine.finalize();
}

TEST_F(PythonEngineTest, Api_Spinlock_WithoutSHM_IsError)
{
    write_script(R"(
def on_produce(tx, msgs, api):
    try:
        api.spinlock(0)
        assert False, "spinlock(0) should raise without producer/consumer"
    except ValueError:
        pass  # expected
    return False
)");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce({&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "try/except should catch the error, not propagate";
    engine.finalize();
}

TEST_F(PythonEngineTest, Api_Flexzone_WithoutSHM_ReturnsNone)
{
    write_script(R"(
def on_produce(tx, msgs, api):
    result = api.flexzone()
    assert result is None, f"flexzone should be None without SHM, got {result}"
    return False
)");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce({&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u);
    engine.finalize();
}

TEST_F(PythonEngineTest, Api_AsNumpy_ArrayField)
{
    write_script(R"(
def on_produce(tx, msgs, api):
    try:
        import numpy as np
    except ImportError:
        return False  # skip test if numpy not available

    arr = api.as_numpy(tx.slot.values)
    assert isinstance(arr, np.ndarray), f"expected ndarray, got {type(arr)}"
    assert arr.dtype == np.float32, f"expected float32, got {arr.dtype}"
    assert len(arr) == 4, f"expected 4 elements, got {len(arr)}"
    arr[:] = [1.0, 2.0, 3.0, 4.0]
    return True
)");

    // Schema: one scalar + one array field
    SchemaSpec spec;
    spec.has_schema = true;
    FieldDef f1;
    f1.name     = "header";
    f1.type_str = "float32";
    f1.count    = 1;
    f1.length   = 0;
    spec.fields.push_back(f1);

    FieldDef f2;
    f2.name     = "values";
    f2.type_str = "float32";
    f2.count    = 4;
    f2.length   = 0;
    spec.fields.push_back(f2);

    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_produce"));
    ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame", "aligned"));

    auto test_api = make_api(default_core_);
    ASSERT_TRUE(engine.build_api(*test_api));

    // Buffer: 1 float (header) + 4 floats (values) = 20 bytes
    // With aligned packing, no padding between float and float[4]
    float buf[5] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    std::vector<IncomingMessage> msgs;
    auto result = engine.invoke_produce({buf, sizeof(buf)}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u);

    if (result == InvokeResult::Commit)
    {
        // numpy was available — verify the write went through
        EXPECT_FLOAT_EQ(buf[1], 1.0f);
        EXPECT_FLOAT_EQ(buf[2], 2.0f);
        EXPECT_FLOAT_EQ(buf[3], 3.0f);
        EXPECT_FLOAT_EQ(buf[4], 4.0f);
    }
    else
    {
        // numpy not available — script returned False (discard)
        EXPECT_EQ(result, InvokeResult::Discard);
        GTEST_SKIP() << "numpy not available in staged Python — skipping";
    }

    engine.finalize();
}

TEST_F(PythonEngineTest, Api_AsNumpy_NonArrayField_Throws)
{
    write_script(R"(
def on_produce(tx, msgs, api):
    try:
        import numpy
    except ImportError:
        tx.slot.value = -1.0  # signal: numpy not available
        return True
    try:
        api.as_numpy(tx.slot.value)  # scalar, not array
        assert False, "as_numpy on scalar should raise"
    except TypeError as e:
        assert "ctypes array" in str(e), f"wrong error: {e}"
    tx.slot.value = 1.0  # signal: test passed
    return True
)");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce({&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u);
    if (buf < 0.0f)
        GTEST_SKIP() << "numpy not available in staged Python";
    EXPECT_FLOAT_EQ(buf, 1.0f) << "TypeError should have been caught";
    engine.finalize();
}

// ============================================================================
// Full engine startup — tests the EngineModuleParams startup/shutdown path
// ============================================================================

TEST_F(PythonEngineTest, FullStartup_Producer_SlotOnly)
{
    write_script(
        "def on_produce(tx, msgs, api):\n"
        "    tx.slot.value = 77.0\n"
        "    return True\n");

    PythonEngine engine;
    RoleHostCore core;

    auto api = make_api(core, "prod");

    pylabhub::scripting::EngineModuleParams params;
    params.engine            = &engine;
    params.api               = api.get();
    params.tag               = "prod";
    params.script_dir        = tmp_ / "script" / "python";
    params.entry_point       = "__init__.py";
    params.required_callback = "on_produce";
    params.out_slot_spec     = simple_schema();
    params.out_packing       = "aligned";

    engine.set_python_venv("");
    ASSERT_NO_THROW(pylabhub::scripting::engine_lifecycle_startup(nullptr, &params));

    EXPECT_GT(engine.type_sizeof("OutSlotFrame"), 0u);
    EXPECT_GT(engine.type_sizeof("SlotFrame"), 0u); // alias

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(result, InvokeResult::Commit);
    EXPECT_FLOAT_EQ(buf, 77.0f);

    pylabhub::scripting::engine_lifecycle_shutdown(nullptr, &params);
    pylabhub::scripting::engine_lifecycle_shutdown(nullptr, &params); // idempotent
}

TEST_F(PythonEngineTest, FullStartup_Producer_SlotAndFlexzone)
{
    write_script(
        "def on_produce(tx, msgs, api):\n"
        "    tx.slot.value = 10.0\n"
        "    return True\n");

    PythonEngine engine;
    RoleHostCore core;

    auto api = make_api(core, "prod");

    pylabhub::scripting::EngineModuleParams params;
    params.engine            = &engine;
    params.api               = api.get();
    params.tag               = "prod";
    params.script_dir        = tmp_ / "script" / "python";
    params.entry_point       = "__init__.py";
    params.required_callback = "on_produce";
    params.out_slot_spec     = simple_schema();
    params.out_fz_spec       = simple_schema();
    params.out_packing       = "aligned";

    // Role hosts set flexzone spec on core before engine startup (page-aligned).
    core.set_out_fz_spec(params.out_fz_spec,
                         pylabhub::hub::align_to_physical_page(
                             pylabhub::hub::compute_schema_size(params.out_fz_spec, params.out_packing)));

    engine.set_python_venv("");
    ASSERT_NO_THROW(pylabhub::scripting::engine_lifecycle_startup(nullptr, &params));

    EXPECT_GT(engine.type_sizeof("OutSlotFrame"), 0u);
    EXPECT_GT(engine.type_sizeof("OutFlexFrame"), 0u);
    EXPECT_GT(engine.type_sizeof("FlexFrame"), 0u); // alias
    EXPECT_TRUE(core.has_tx_fz());
    EXPECT_GT(core.out_schema_fz_size(), 0u);

    // Cross-check: engine type size must match schema logical size.
    // core.out_schema_fz_size() is page-aligned (physical); engine type is logical.
    EXPECT_EQ(engine.type_sizeof("OutFlexFrame"),
              pylabhub::hub::compute_schema_size(params.out_fz_spec, params.out_packing))
        << "Engine-built type size must match schema logical size";

    float slot_buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    auto result = engine.invoke_produce(
        InvokeTx{&slot_buf, sizeof(slot_buf)}, msgs);
    EXPECT_EQ(result, InvokeResult::Commit);
    EXPECT_FLOAT_EQ(slot_buf, 10.0f);

    pylabhub::scripting::engine_lifecycle_shutdown(nullptr, &params);
}

TEST_F(PythonEngineTest, FullStartup_Consumer)
{
    write_script(
        "def on_consume(rx, msgs, api):\n"
        "    assert rx.slot is not None, 'expected slot'\n"
        "    return True\n");

    PythonEngine engine;
    RoleHostCore core;

    auto api = make_api(core, "cons");

    pylabhub::scripting::EngineModuleParams params;
    params.engine            = &engine;
    params.api               = api.get();
    params.tag               = "cons";
    params.script_dir        = tmp_ / "script" / "python";
    params.entry_point       = "__init__.py";
    params.required_callback = "on_consume";
    params.in_slot_spec      = simple_schema();
    params.in_packing        = "aligned";

    engine.set_python_venv("");
    ASSERT_NO_THROW(pylabhub::scripting::engine_lifecycle_startup(nullptr, &params));

    EXPECT_GT(engine.type_sizeof("InSlotFrame"), 0u);
    EXPECT_GT(engine.type_sizeof("SlotFrame"), 0u); // alias

    float data = 42.0f;
    std::vector<IncomingMessage> msgs;
    auto result = engine.invoke_consume(InvokeRx{&data, sizeof(data)}, msgs);
    EXPECT_EQ(result, InvokeResult::Commit);
    EXPECT_EQ(engine.script_error_count(), 0u);

    pylabhub::scripting::engine_lifecycle_shutdown(nullptr, &params);
}

TEST_F(PythonEngineTest, FullStartup_Processor)
{
    write_script(
        "def on_process(rx, tx, msgs, api):\n"
        "    if rx.slot is not None and tx.slot is not None:\n"
        "        tx.slot.value = rx.slot.value * 2.0\n"
        "    return True\n");

    PythonEngine engine;
    RoleHostCore core;

    auto api = make_api(core, "proc");

    pylabhub::scripting::EngineModuleParams params;
    params.engine            = &engine;
    params.api               = api.get();
    params.tag               = "proc";
    params.script_dir        = tmp_ / "script" / "python";
    params.entry_point       = "__init__.py";
    params.required_callback = "on_process";
    params.in_slot_spec      = simple_schema();
    params.out_slot_spec     = simple_schema();
    params.in_packing        = "aligned";
    params.out_packing       = "aligned";

    engine.set_python_venv("");
    ASSERT_NO_THROW(pylabhub::scripting::engine_lifecycle_startup(nullptr, &params));

    EXPECT_GT(engine.type_sizeof("InSlotFrame"), 0u);
    EXPECT_GT(engine.type_sizeof("OutSlotFrame"), 0u);
    EXPECT_EQ(engine.type_sizeof("SlotFrame"), 0u); // no alias for processor

    float in_data = 5.0f;
    float out_data = 0.0f;
    std::vector<IncomingMessage> msgs;
    auto result = engine.invoke_process(
        InvokeRx{&in_data, sizeof(in_data)},
        InvokeTx{&out_data, sizeof(out_data)}, msgs);
    EXPECT_EQ(result, InvokeResult::Commit);
    EXPECT_FLOAT_EQ(out_data, 10.0f);

    pylabhub::scripting::engine_lifecycle_shutdown(nullptr, &params);
}

// ============================================================================
// Schema logical size — complex schemas, aligned vs packed, slot + flexzone
// ============================================================================

TEST_F(PythonEngineTest, SlotLogicalSize_Aligned_PaddingSensitive)
{
    // Script reads api.slot_logical_size() and asserts against expected value.
    write_script(
        "def on_produce(tx, msgs, api):\n"
        "    sz = api.slot_logical_size()\n"
        "    assert sz == 16, f'expected 16, got {sz}'\n"
        "    return False\n");

    PythonEngine engine;
    RoleHostCore core;
    auto api = make_api(core, "prod");

    auto slot_spec = padding_schema();
    slot_spec.packing = "aligned";
    core.set_out_slot_spec(SchemaSpec{slot_spec},
                           pylabhub::hub::compute_schema_size(slot_spec, "aligned"));

    pylabhub::scripting::EngineModuleParams params;
    params.engine            = &engine;
    params.api               = api.get();
    params.tag               = "prod";
    params.script_dir        = tmp_ / "script" / "python";
    params.entry_point       = "__init__.py";
    params.required_callback = "on_produce";
    params.out_slot_spec     = slot_spec;
    params.out_packing       = "aligned";

    engine.set_python_venv("");
    ASSERT_NO_THROW(pylabhub::scripting::engine_lifecycle_startup(nullptr, &params));

    // C++ side: verify compute_schema_size matches engine type_sizeof.
    size_t logical = pylabhub::hub::compute_schema_size(slot_spec, "aligned");
    EXPECT_EQ(logical, 16u);
    EXPECT_EQ(engine.type_sizeof("OutSlotFrame"), logical);
    EXPECT_EQ(core.out_slot_logical_size(), logical);

    float buf[4] = {};
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce(InvokeTx{buf, sizeof(buf)}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u);

    pylabhub::scripting::engine_lifecycle_shutdown(nullptr, &params);
}

TEST_F(PythonEngineTest, SlotLogicalSize_Packed_NoPadding)
{
    write_script(
        "def on_produce(tx, msgs, api):\n"
        "    sz = api.slot_logical_size()\n"
        "    assert sz == 13, f'expected 13, got {sz}'\n"
        "    return False\n");

    PythonEngine engine;
    RoleHostCore core;
    auto api = make_api(core, "prod");

    auto slot_spec = padding_schema();
    slot_spec.packing = "packed";
    core.set_out_slot_spec(SchemaSpec{slot_spec},
                           pylabhub::hub::compute_schema_size(slot_spec, "packed"));

    pylabhub::scripting::EngineModuleParams params;
    params.engine            = &engine;
    params.api               = api.get();
    params.tag               = "prod";
    params.script_dir        = tmp_ / "script" / "python";
    params.entry_point       = "__init__.py";
    params.required_callback = "on_produce";
    params.out_slot_spec     = slot_spec;
    params.out_packing       = "packed";

    engine.set_python_venv("");
    ASSERT_NO_THROW(pylabhub::scripting::engine_lifecycle_startup(nullptr, &params));

    size_t logical = pylabhub::hub::compute_schema_size(slot_spec, "packed");
    EXPECT_EQ(logical, 13u);
    EXPECT_EQ(engine.type_sizeof("OutSlotFrame"), logical);
    EXPECT_EQ(core.out_slot_logical_size(), logical);

    uint8_t buf[16] = {};
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce(InvokeTx{buf, sizeof(buf)}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u);

    pylabhub::scripting::engine_lifecycle_shutdown(nullptr, &params);
}

TEST_F(PythonEngineTest, SlotLogicalSize_ComplexMixed_Aligned)
{
    write_script(
        "def on_produce(tx, msgs, api):\n"
        "    sz = api.slot_logical_size()\n"
        "    assert sz == 56, f'expected 56, got {sz}'\n"
        "    return False\n");

    PythonEngine engine;
    RoleHostCore core;
    auto api = make_api(core, "prod");

    auto slot_spec = complex_mixed_schema();
    slot_spec.packing = "aligned";
    core.set_out_slot_spec(SchemaSpec{slot_spec},
                           pylabhub::hub::compute_schema_size(slot_spec, "aligned"));

    pylabhub::scripting::EngineModuleParams params;
    params.engine            = &engine;
    params.api               = api.get();
    params.tag               = "prod";
    params.script_dir        = tmp_ / "script" / "python";
    params.entry_point       = "__init__.py";
    params.required_callback = "on_produce";
    params.out_slot_spec     = slot_spec;
    params.out_packing       = "aligned";

    engine.set_python_venv("");
    ASSERT_NO_THROW(pylabhub::scripting::engine_lifecycle_startup(nullptr, &params));

    size_t logical = pylabhub::hub::compute_schema_size(slot_spec, "aligned");
    EXPECT_EQ(logical, 56u);
    EXPECT_EQ(engine.type_sizeof("OutSlotFrame"), logical);

    uint8_t buf[64] = {};
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce(InvokeTx{buf, sizeof(buf)}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u);

    pylabhub::scripting::engine_lifecycle_shutdown(nullptr, &params);
}

TEST_F(PythonEngineTest, FlexzoneLogicalSize_ArrayFields)
{
    // Slot + flexzone with different schemas — both logical sizes accessible.
    write_script(
        "def on_produce(tx, msgs, api):\n"
        "    slot_sz = api.slot_logical_size()\n"
        "    fz_sz = api.flexzone_logical_size()\n"
        "    assert slot_sz == 16, f'slot: expected 16, got {slot_sz}'\n"
        "    assert fz_sz == 24, f'fz: expected 24, got {fz_sz}'\n"
        "    return False\n");

    PythonEngine engine;
    RoleHostCore core;
    auto api = make_api(core, "prod");

    auto slot_spec = padding_schema();
    slot_spec.packing = "aligned";
    auto fz_spec = fz_array_schema();
    fz_spec.packing = "aligned";

    core.set_out_slot_spec(SchemaSpec{slot_spec},
                           pylabhub::hub::compute_schema_size(slot_spec, "aligned"));
    core.set_out_fz_spec(SchemaSpec{fz_spec},
                         pylabhub::hub::align_to_physical_page(
                             pylabhub::hub::compute_schema_size(fz_spec, "aligned")));

    pylabhub::scripting::EngineModuleParams params;
    params.engine            = &engine;
    params.api               = api.get();
    params.tag               = "prod";
    params.script_dir        = tmp_ / "script" / "python";
    params.entry_point       = "__init__.py";
    params.required_callback = "on_produce";
    params.out_slot_spec     = slot_spec;
    params.out_fz_spec       = fz_spec;
    params.out_packing       = "aligned";

    engine.set_python_venv("");
    ASSERT_NO_THROW(pylabhub::scripting::engine_lifecycle_startup(nullptr, &params));

    // C++ cross-checks.
    EXPECT_EQ(pylabhub::hub::compute_schema_size(slot_spec, "aligned"), 16u);
    EXPECT_EQ(pylabhub::hub::compute_schema_size(fz_spec, "aligned"), 24u);
    EXPECT_EQ(core.out_slot_logical_size(), 16u);
    EXPECT_EQ(core.out_schema_fz_size(), 4096u); // page-aligned physical
    EXPECT_EQ(engine.type_sizeof("OutSlotFrame"), 16u);
    EXPECT_EQ(engine.type_sizeof("OutFlexFrame"), 24u);

    uint8_t slot_buf[16] = {};
    uint8_t fz_buf[24] = {};
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce(InvokeTx{slot_buf, sizeof(slot_buf)}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u);

    pylabhub::scripting::engine_lifecycle_shutdown(nullptr, &params);
}

// ============================================================================
// FullStartup — multifield schema data round-trip (all three roles)
// ============================================================================

TEST_F(PythonEngineTest, FullStartup_Producer_Multifield)
{
    write_script(
        "def on_produce(tx, msgs, api):\n"
        "    tx.slot.ts = 1.23456789\n"
        "    tx.slot.flag = 0xAB\n"
        "    tx.slot.count = -42\n"
        "    return True\n");

    PythonEngine engine;
    RoleHostCore core;
    auto api = make_api(core, "prod");

    auto spec = padding_schema();
    spec.packing = "aligned";
    core.set_out_slot_spec(SchemaSpec{spec},
                           pylabhub::hub::compute_schema_size(spec, "aligned"));

    pylabhub::scripting::EngineModuleParams params;
    params.engine            = &engine;
    params.api               = api.get();
    params.tag               = "prod";
    params.script_dir        = tmp_ / "script" / "python";
    params.entry_point       = "__init__.py";
    params.required_callback = "on_produce";
    params.out_slot_spec     = spec;
    params.out_packing       = "aligned";

    engine.set_python_venv("");
    ASSERT_NO_THROW(pylabhub::scripting::engine_lifecycle_startup(nullptr, &params));
    EXPECT_EQ(engine.type_sizeof("OutSlotFrame"), 16u);

    // C struct matching the schema layout.
    struct { double ts; uint8_t flag; uint8_t pad[3]; int32_t count; } buf{};
    static_assert(sizeof(buf) == 16);

    std::vector<IncomingMessage> msgs;
    auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(result, InvokeResult::Commit);
    EXPECT_EQ(engine.script_error_count(), 0u);

    EXPECT_DOUBLE_EQ(buf.ts, 1.23456789);
    EXPECT_EQ(buf.flag, 0xAB);
    EXPECT_EQ(buf.count, -42);

    pylabhub::scripting::engine_lifecycle_shutdown(nullptr, &params);
}

TEST_F(PythonEngineTest, FullStartup_Consumer_Multifield)
{
    write_script(
        "def on_consume(rx, msgs, api):\n"
        "    assert abs(rx.slot.ts - 9.87) < 0.001, f'ts={rx.slot.ts}'\n"
        "    assert rx.slot.flag == 0xCD, f'flag={rx.slot.flag}'\n"
        "    assert rx.slot.count == 100, f'count={rx.slot.count}'\n"
        "    return True\n");

    PythonEngine engine;
    RoleHostCore core;
    auto api = make_api(core, "cons");

    auto spec = padding_schema();
    spec.packing = "aligned";
    core.set_in_slot_spec(SchemaSpec{spec},
                          pylabhub::hub::compute_schema_size(spec, "aligned"));

    pylabhub::scripting::EngineModuleParams params;
    params.engine            = &engine;
    params.api               = api.get();
    params.tag               = "cons";
    params.script_dir        = tmp_ / "script" / "python";
    params.entry_point       = "__init__.py";
    params.required_callback = "on_consume";
    params.in_slot_spec      = spec;
    params.in_packing        = "aligned";

    engine.set_python_venv("");
    ASSERT_NO_THROW(pylabhub::scripting::engine_lifecycle_startup(nullptr, &params));

    struct { double ts; uint8_t flag; uint8_t pad[3]; int32_t count; } buf{};
    buf.ts = 9.87; buf.flag = 0xCD; buf.count = 100;

    std::vector<IncomingMessage> msgs;
    auto result = engine.invoke_consume(
        pylabhub::scripting::InvokeRx{&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(result, InvokeResult::Commit);
    EXPECT_EQ(engine.script_error_count(), 0u);

    pylabhub::scripting::engine_lifecycle_shutdown(nullptr, &params);
}

TEST_F(PythonEngineTest, FullStartup_Processor_Multifield)
{
    write_script(
        "def on_process(rx, tx, msgs, api):\n"
        "    tx.slot.ts = rx.slot.ts\n"
        "    tx.slot.flag = rx.slot.flag\n"
        "    tx.slot.count = rx.slot.count * 2\n"
        "    return True\n");

    PythonEngine engine;
    RoleHostCore core;
    auto api = make_api(core, "proc");

    auto spec = padding_schema();
    spec.packing = "aligned";
    size_t sz = pylabhub::hub::compute_schema_size(spec, "aligned");
    core.set_in_slot_spec(SchemaSpec{spec}, sz);
    core.set_out_slot_spec(SchemaSpec{spec}, sz);

    pylabhub::scripting::EngineModuleParams params;
    params.engine            = &engine;
    params.api               = api.get();
    params.tag               = "proc";
    params.script_dir        = tmp_ / "script" / "python";
    params.entry_point       = "__init__.py";
    params.required_callback = "on_process";
    params.in_slot_spec      = spec;
    params.out_slot_spec     = spec;
    params.in_packing        = "aligned";
    params.out_packing       = "aligned";

    engine.set_python_venv("");
    ASSERT_NO_THROW(pylabhub::scripting::engine_lifecycle_startup(nullptr, &params));

    struct { double ts; uint8_t flag; uint8_t pad[3]; int32_t count; } in_buf{}, out_buf{};
    in_buf.ts = 1.23456789; in_buf.flag = 0xAB; in_buf.count = -42;

    std::vector<IncomingMessage> msgs;
    auto result = engine.invoke_process(
        pylabhub::scripting::InvokeRx{&in_buf, sizeof(in_buf)},
        InvokeTx{&out_buf, sizeof(out_buf)}, msgs);
    EXPECT_EQ(result, InvokeResult::Commit);
    EXPECT_EQ(engine.script_error_count(), 0u);

    EXPECT_DOUBLE_EQ(out_buf.ts, 1.23456789);
    EXPECT_EQ(out_buf.flag, 0xAB);
    EXPECT_EQ(out_buf.count, -84);

    pylabhub::scripting::engine_lifecycle_shutdown(nullptr, &params);
}

// ============================================================================
// Band pub/sub API — L2 (no broker; the 4 api methods must be callable
// and return gracefully: None / False / no-op without any broker attached).
// ============================================================================

TEST_F(PythonEngineTest, Api_Band_AllMethodsGraceful_NoBroker)
{
    write_script(
        "results = {}\n"
        "def on_produce(tx, msgs, api):\n"
        "    results['join'] = api.band_join('#l2_test')\n"
        "    results['leave'] = api.band_leave('#l2_test')\n"
        "    api.band_broadcast('#l2_test', {'hello': 'world'})\n"
        "    results['send_ok'] = True\n"
        "    results['members'] = api.band_members('#l2_test')\n"
        "    assert results['join'] is None, f\"join={results['join']}\"\n"
        "    assert results['leave'] == False, f\"leave={results['leave']}\"\n"
        "    assert results['members'] is None, f\"members={results['members']}\"\n"
        "    return True\n");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    auto result = engine.invoke_produce(
        InvokeTx{&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(result, InvokeResult::Commit)
        << "on_produce should commit: all 4 band methods must return "
           "gracefully (None/False/no-op) without a broker";

    engine.finalize();
}

} // anonymous namespace
