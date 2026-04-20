/**
 * @file test_lua_engine.cpp
 * @brief Unit tests for LuaEngine — the ScriptEngine implementation for Lua.
 *
 * Tests the engine in isolation: no broker, no queues, no role host.
 * Uses raw memory buffers and temporary Lua script files.
 *
 * Covers:
 *   1. Lifecycle: initialize → load → register → build_api → invoke → finalize
 *   2. Type registration: ffi.typeof caching, sizeof validation
 *   3. invoke_produce: commit, discard, nil slot, error paths
 *   4. invoke_consume: read-only slot, return value, error detection
 *   5. invoke_process: dual slots, nil combinations
 *   6. Messages: table format for producer/consumer
 *   7. API closures: log, stop, version_info
 *   8. Error handling: script_error_count, stop_on_script_error
 *   9. create_thread_state: independent engine
 *
 * ============================================================================
 * HYBRID STATE DURING THE TEST-FRAMEWORK SWEEP (tracked in docs/tech_draft/
 * test_compliance_audit.md § "Correction status"). This file currently
 * contains two fixtures that are intentionally both live:
 *
 *   - LuaEngineIsolatedTest  (IsolatedProcessTest — Pattern 3)
 *     Tests already converted by chunks 1-5 + 6a of the Lua sweep.
 *     Each spawns a worker subprocess; bodies live in
 *     workers/lua_engine_workers.cpp.
 *
 *   - LuaEngineTest        (plain ::testing::Test — V2 antipattern)
 *     Tests NOT YET converted. Will be migrated chunk-by-chunk by
 *     subsequent commits (one chunk per thematic group in the numbered
 *     list above). **Policy (established 2026-04-19 during chunk-6a
 *     cleanup):** V2 tests whose P3 replacement strictly dominates
 *     them are deleted IN THE SAME COMMIT as the P3 conversion — not
 *     at sweep-end. The V2 fixture wrapper itself stays until its
 *     last test is converted, then goes in the sweep-end commit.
 *
 * The hybrid is temporary and deliberately visible in-file so a reader
 * landing on this source is not misled into thinking "both patterns
 * coexist forever" is the intended design. The audit doc tracks
 * progress; the chunk-local header comments below explain the
 * strengthenings and gap-fills applied to each converted group.
 * ============================================================================
 */
#include <gtest/gtest.h>

#include "lua_engine.hpp"
#include "utils/engine_module_params.hpp"
#include "utils/schema_utils.hpp"
#include "utils/role_host_core.hpp"
#include "test_patterns.h"    // Pattern 3 (IsolatedProcessTest) for chunk 1
#include "test_schema_helpers.h"

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
using pylabhub::scripting::LuaEngine;
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

namespace
{

// ============================================================================
// Test fixture
// ============================================================================

class LuaEngineTest : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        tmp_ = fs::temp_directory_path() / ("lua_engine_test_" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(tmp_);
    }

    void TearDown() override
    {
        fs::remove_all(tmp_);
    }

    void write_script(const std::string &content)
    {
        std::ofstream f(tmp_ / "init.lua");
        f << content;
    }

    /// Create a simple schema with one float32 field.
    RoleHostCore default_core_;

    std::unique_ptr<RoleAPIBase> make_api(RoleHostCore &core,
                                          const std::string &tag = "prod")
    {
        auto api = std::make_unique<RoleAPIBase>(core, tag, "TEST-ENGINE-00000001");
        api->set_name("TestEngine");
        api->set_channel("test.channel");
        api->set_log_level("error");
        api->set_stop_on_script_error(false);
        return api;
    }

    std::unique_ptr<RoleAPIBase> test_api_;

    bool setup_engine(LuaEngine &engine, const std::string &required_cb = "on_produce")
    {
        return setup_engine_with_core(engine, default_core_, required_cb);
    }

    bool setup_engine_with_core(LuaEngine &engine, RoleHostCore &core,
                                 const std::string &required_cb = "on_produce")
    {
        if (!engine.initialize("test", &core))
            return false;
        if (!engine.load_script(tmp_, "init.lua", required_cb.c_str()))
            return false;

        auto spec = simple_schema();
        if (!engine.register_slot_type(spec, "OutSlotFrame", "aligned"))
            return false;

        test_api_ = make_api(core);
        return engine.build_api(*test_api_);
    }

    fs::path tmp_;
};

// ============================================================================
// 1. Lifecycle
// ============================================================================

// ============================================================================
// Chunk 1 — Lifecycle, Type registration, Alias creation (Pattern 3)
//
// These tests used to live under the V2 LuaEngineTest fixture above; they
// were the first group converted to Pattern 3 by the test-framework sweep.
// Each spawns a subprocess where the engine is constructed and driven;
// bodies live in workers/lua_engine_workers.cpp.
//
// Remaining tests in this file still use the V2 fixture and will be
// converted chunk-by-chunk in subsequent commits (one chunk per thematic
// group: produce / consume / process / messages / api / error / multi-state).
// ============================================================================

namespace
{

class LuaEngineIsolatedTest : public pylabhub::tests::IsolatedProcessTest
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
                     ("plh_l2_lua_" + std::string(label) + "_" +
                      std::to_string(::getpid()) + "_" +
                      std::to_string(ctr.fetch_add(1)));
        fs::create_directories(p);
        paths_to_clean_.push_back(p);
        return p.string();
    }

    std::vector<fs::path> paths_to_clean_;
};

} // namespace

TEST_F(LuaEngineIsolatedTest, FullLifecycle)
{
    // Strengthened: the worker verifies on_init and on_stop actually
    // dispatched into the Lua runtime by checking report_metric side
    // effects on RoleHostCore.custom_metrics_snapshot(). See
    // workers/lua_engine_workers.cpp::full_lifecycle for the full body.
    auto w = SpawnWorker("lua_engine.full_lifecycle",
                         {unique_dir("full_lifecycle")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, InitializeAndFinalize_Succeeds)
{
    // Renamed from InitializeFailsGracefully — body never matched that
    // name. A real initialize-fails-gracefully test needs a failure-
    // injection hook (e.g. simulate luaL_newstate() returning nullptr);
    // adding that hook is queued as a follow-up.
    auto w = SpawnWorker("lua_engine.initialize_and_finalize_succeeds",
                         {unique_dir("init_finalize")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, RegisterSlotType_SizeofCorrect)
{
    auto w = SpawnWorker("lua_engine.register_slot_type_sizeof_correct",
                         {unique_dir("sizeof")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, RegisterSlotType_MultiField)
{
    auto w = SpawnWorker("lua_engine.register_slot_type_multi_field",
                         {unique_dir("multifield")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, RegisterSlotType_Packed_vs_Aligned)
{
    // Strengthened from RegisterSlotType_PackedPacking: verifies BOTH
    // aligned (8 bytes) and packed (5 bytes) for the same schema, and
    // explicitly asserts the sizes differ so a silent packing-arg-ignored
    // regression cannot slip by.
    auto w = SpawnWorker("lua_engine.register_slot_type_packed_vs_aligned",
                         {unique_dir("packed_vs_aligned")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, RegisterSlotType_HasSchemaFalse_ReturnsFalse)
{
    auto w = SpawnWorker(
        "lua_engine.register_slot_type_has_schema_false_returns_false",
        {unique_dir("has_schema_false")});
    // Engine logs "has_schema=false" rejection at ERROR level; declare
    // it so the framework's broad "no ERROR" check doesn't fire.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/{"has_schema"});
}

// NEW: coverage fill for types previously not exercised at L2
// (bool, int8, int16, uint64).  See all_types_schema in
// tests/test_framework/test_schema_helpers.h for the full list and the
// dispatcher-source citations that justify the coverage.
TEST_F(LuaEngineIsolatedTest, RegisterSlotType_AllSupportedTypes_Succeeds)
{
    auto w = SpawnWorker("lua_engine.register_slot_type_all_supported_types",
                         {unique_dir("all_types")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, Alias_SlotFrame_Producer)
{
    auto w = SpawnWorker("lua_engine.alias_slot_frame_producer",
                         {unique_dir("alias_prod")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, Alias_SlotFrame_Consumer)
{
    auto w = SpawnWorker("lua_engine.alias_slot_frame_consumer",
                         {unique_dir("alias_cons")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, Alias_NoAlias_Processor)
{
    auto w = SpawnWorker("lua_engine.alias_no_alias_processor",
                         {unique_dir("alias_proc")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, Alias_FlexFrame_Producer)
{
    auto w = SpawnWorker("lua_engine.alias_flex_frame_producer",
                         {unique_dir("alias_flex")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, Alias_ProducerNoFz_NoFlexFrameAlias)
{
    auto w = SpawnWorker("lua_engine.alias_producer_no_fz_no_flex_frame_alias",
                         {unique_dir("alias_no_fz")});
    ExpectWorkerOk(w);
}

// ============================================================================
// 3. invoke_produce
// ============================================================================

// ============================================================================
// Chunk 2 — invoke_produce (Pattern 3)
//
// Converted and strengthened. Bodies live in workers/lua_engine_workers.cpp.
// See the header comment on each TEST_F for what was strengthened over
// the pre-conversion test and why.
// ============================================================================

TEST_F(LuaEngineIsolatedTest, InvokeProduce_CommitOnTrue)
{
    // Strengthened: additionally asserts script_error_count == 0 (a Commit
    // path that silently logged a script error would slip through the
    // original body's check).
    auto w = SpawnWorker("lua_engine.invoke_produce_commit_on_true",
                         {unique_dir("produce_commit")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, InvokeProduce_DiscardOnFalse)
{
    // Strengthened: buf is initialized to the sentinel value 777.0f; the
    // test asserts the engine did NOT overwrite it on the Discard path
    // (the original test initialized buf = 0.0f and couldn't tell the
    // difference between "engine left it alone" and "engine wrote 0.0").
    auto w = SpawnWorker("lua_engine.invoke_produce_discard_on_false",
                         {unique_dir("produce_discard")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, InvokeProduce_NilReturn_IsError)
{
    auto w = SpawnWorker("lua_engine.invoke_produce_nil_return_is_error",
                         {unique_dir("produce_nil_return")});
    // Engine logs: "on_produce returned nil — explicit 'return true' or
    // 'return false' is required. Treating as error." — declare the
    // exact text fragment so a reworded diagnostic (which would still
    // return Error from invoke_produce, keeping the body green) is
    // caught at the parent level.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/
                   {"on_produce returned nil"});
}

TEST_F(LuaEngineIsolatedTest, InvokeProduce_NilSlot)
{
    // Strengthened: additionally asserts script_error_count == 0 to
    // confirm the Lua-side assert(tx.slot == nil, ...) actually passed.
    // A failing Lua assert would bump the count but still return Discard
    // from the engine — the original test couldn't distinguish the two.
    auto w = SpawnWorker("lua_engine.invoke_produce_nil_slot",
                         {unique_dir("produce_nil_slot")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, InvokeProduce_ScriptError)
{
    auto w = SpawnWorker("lua_engine.invoke_produce_script_error",
                         {unique_dir("produce_script_error")});
    // The engine logs the Lua error() message at ERROR level.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/{"intentional error"});
}

// NEW: pins the production contract that Discard does NOT roll back
// Lua-side writes to tx.slot. The Lua script writes tx.slot.value = 42.0
// then returns false; the worker verifies result == Discard AND
// buf == 42.0. Worth having explicitly because users may expect the
// engine to clear the buffer on Discard (it doesn't).
TEST_F(LuaEngineIsolatedTest, InvokeProduce_DiscardOnFalse_ButLuaWroteSlot)
{
    auto w = SpawnWorker(
        "lua_engine.invoke_produce_discard_on_false_but_lua_wrote_slot",
        {unique_dir("produce_discard_wrote")});
    ExpectWorkerOk(w);
}

// ============================================================================
// 4. invoke_consume
// ============================================================================

// ============================================================================
// Chunk 3 — invoke_consume (Pattern 3)
//
// Strengthened bodies in workers/lua_engine_workers.cpp.  Highlights:
//
//   - InvokeConsume_ReceivesSlot: renamed from ReceivesReadOnlySlot (the
//     original body never actually attempted a write to verify the
//     read-only claim).  Now asserts the return value too, not only
//     the script_error_count side-effect.
//
//   - InvokeConsume_RxSlot_IsReadOnly (NEW): actually pins the
//     "read-only slot" contract by attempting a Lua write to rx.slot
//     and verifying the underlying C buffer is unchanged afterwards.
// ============================================================================

TEST_F(LuaEngineIsolatedTest, InvokeConsume_ReceivesSlot)
{
    auto w = SpawnWorker("lua_engine.invoke_consume_receives_slot",
                         {unique_dir("consume_receives")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, InvokeConsume_NilSlot)
{
    auto w = SpawnWorker("lua_engine.invoke_consume_nil_slot",
                         {unique_dir("consume_nil_slot")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, InvokeConsume_ScriptErrorDetected)
{
    auto w = SpawnWorker("lua_engine.invoke_consume_script_error_detected",
                         {unique_dir("consume_script_error")});
    // Engine logs the Lua `error("consume error")` at ERROR level.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/{"consume error"});
}

// Design-enforcement test — pins that the library's rx read-only
// contract is implemented as a LOUD FAILURE:
//   - LuaJIT raises on the `const*` cdata write
//   - engine pcall path returns InvokeResult::Error
//   - script_error_count increments by 1
//   - ERROR log entry "[on_consume] Lua error: ..." is emitted
// Silent no-op would be a design regression (a buggy script would
// appear healthy while silently producing incorrect data downstream).
// See the worker body's doc block for the source-traced mechanism
// (lua_engine.cpp:697-699 + lua_state.cpp:285,291 + :843).
TEST_F(LuaEngineIsolatedTest, InvokeConsume_RxSlot_IsReadOnly)
{
    auto w = SpawnWorker("lua_engine.invoke_consume_rx_slot_is_read_only",
                         {unique_dir("consume_read_only")});
    // The ERROR log tag comes from src/scripting/lua_state.cpp:379
    // ("[{tag}] Lua error: ..."). Pinning the exact callback tag
    // catches a reworded diagnostic.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/
                   {"[on_consume] Lua error"});
}

// ============================================================================
// 5. invoke_process
// ============================================================================

// ============================================================================
// Chunk 4 — invoke_process (Pattern 3)
//
// Processor design note carried over from the worker doc block: a
// processor ALWAYS has an input channel. Nil rx.slot inside on_process
// means the input queue timed out (or upstream has no data this
// iteration), not "no input channel". Nil tx.slot means the output queue
// is backpressured / no slot currently available, not "no output
// channel". The renamed tests (BothSlotsNil, RxPresent_TxNil) describe
// the state, not a role interpretation, so the name stays accurate
// whichever runtime condition produced that state.
// ============================================================================

TEST_F(LuaEngineIsolatedTest, InvokeProcess_DualSlots)
{
    // Strengthened: also asserts rx input buffer unchanged (read-only in
    // the dual-slot path) and script_error_count == 0.
    auto w = SpawnWorker("lua_engine.invoke_process_dual_slots",
                         {unique_dir("process_dual")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, InvokeProcess_BothSlotsNil)
{
    // Renamed from InvokeProcess_NilInput to describe the slot-state
    // rather than a misreadable role-semantic. Both rx and tx arrive
    // nil; Lua asserts nil for both and returns Discard.
    auto w = SpawnWorker("lua_engine.invoke_process_both_slots_nil",
                         {unique_dir("process_both_nil")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, InvokeProcess_RxPresent_TxNil)
{
    // Renamed from InvokeProcess_InputOnlyNoOutput. Represents tx
    // backpressure while input data is available. Lua drops the
    // iteration (return false → Discard).
    auto w = SpawnWorker("lua_engine.invoke_process_rx_present_tx_nil",
                         {unique_dir("process_rx_tx_nil")});
    ExpectWorkerOk(w);
}

// Design-enforcement test (processor dual-slot variant). Same loud-
// failure contract as the consumer-path test above — the processor's
// invoke_process entry reuses the same ref_in_slot_readonly_ cached
// `const*` ctype (lua_engine.cpp:899-900). The Lua error on the rx
// write aborts the callback before the subsequent tx write, so
// out_data stays at its caller-initialised value.
TEST_F(LuaEngineIsolatedTest, InvokeProcess_RxSlot_IsReadOnly)
{
    auto w = SpawnWorker("lua_engine.invoke_process_rx_slot_is_read_only",
                         {unique_dir("process_rx_ro")});
    // Callback tag "on_process" in the ERROR log. Same rationale as the
    // consumer-path pin.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/
                   {"[on_process] Lua error"});
}

// ============================================================================
// 6. Messages
// ============================================================================

// ============================================================================
// Chunk 5 — Messages (Pattern 3)
//
// The Lua engine has two distinct message-table projection functions:
// push_messages_table_ (producer/processor) and push_messages_table_bare_
// (consumer). The bodies diverge only on DATA messages (empty event
// field). Event messages use the same flattened-details projection in
// both.  See the chunk-5 header comment in
// workers/lua_engine_workers.cpp for the full shape documentation.
//
// The pre-conversion suite had one test here that covered event
// messages only, and only checked the event field name — not the
// details map promotion. Three new tests fill the data-message,
// empty-vector, and consumer-bare-format coverage holes.
// ============================================================================

TEST_F(LuaEngineIsolatedTest, InvokeProduce_ReceivesMessages_EventWithDetails)
{
    // Strengthened from pre-conversion InvokeProduce_ReceivesMessages.
    // Now verifies the details-map-flattening contract — previously the
    // test set m1.details["identity"] = "abc123" but never checked Lua
    // saw it.
    auto w = SpawnWorker(
        "lua_engine.invoke_produce_receives_messages_event_with_details",
        {unique_dir("msgs_event")});
    ExpectWorkerOk(w);
}

// NEW: empty msgs vector — simplest edge case, not covered pre-sweep.
TEST_F(LuaEngineIsolatedTest, InvokeProduce_ReceivesMessages_EmptyVector)
{
    auto w = SpawnWorker(
        "lua_engine.invoke_produce_receives_messages_empty_vector",
        {unique_dir("msgs_empty")});
    ExpectWorkerOk(w);
}

// NEW: data-message shape on the producer path. Verifies the
// sender-to-hex projection and the data-to-byte-string projection
// (previously untested).
TEST_F(LuaEngineIsolatedTest, InvokeProduce_ReceivesMessages_DataMessage)
{
    auto w = SpawnWorker(
        "lua_engine.invoke_produce_receives_messages_data_message",
        {unique_dir("msgs_data_producer")});
    ExpectWorkerOk(w);
}

// NEW: data-message shape on the CONSUMER path — a fundamentally
// different projection (bare byte string at msgs[i], not a table).
// This is the push_messages_table_bare_ code path which was entirely
// untested at L2 before this test.
TEST_F(LuaEngineIsolatedTest, InvokeConsume_ReceivesMessages_DataBareFormat)
{
    auto w = SpawnWorker(
        "lua_engine.invoke_consume_receives_messages_data_bare_format",
        {unique_dir("msgs_data_consumer")});
    ExpectWorkerOk(w);
}

// ============================================================================
// 7. API closures
// ============================================================================

// ============================================================================
// Chunk 6a — api.* closures: introspection + control (Pattern 3)
//
// L2-scoped api.* closures only: version_info, uid, name, channel, log,
// stop, critical_error/set_critical_error/stop_reason. Queue-state, band
// pub/sub, and inbox closures are deferred to L3 — they need real
// ShmQueue/ZmqQueue/BrokerService infrastructure that belongs in
// tests/test_layer3_datahub/, not the engine-unit suite here.
//
// Chunk 6b will cover the remaining L2-eligible api.* closures: the
// metrics trio (report_metric/report_metrics/clear_custom_metrics) and
// the shared-data pair (get_shared_data/set_shared_data).
// ============================================================================

TEST_F(LuaEngineIsolatedTest, ApiVersionInfo_ReturnsJsonString)
{
    // Strengthened from ApiVersionInfo_ReturnsNonEmptyString. The
    // original only asserted type==string + length>10 + contains "{";
    // this pins that the string ALSO contains "}" (bracket balance) and
    // a "version" substring so a reworded/truncated output is caught.
    auto w = SpawnWorker("lua_engine.api_version_info_returns_json_string",
                         {unique_dir("api_version_info")});
    ExpectWorkerOk(w);
}

// NEW: identity getters round-trip from RoleAPIBase through Lua.
TEST_F(LuaEngineIsolatedTest, ApiIdentity_UidNameChannel)
{
    auto w = SpawnWorker("lua_engine.api_identity_uid_name_channel",
                         {unique_dir("api_identity")});
    ExpectWorkerOk(w);
}

// NEW: api.log(level, msg) level dispatch. The Lua body emits one
// ERROR + one WARN + one WARNING (WARN) + one INFO + one "unknown"
// (INFO fallback). Parent declares the one ERROR line so the
// framework's "no ERROR" check doesn't reject the test.
TEST_F(LuaEngineIsolatedTest, ApiLog_DispatchesLevels)
{
    auto w = SpawnWorker("lua_engine.api_log_dispatches_levels",
                         {unique_dir("api_log")});
    // Pins the exact tag format "[<log_tag>-lua] <msg>" from
    // lua_engine.cpp:1317-1325.  If the engine reworded the tag
    // template this substring would stop matching — intentional.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/
                   {"[test-lua] lua_error_msg"});
}

// NEW: api.stop() → core->request_stop() propagation.
TEST_F(LuaEngineIsolatedTest, ApiStop_SetsShutdownRequested)
{
    auto w = SpawnWorker("lua_engine.api_stop_sets_shutdown_requested",
                         {unique_dir("api_stop")});
    ExpectWorkerOk(w);
}

// NEW: api.critical_error (read) + api.set_critical_error (write with
// three side-effects) + api.stop_reason (read) round-trip.
TEST_F(LuaEngineIsolatedTest, ApiCriticalError_SetAndReadAndStopReason)
{
    auto w = SpawnWorker(
        "lua_engine.api_critical_error_set_and_read_and_stop_reason",
        {unique_dir("api_crit")});
    // set_critical_error logs the user-supplied message at ERROR level
    // (lua_engine.cpp:1342) — pin the exact format prefix.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/
                   {"[test-lua] CRITICAL:"});
}

// NEW: exhaustive api.stop_reason() → StopReason enum mapping. Injects
// each enum value from C++ and verifies the Lua closure returns the
// right string. Subsumes the V2 ApiStopReason_ReflectsPeerDead and
// ApiStopReason_AfterCriticalError tests AND adds HubDead coverage
// which was never tested before this.
TEST_F(LuaEngineIsolatedTest, ApiStopReason_ReflectsAllEnumValues)
{
    auto w = SpawnWorker(
        "lua_engine.api_stop_reason_reflects_all_enum_values",
        {unique_dir("api_stop_reason_all")});
    ExpectWorkerOk(w);
}

// ============================================================================
// Chunk 6b — api.* closures: custom metrics (Pattern 3)
//
// Covers: api.report_metric(key, value) / api.report_metrics({table}) /
// api.clear_custom_metrics() / readback via api.metrics().custom.
// All six tests fully subsume their V2 counterparts (deleted in the
// same commit per the delete-as-we-go policy established 2026-04-19).
// ============================================================================

TEST_F(LuaEngineIsolatedTest, ApiReportMetric_AppearsUnderCustom)
{
    auto w = SpawnWorker(
        "lua_engine.api_report_metric_appears_under_custom",
        {unique_dir("api_report_metric")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, ApiReportMetric_OverwriteSameKey)
{
    auto w = SpawnWorker(
        "lua_engine.api_report_metric_overwrite_same_key",
        {unique_dir("api_report_overwrite")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, ApiReportMetric_ZeroValuePreserved)
{
    auto w = SpawnWorker(
        "lua_engine.api_report_metric_zero_value_preserved",
        {unique_dir("api_report_zero")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, ApiReportMetrics_BatchAcceptsTable)
{
    auto w = SpawnWorker(
        "lua_engine.api_report_metrics_batch_accepts_table",
        {unique_dir("api_report_batch")});
    ExpectWorkerOk(w);
}

// Lua's luaL_checktype raises on wrong arg type; the engine surfaces
// that as a script error. The raised-error lua stacktrace message is
// emitted at ERROR level by the pcall catch path, so the framework
// needs to know about it.
TEST_F(LuaEngineIsolatedTest, ApiReportMetrics_NonTableArg_IsError)
{
    auto w = SpawnWorker(
        "lua_engine.api_report_metrics_non_table_arg_is_error",
        {unique_dir("api_report_wrongarg")});
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/
                   {"table expected"});
}

TEST_F(LuaEngineIsolatedTest, ApiClearCustomMetrics_EmptiesAndAllowsRewrite)
{
    auto w = SpawnWorker(
        "lua_engine.api_clear_custom_metrics_empties_and_allows_rewrite",
        {unique_dir("api_clear_custom")});
    ExpectWorkerOk(w);
}

// ============================================================================
// Chunk 6c — api.* closures: shared data (Pattern 3)
//
// Covers: api.get_shared_data / api.set_shared_data across all four
// variant types (int64/string/bool/double), missing-key,
// nil-removes-key, cross-type and same-type overwrite.
// Cross-thread visibility deferred to a later chunk covering
// multi-state / thread-state plumbing.
// ============================================================================

TEST_F(LuaEngineIsolatedTest, ApiSharedData_RoundTripAllVariantTypes)
{
    auto w = SpawnWorker(
        "lua_engine.api_shared_data_round_trip_all_variant_types",
        {unique_dir("api_sd_roundtrip")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, ApiSharedData_GetMissingKeyReturnsNil)
{
    auto w = SpawnWorker(
        "lua_engine.api_shared_data_get_missing_key_returns_nil",
        {unique_dir("api_sd_missing")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, ApiSharedData_NilRemovesKey)
{
    auto w = SpawnWorker(
        "lua_engine.api_shared_data_nil_removes_key",
        {unique_dir("api_sd_nil_remove")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, ApiSharedData_OverwriteChangesType)
{
    auto w = SpawnWorker(
        "lua_engine.api_shared_data_overwrite_changes_type",
        {unique_dir("api_sd_xtype")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, ApiSharedData_OverwriteChangesValueSameType)
{
    auto w = SpawnWorker(
        "lua_engine.api_shared_data_overwrite_changes_value_same_type",
        {unique_dir("api_sd_samevtype")});
    ExpectWorkerOk(w);
}

// ============================================================================
// Chunk 7a — error handling: runtime error surfacing (Pattern 3)
//
// Covers: script error counting, wrong return type, stop_on_script_error,
// on_init/on_stop/on_inbox errors, engine.eval syntax errors.
// Every test pins InvokeResult / InvokeStatus where V2 checked only
// script_error_count, plus post-error engine usability where applicable.
// ============================================================================

TEST_F(LuaEngineIsolatedTest, Invoke_MultipleErrors_CountAccumulates)
{
    auto w = SpawnWorker(
        "lua_engine.invoke_multiple_errors_count_accumulates",
        {unique_dir("err_accumulate")});
    // 5 raised Lua errors → 5 ERROR log lines from the engine's
    // pcall path.  Framework needs to know all of them are expected.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/
                   {"oops", "oops", "oops", "oops", "oops"});
}

TEST_F(LuaEngineIsolatedTest, InvokeProduce_WrongReturnType_IsError)
{
    auto w = SpawnWorker(
        "lua_engine.invoke_produce_wrong_return_type_is_error",
        {unique_dir("err_wrong_ret_type")});
    // Exact engine log at lua_engine.cpp:814:
    // "[{log_tag}] on_produce returned non-boolean type '...'"
    // Worker invokes TWICE → 2 ERROR lines, so list substring twice
    // under multiset semantics.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/
                   {"on_produce returned non-boolean type",
                    "on_produce returned non-boolean type"});
}

TEST_F(LuaEngineIsolatedTest, InvokeProduce_WrongReturnString_IsError)
{
    auto w = SpawnWorker(
        "lua_engine.invoke_produce_wrong_return_string_is_error",
        {unique_dir("err_wrong_ret_string")});
    // Worker invokes TWICE → 2 ERROR lines, list substring twice.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/
                   {"on_produce returned non-boolean type",
                    "on_produce returned non-boolean type"});
}

TEST_F(LuaEngineIsolatedTest, InvokeProduce_StopOnScriptError_SetsShutdown)
{
    auto w = SpawnWorker(
        "lua_engine.invoke_produce_stop_on_script_error_sets_shutdown",
        {unique_dir("err_stop_on_error")});
    // stop_on_script_error path emits TWO ERROR log lines:
    //   1. "[test] Lua error: ... intentional error"  (pcall path)
    //   2. "[test] stop_on_script_error: requesting shutdown ..."
    //      (on_pcall_error_ path, lua_engine.cpp:1040)
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/
                   {"intentional error",
                    "stop_on_script_error: requesting shutdown"});
}

TEST_F(LuaEngineIsolatedTest, InvokeOnInitOrStop_ScriptError_Accumulates)
{
    auto w = SpawnWorker(
        "lua_engine.invoke_on_init_or_stop_script_error_accumulates",
        {unique_dir("err_init_stop")});
    // Two raised errors (on_init, on_stop) → two ERROR log lines.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/
                   {"init failed", "stop failed"});
}

TEST_F(LuaEngineIsolatedTest, InvokeOnInbox_ScriptError_IncrementsCount)
{
    auto w = SpawnWorker(
        "lua_engine.invoke_on_inbox_script_error_increments_count",
        {unique_dir("err_inbox")});
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/
                   {"inbox failed"});
}

TEST_F(LuaEngineIsolatedTest, Eval_SyntaxError_ReturnsScriptError)
{
    auto w = SpawnWorker(
        "lua_engine.eval_syntax_error_returns_script_error",
        {unique_dir("err_eval_syntax")});
    // eval() uses luaL_dostring, not state_.pcall — the engine's
    // on_pcall_error_ (lua_engine.cpp:1033-1045) does NOT log the
    // error message, only the stop_on_script_error notice if enabled.
    // So an eval syntax error bumps script_error_count but emits no
    // ERROR log line. No expected_error_substrings needed.
    ExpectWorkerOk(w);
}

// ============================================================================
// Chunk 7b — error handling: setup-phase error paths (Pattern 3)
//
// Covers load_script / register_slot_type error paths and a NEW
// finalize() idempotence gap-fill. Every setup-error test
// additionally asserts the engine is REUSABLE after the failure
// (pins "one bad setup input does not brick the engine").
// ============================================================================

TEST_F(LuaEngineIsolatedTest, LoadScript_MissingFile_ReturnsFalse)
{
    auto w = SpawnWorker(
        "lua_engine.load_script_missing_file_returns_false",
        {unique_dir("loadscript_missing_file")});
    // Engine logs "Script not found: <path>" at ERROR level
    // (lua_engine.cpp:217).
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/
                   {"Script not found"});
}

TEST_F(LuaEngineIsolatedTest, LoadScript_MissingRequiredCallback_ReturnsFalse)
{
    auto w = SpawnWorker(
        "lua_engine.load_script_missing_required_callback_returns_false",
        {unique_dir("loadscript_missing_cb")});
    // Engine logs "Script has no 'on_produce' function"
    // (lua_engine.cpp:237).
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/
                   {"Script has no 'on_produce' function"});
}

TEST_F(LuaEngineIsolatedTest, LoadScript_SyntaxError_ReturnsFalse)
{
    auto w = SpawnWorker(
        "lua_engine.load_script_syntax_error_returns_false",
        {unique_dir("loadscript_syntax")});
    // LuaJIT syntax error gets logged via state_.load_script
    // (see lua_state.cpp).  Substring varies by LuaJIT version;
    // "syntax" or "<eof>" are robust.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/
                   {"<eof>"});
}

TEST_F(LuaEngineIsolatedTest, RegisterSlotType_BadFieldType_ReturnsFalse)
{
    auto w = SpawnWorker(
        "lua_engine.register_slot_type_bad_field_type_returns_false",
        {unique_dir("reg_bad_type")});
    // build_ffi_cdef_ emits "Unsupported field type '<name>' in schema"
    // at ERROR level for each unsupported type. Two bad registrations
    // → two ERROR log lines, one per type name.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/
                   {"Unsupported field type 'complex128' in schema",
                    "Unsupported field type 'not_a_type' in schema"});
}

TEST_F(LuaEngineIsolatedTest, Finalize_DoubleCallIsSafe)
{
    auto w = SpawnWorker(
        "lua_engine.finalize_double_call_is_safe",
        {unique_dir("finalize_double")});
    // Post-finalize invoke_produce may log an error — acceptable if
    // it does, not pinning a specific substring (engine is in a dead
    // state, exact logging behavior is implementation-defined).
    ExpectWorkerOk(w);
}

// ============================================================================
// Chunk 8a — generic engine.invoke() / engine.eval() (Pattern 3)
//
// The role callbacks (invoke_produce/consume/process) are NOT the only
// engine entry points — scripts can expose arbitrary callable functions
// (e.g. on_heartbeat, band handlers) that role hosts dispatch via the
// generic invoke(name[, args]) API.  eval() runs ad-hoc Lua code and
// captures the result as JSON. Both have distinct contracts covered here.
// ============================================================================

TEST_F(LuaEngineIsolatedTest, Invoke_ExistingFunction_ReturnsTrue)
{
    auto w = SpawnWorker(
        "lua_engine.invoke_existing_function_returns_true",
        {unique_dir("invoke_exist")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, Invoke_NonExistentFunction_ReturnsFalse)
{
    auto w = SpawnWorker(
        "lua_engine.invoke_non_existent_function_returns_false",
        {unique_dir("invoke_nx")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, Invoke_EmptyName_ReturnsFalse)
{
    auto w = SpawnWorker(
        "lua_engine.invoke_empty_name_returns_false",
        {unique_dir("invoke_empty")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, Invoke_ScriptError_ReturnsFalseAndIncrementsErrors)
{
    auto w = SpawnWorker(
        "lua_engine.invoke_script_error_returns_false_and_increments_errors",
        {unique_dir("invoke_script_err")});
    // lua_state.cpp:379 logs "[<tag>] Lua error: <file>:<line>: <msg>"
    // when pcall fails. generic invoke uses the pcall path too.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/
                   {"intentional test error"});
}

TEST_F(LuaEngineIsolatedTest, Invoke_WithArgs_ReturnsTrue)
{
    auto w = SpawnWorker(
        "lua_engine.invoke_with_args_returns_true",
        {unique_dir("invoke_args")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, Invoke_AfterFinalize_ReturnsFalse)
{
    auto w = SpawnWorker(
        "lua_engine.invoke_after_finalize_returns_false",
        {unique_dir("invoke_finalized")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, Eval_ReturnsScalarResult)
{
    auto w = SpawnWorker(
        "lua_engine.eval_returns_scalar_result",
        {unique_dir("eval_scalar")});
    ExpectWorkerOk(w);
}

// ============================================================================
// Chunk 8b — multi-state / thread-state (Pattern 3)
//
// LuaEngine is multi-state: each non-owner thread that calls
// invoke()/eval() gets its own thread-local child engine. Lua globals
// are PER-STATE (do NOT leak between threads). RoleHostCore's
// shared_data IS thread-shared. These tests pin both invariants in
// both directions.
// ============================================================================

TEST_F(LuaEngineIsolatedTest, SupportsMultiState_ReturnsTrue)
{
    auto w = SpawnWorker(
        "lua_engine.supports_multi_state_returns_true",
        {unique_dir("multi_state")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, State_PersistsAcrossCalls)
{
    auto w = SpawnWorker(
        "lua_engine.state_persists_across_calls",
        {unique_dir("state_persist")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, Invoke_FromNonOwnerThread_Works)
{
    auto w = SpawnWorker(
        "lua_engine.invoke_from_non_owner_thread_works",
        {unique_dir("invoke_nonowner")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, Invoke_NonOwnerThread_UsesIndependentState)
{
    auto w = SpawnWorker(
        "lua_engine.invoke_non_owner_thread_uses_independent_state",
        {unique_dir("indep_state")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, Invoke_ConcurrentOwnerAndNonOwner)
{
    auto w = SpawnWorker(
        "lua_engine.invoke_concurrent_owner_and_non_owner",
        {unique_dir("concurrent")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, SharedData_CrossThread_Visible)
{
    auto w = SpawnWorker(
        "lua_engine.shared_data_cross_thread_visible",
        {unique_dir("sd_xthread")});
    ExpectWorkerOk(w);
}

// ============================================================================
// Chunk 8c — misc V2 leftovers (Pattern 3)
//
// Two more conversions; one straight V2 deletion (no P3 needed because
// already covered by chunk-1 FullLifecycle + chunk-6c shared_data tests).
// ============================================================================

TEST_F(LuaEngineIsolatedTest, HasCallback_DetectsPresenceAbsence)
{
    auto w = SpawnWorker(
        "lua_engine.has_callback_detects_presence_absence",
        {unique_dir("has_callback")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, InvokeConsume_Messages_DataAndEventMixed)
{
    auto w = SpawnWorker(
        "lua_engine.invoke_consume_messages_data_and_event_mixed",
        {unique_dir("consume_mixed")});
    ExpectWorkerOk(w);
}

// ============================================================================
// Chunk 9a — inbox + slot-only invoke (Pattern 3)
//
// Inbox is a separate engine entry point (invoke_on_inbox) with its
// own typed slot view (InboxFrame). Also covers the contract that
// InvokeTx carries a slot only (flexzone is closure-bound at
// build_api time, not invoke time).
// ============================================================================

TEST_F(LuaEngineIsolatedTest, InvokeProduce_SlotOnly_NoFlexzoneOnInvoke)
{
    auto w = SpawnWorker(
        "lua_engine.invoke_produce_slot_only_no_flexzone_on_invoke",
        {unique_dir("slot_only_no_fz")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, InvokeOnInbox_TypedData)
{
    auto w = SpawnWorker(
        "lua_engine.invoke_on_inbox_typed_data",
        {unique_dir("inbox_typed")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, TypeSizeof_InboxFrame_ReturnsCorrectSize)
{
    auto w = SpawnWorker(
        "lua_engine.type_sizeof_inbox_frame_returns_correct_size",
        {unique_dir("sizeof_inbox")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, InvokeOnInbox_MissingType_ReportsError)
{
    auto w = SpawnWorker(
        "lua_engine.invoke_on_inbox_missing_type_reports_error",
        {unique_dir("inbox_missing_type")});
    // Engine logs at lua_engine.cpp:965-967:
    //   "[<tag>] invoke_on_inbox: InboxFrame type not registered ..."
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/
                   {"InboxFrame type not registered"});
}

// ============================================================================
// Chunk 9b — logical-size accessors via engine_lifecycle_startup (Pattern 3)
//
// api.slot_logical_size() / api.flexzone_logical_size() Lua closures
// read schema-derived sizes from RoleHostCore. Tests use the
// production setup pathway (engine_lifecycle_startup, same one role
// hosts use) and pass expected sizes through shared_data so the Lua
// assertion uses authoritative compute_schema_size, not literals.
// ============================================================================

TEST_F(LuaEngineIsolatedTest, SlotLogicalSize_Aligned_PaddingSensitive)
{
    auto w = SpawnWorker(
        "lua_engine.slot_logical_size_aligned_padding_sensitive",
        {unique_dir("slot_aligned")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, SlotLogicalSize_Packed_NoPadding)
{
    auto w = SpawnWorker(
        "lua_engine.slot_logical_size_packed_no_padding",
        {unique_dir("slot_packed")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, SlotLogicalSize_ComplexMixed_Aligned)
{
    auto w = SpawnWorker(
        "lua_engine.slot_logical_size_complex_mixed_aligned",
        {unique_dir("slot_complex")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, FlexzoneLogicalSize_ArrayFields)
{
    auto w = SpawnWorker(
        "lua_engine.flexzone_logical_size_array_fields",
        {unique_dir("fz_array")});
    ExpectWorkerOk(w);
}

// ============================================================================
// Chunk 10 — graceful degradation: api.* closures without infrastructure
//
// L2 engine usable without broker / SHM / real queues. The closures
// that need those resources must degrade gracefully (return nil /
// false / 0) or raise a pcall-catchable error — never crash, never
// emit ERROR-level logs.  Each test invokes its closure twice in
// the Lua body (idempotence pin, V2 only invoked once).
// ============================================================================

TEST_F(LuaEngineIsolatedTest, Api_OpenInbox_WithoutBroker_ReturnsNil)
{
    auto w = SpawnWorker(
        "lua_engine.api_open_inbox_without_broker_returns_nil",
        {unique_dir("open_inbox_no_broker")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, Api_BandJoin_WithoutBroker_ReturnsNil)
{
    auto w = SpawnWorker(
        "lua_engine.api_band_join_without_broker_returns_nil",
        {unique_dir("band_join_no_broker")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, Api_BandLeave_WithoutBroker_ReturnsFalse)
{
    auto w = SpawnWorker(
        "lua_engine.api_band_leave_without_broker_returns_false",
        {unique_dir("band_leave_no_broker")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, Api_BandBroadcast_WithoutBroker_NoError)
{
    auto w = SpawnWorker(
        "lua_engine.api_band_broadcast_without_broker_no_error",
        {unique_dir("band_bcast_no_broker")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, Api_BandMembers_WithoutBroker_ReturnsNil)
{
    auto w = SpawnWorker(
        "lua_engine.api_band_members_without_broker_returns_nil",
        {unique_dir("band_members_no_broker")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, Api_SpinlockCount_WithoutSHM_ReturnsZero)
{
    auto w = SpawnWorker(
        "lua_engine.api_spinlock_count_without_shm_returns_zero",
        {unique_dir("spinlock_count_no_shm")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, Api_SpinlockAcquire_WithoutSHM_IsPcallError)
{
    auto w = SpawnWorker(
        "lua_engine.api_spinlock_acquire_without_shm_is_pcall_error",
        {unique_dir("spinlock_acq_no_shm")});
    // pcall-caught error must NOT propagate as a [LOGGER ERROR ] line.
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, Api_FlexzoneAccessor_WithoutSHM_ReturnsNil)
{
    auto w = SpawnWorker(
        "lua_engine.api_flexzone_accessor_without_shm_returns_nil",
        {unique_dir("flexzone_acc_no_shm")});
    ExpectWorkerOk(w);
}

// ============================================================================
// Chunk 11 — metrics tests (Pattern 3)
//
// Live read semantics, hierarchical table shape, anchored values for
// every loop field (including acquire_retry_count V2 missed),
// inventory check on m.loop keys, script_error_count transition pin.
// ============================================================================

TEST_F(LuaEngineIsolatedTest, Metrics_IndividualAccessors_ReadCoreCounters_Live)
{
    auto w = SpawnWorker(
        "lua_engine.metrics_individual_accessors_read_core_counters_live",
        {unique_dir("metrics_indiv_live")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, Metrics_InSlotsReceived_Works_Consumer)
{
    auto w = SpawnWorker(
        "lua_engine.metrics_in_slots_received_works_consumer",
        {unique_dir("metrics_inrx_consumer")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, Metrics_HierarchicalTable_Producer_FullShape)
{
    auto w = SpawnWorker(
        "lua_engine.metrics_hierarchical_table_producer_full_shape",
        {unique_dir("metrics_ht_prod_full")});
    // Phase 0 deliberately raises a Lua error to seed
    // script_error_count == 1 — that error message must be expected.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/
                   {"seed for script_error_count"});
}

TEST_F(LuaEngineIsolatedTest, Metrics_HierarchicalTable_Consumer_FullShape)
{
    auto w = SpawnWorker(
        "lua_engine.metrics_hierarchical_table_consumer_full_shape",
        {unique_dir("metrics_ht_cons_full")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, Metrics_LoopOverrunCount_LiveIncrements)
{
    auto w = SpawnWorker(
        "lua_engine.metrics_loop_overrun_count_live_increments",
        {unique_dir("metrics_overrun_live")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, Metrics_LastCycleWorkUs_OverwriteSemantics)
{
    auto w = SpawnWorker(
        "lua_engine.metrics_last_cycle_work_us_overwrite_semantics",
        {unique_dir("metrics_lcwu_overwrite")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, Metrics_AllLoopFields_AnchoredValues)
{
    auto w = SpawnWorker(
        "lua_engine.metrics_all_loop_fields_anchored_values",
        {unique_dir("metrics_all_loop_anchored")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineIsolatedTest, Metrics_RoleScriptErrorCount_ReflectsRaisedError)
{
    auto w = SpawnWorker(
        "lua_engine.metrics_role_script_error_count_reflects_raised_error",
        {unique_dir("metrics_serc_reflects")});
    // 2 raised "seed phase N" errors expected.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/
                   {"seed phase 1", "seed phase 2"});
}

// ============================================================================
// 8. Error handling
// ============================================================================

// ============================================================================
// 11. Flexzone
// ============================================================================


// ============================================================================
// 18. Metrics closures read from RoleHostCore counters
// ============================================================================

// ============================================================================
// Generic invoke() tests
// ============================================================================

// ============================================================================
// Lua api.metrics() — hierarchical table structure
// ============================================================================

// ============================================================================
// New API closures — diagnostics, queue-state, custom metrics, environment
// ============================================================================

TEST_F(LuaEngineTest, Api_EnvironmentStrings_LogsDirRunDir)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            assert(api.log_level == "error", "log_level expected 'error'")
            assert(type(api.script_dir) == "string", "script_dir must be string")
            assert(type(api.role_dir) == "string", "role_dir must be string")
            assert(type(api.logs_dir) == "string", "logs_dir must be string")
            assert(type(api.run_dir) == "string", "run_dir must be string")
            return false
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce(InvokeTx{&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u);
    engine.finalize();
}

TEST_F(LuaEngineTest, Api_ConsumerQueueState_WithoutQueue)
{
    // Without a real consumer object, queue-state methods return safe defaults.
    write_script(R"(
        function on_consume(rx, msgs, api)
            assert(api.in_capacity() == 0, "in_capacity should be 0 without queue")
            assert(api.in_policy() == "", "in_policy should be '' without queue")
            assert(api.last_seq() == 0, "last_seq should be 0 without queue")
            return true
        end
    )");

    RoleHostCore core;
    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test", &core));
    ASSERT_TRUE(engine.load_script(tmp_, "init.lua", "on_consume"));

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

// ============================================================================
// 23. Queue-state defaults for producer and processor
// ============================================================================

TEST_F(LuaEngineTest, Api_ProducerQueueState_WithoutQueue)
{
    // Without a real producer object, queue-state methods return safe defaults.
    write_script(R"(
        function on_produce(tx, msgs, api)
            assert(api.out_capacity() == 0, "out_capacity should be 0 without queue")
            assert(api.out_policy() == "", "out_policy should be '' without queue")
            return false
        end
    )");

    RoleHostCore core;
    LuaEngine engine;
    ASSERT_TRUE(setup_engine_with_core(engine, core));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce(InvokeTx{&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u);
    engine.finalize();
}

TEST_F(LuaEngineTest, Api_ProcessorQueueState_DualDefaults)
{
    // Processor context with no real queues — all queue-state accessors return defaults.
    write_script(R"(
        function on_process(rx, tx, msgs, api)
            assert(api.in_capacity() == 0, "in_capacity should be 0")
            assert(api.out_capacity() == 0, "out_capacity should be 0")
            assert(api.in_policy() == "", "in_policy should be ''")
            assert(api.out_policy() == "", "out_policy should be ''")
            assert(api.last_seq() == 0, "last_seq should be 0")
            return false
        end
    )");

    RoleHostCore core;
    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test", &core));
    ASSERT_TRUE(engine.load_script(tmp_, "init.lua", "on_process"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "InSlotFrame", "aligned"));
    ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame", "aligned"));

    auto test_api = make_api(core, "proc");
    test_api->set_channel("test.in_channel");
    test_api->set_out_channel("test.out_channel");
    ASSERT_TRUE(engine.build_api(*test_api));

    float in_data = 1.0f;
    float out_data = 0.0f;
    std::vector<IncomingMessage> msgs;
    auto result = engine.invoke_process(
        InvokeRx{&in_data, sizeof(in_data)},
        InvokeTx{&out_data, sizeof(out_data)}, msgs);
    EXPECT_EQ(result, InvokeResult::Discard);
    EXPECT_EQ(engine.script_error_count(), 0u);
    engine.finalize();
}

// ============================================================================
// 26. Metrics — all loop fields present with non-zero verification
// ============================================================================

// ============================================================================
// 30. open_inbox without broker returns nil
// ============================================================================

// ============================================================================
// 31. Processor in_channel / out_channel match context
// ============================================================================

TEST_F(LuaEngineTest, Api_ProcessorChannels_InOut)
{
    write_script(R"(
        function on_process(rx, tx, msgs, api)
            assert(api.in_channel() == "sensor.input",
                   "in_channel mismatch: " .. tostring(api.in_channel()))
            assert(api.out_channel() == "sensor.output",
                   "out_channel mismatch: " .. tostring(api.out_channel()))
            return false
        end
    )");

    RoleHostCore core;
    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test", &core));
    ASSERT_TRUE(engine.load_script(tmp_, "init.lua", "on_process"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "InSlotFrame", "aligned"));
    ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame", "aligned"));

    auto test_api = make_api(core, "proc");
    test_api->set_channel("sensor.input");
    test_api->set_out_channel("sensor.output");
    ASSERT_TRUE(engine.build_api(*test_api));

    float in_data = 1.0f;
    float out_data = 0.0f;
    std::vector<IncomingMessage> msgs;
    auto result = engine.invoke_process(
        InvokeRx{&in_data, sizeof(in_data)},
        InvokeTx{&out_data, sizeof(out_data)}, msgs);
    EXPECT_EQ(result, InvokeResult::Discard);
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "Script assertion failed — processor channels do not match context";
    engine.finalize();
}

// ============================================================================
// Full engine startup — tests the EngineModuleParams startup/shutdown path
// ============================================================================

TEST_F(LuaEngineTest, FullStartup_Producer_SlotOnly)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            tx.slot.value = 77.0
            return true
        end
    )");

    LuaEngine engine;
    RoleHostCore core;

    pylabhub::scripting::EngineModuleParams params;
    params.engine           = &engine;
    params.tag              = "prod";
    auto api = make_api(core, "prod");
    params.api              = api.get();
    params.script_dir       = tmp_;
    params.entry_point      = "init.lua";
    params.required_callback = "on_produce";
    params.out_slot_spec    = simple_schema();
    params.out_packing      = "aligned";

    ASSERT_NO_THROW(pylabhub::scripting::engine_lifecycle_startup(nullptr, &params));

    // Engine should be fully ready — types registered, API built.
    EXPECT_GT(engine.type_sizeof("OutSlotFrame"), 0u);
    EXPECT_GT(engine.type_sizeof("SlotFrame"), 0u); // alias

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(result, InvokeResult::Commit);
    EXPECT_FLOAT_EQ(buf, 77.0f);

    // Shutdown should be idempotent.
    pylabhub::scripting::engine_lifecycle_shutdown(nullptr, &params);
    pylabhub::scripting::engine_lifecycle_shutdown(nullptr, &params); // second call is no-op
}

TEST_F(LuaEngineTest, FullStartup_Producer_SlotAndFlexzone)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            tx.slot.value = 10.0
            return true
        end
    )");

    LuaEngine engine;
    RoleHostCore core;

    pylabhub::scripting::EngineModuleParams params;
    params.engine           = &engine;
    params.tag              = "prod";
    auto api = make_api(core, "prod");
    params.api              = api.get();
    params.script_dir       = tmp_;
    params.entry_point      = "init.lua";
    params.required_callback = "on_produce";
    params.out_slot_spec    = simple_schema();
    params.out_fz_spec      = simple_schema();
    params.out_packing      = "aligned";

    // Role host computes fz size from schema and sets on core before engine startup.
    // Role host computes fz size from schema before engine startup.
    core.set_out_fz_spec(SchemaSpec{params.out_fz_spec},
                         pylabhub::hub::align_to_physical_page(
                             pylabhub::hub::compute_schema_size(params.out_fz_spec, params.out_packing)));

    ASSERT_NO_THROW(pylabhub::scripting::engine_lifecycle_startup(nullptr, &params));

    EXPECT_GT(engine.type_sizeof("OutSlotFrame"), 0u);
    EXPECT_GT(engine.type_sizeof("OutFlexFrame"), 0u);
    EXPECT_GT(engine.type_sizeof("FlexFrame"), 0u); // alias
    EXPECT_TRUE(core.has_tx_fz());
    EXPECT_GT(core.out_schema_fz_size(), 0u);

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

TEST_F(LuaEngineTest, FullStartup_Consumer)
{
    write_script(R"(
        function on_consume(rx, msgs, api)
            assert(rx.slot ~= nil, "expected slot")
            return true
        end
    )");

    LuaEngine engine;
    RoleHostCore core;

    pylabhub::scripting::EngineModuleParams params;
    params.engine           = &engine;
    params.tag              = "cons";
    auto api = make_api(core, "cons");
    params.api              = api.get();
    params.script_dir       = tmp_;
    params.entry_point      = "init.lua";
    params.required_callback = "on_consume";
    params.in_slot_spec     = simple_schema();
    params.in_packing       = "aligned";

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

TEST_F(LuaEngineTest, FullStartup_Processor)
{
    write_script(R"(
        function on_process(rx, tx, msgs, api)
            if rx.slot and tx.slot then
                tx.slot.value = rx.slot.value * 2.0
            end
            return true
        end
    )");

    LuaEngine engine;
    RoleHostCore core;

    pylabhub::scripting::EngineModuleParams params;
    params.engine           = &engine;
    params.tag              = "proc";
    auto api = make_api(core, "proc");
    params.api              = api.get();
    params.script_dir       = tmp_;
    params.entry_point      = "init.lua";
    params.required_callback = "on_process";
    params.in_slot_spec     = simple_schema();
    params.out_slot_spec    = simple_schema();
    params.in_packing       = "aligned";
    params.out_packing      = "aligned";

    ASSERT_NO_THROW(pylabhub::scripting::engine_lifecycle_startup(nullptr, &params));

    EXPECT_GT(engine.type_sizeof("InSlotFrame"), 0u);
    EXPECT_GT(engine.type_sizeof("OutSlotFrame"), 0u);
    // Processor: no aliases.
    EXPECT_EQ(engine.type_sizeof("SlotFrame"), 0u);

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
// FullStartup — multifield schema data round-trip (all three roles)
// ============================================================================

TEST_F(LuaEngineTest, FullStartup_Producer_Multifield)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            tx.slot.ts = 1.23456789
            tx.slot.flag = 0xAB
            tx.slot.count = -42
            return true
        end
    )");

    LuaEngine engine;
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
    params.script_dir        = tmp_;
    params.entry_point       = "init.lua";
    params.required_callback = "on_produce";
    params.out_slot_spec     = spec;
    params.out_packing       = "aligned";

    ASSERT_NO_THROW(pylabhub::scripting::engine_lifecycle_startup(nullptr, &params));
    EXPECT_EQ(engine.type_sizeof("OutSlotFrame"), 16u);

    struct { double ts; uint8_t flag; uint8_t pad[3]; int32_t count; } buf{};

    std::vector<IncomingMessage> msgs;
    auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(result, InvokeResult::Commit);
    EXPECT_EQ(engine.script_error_count(), 0u);

    EXPECT_DOUBLE_EQ(buf.ts, 1.23456789);
    EXPECT_EQ(buf.flag, 0xAB);
    EXPECT_EQ(buf.count, -42);

    pylabhub::scripting::engine_lifecycle_shutdown(nullptr, &params);
}

TEST_F(LuaEngineTest, FullStartup_Consumer_Multifield)
{
    write_script(R"(
        function on_consume(rx, msgs, api)
            assert(math.abs(rx.slot.ts - 9.87) < 0.001, "ts=" .. tostring(rx.slot.ts))
            assert(rx.slot.flag == 0xCD, "flag=" .. tostring(rx.slot.flag))
            assert(rx.slot.count == 100, "count=" .. tostring(rx.slot.count))
            return true
        end
    )");

    LuaEngine engine;
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
    params.script_dir        = tmp_;
    params.entry_point       = "init.lua";
    params.required_callback = "on_consume";
    params.in_slot_spec      = spec;
    params.in_packing        = "aligned";

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

TEST_F(LuaEngineTest, FullStartup_Processor_Multifield)
{
    write_script(R"(
        function on_process(rx, tx, msgs, api)
            tx.slot.ts = rx.slot.ts
            tx.slot.flag = rx.slot.flag
            tx.slot.count = rx.slot.count * 2
            return true
        end
    )");

    LuaEngine engine;
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
    params.script_dir        = tmp_;
    params.entry_point       = "init.lua";
    params.required_callback = "on_process";
    params.in_slot_spec      = spec;
    params.out_slot_spec     = spec;
    params.in_packing        = "aligned";
    params.out_packing       = "aligned";

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

} // anonymous namespace
