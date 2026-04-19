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
 *   - LuaEngineChunk1Test  (IsolatedProcessTest — Pattern 3)
 *     Tests already converted by chunks 1-3 of the Lua sweep. Each
 *     spawns a worker subprocess; bodies live in
 *     workers/lua_engine_workers.cpp.
 *
 *   - LuaEngineTest        (plain ::testing::Test — V2 antipattern)
 *     Tests NOT YET converted. Will be migrated chunk-by-chunk by
 *     subsequent commits (one chunk per thematic group in the numbered
 *     list above). The V2 fixture will be deleted in the final commit
 *     of the Lua sweep, at which point this file contains Pattern 3
 *     only.
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

class LuaEngineChunk1Test : public pylabhub::tests::IsolatedProcessTest
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

TEST_F(LuaEngineChunk1Test, FullLifecycle)
{
    // Strengthened: the worker verifies on_init and on_stop actually
    // dispatched into the Lua runtime by checking report_metric side
    // effects on RoleHostCore.custom_metrics_snapshot(). See
    // workers/lua_engine_workers.cpp::full_lifecycle for the full body.
    auto w = SpawnWorker("lua_engine.full_lifecycle",
                         {unique_dir("full_lifecycle")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineChunk1Test, InitializeAndFinalize_Succeeds)
{
    // Renamed from InitializeFailsGracefully — body never matched that
    // name. A real initialize-fails-gracefully test needs a failure-
    // injection hook (e.g. simulate luaL_newstate() returning nullptr);
    // adding that hook is queued as a follow-up.
    auto w = SpawnWorker("lua_engine.initialize_and_finalize_succeeds",
                         {unique_dir("init_finalize")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineChunk1Test, RegisterSlotType_SizeofCorrect)
{
    auto w = SpawnWorker("lua_engine.register_slot_type_sizeof_correct",
                         {unique_dir("sizeof")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineChunk1Test, RegisterSlotType_MultiField)
{
    auto w = SpawnWorker("lua_engine.register_slot_type_multi_field",
                         {unique_dir("multifield")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineChunk1Test, RegisterSlotType_Packed_vs_Aligned)
{
    // Strengthened from RegisterSlotType_PackedPacking: verifies BOTH
    // aligned (8 bytes) and packed (5 bytes) for the same schema, and
    // explicitly asserts the sizes differ so a silent packing-arg-ignored
    // regression cannot slip by.
    auto w = SpawnWorker("lua_engine.register_slot_type_packed_vs_aligned",
                         {unique_dir("packed_vs_aligned")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineChunk1Test, RegisterSlotType_HasSchemaFalse_ReturnsFalse)
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
TEST_F(LuaEngineChunk1Test, RegisterSlotType_AllSupportedTypes_Succeeds)
{
    auto w = SpawnWorker("lua_engine.register_slot_type_all_supported_types",
                         {unique_dir("all_types")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineChunk1Test, Alias_SlotFrame_Producer)
{
    auto w = SpawnWorker("lua_engine.alias_slot_frame_producer",
                         {unique_dir("alias_prod")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineChunk1Test, Alias_SlotFrame_Consumer)
{
    auto w = SpawnWorker("lua_engine.alias_slot_frame_consumer",
                         {unique_dir("alias_cons")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineChunk1Test, Alias_NoAlias_Processor)
{
    auto w = SpawnWorker("lua_engine.alias_no_alias_processor",
                         {unique_dir("alias_proc")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineChunk1Test, Alias_FlexFrame_Producer)
{
    auto w = SpawnWorker("lua_engine.alias_flex_frame_producer",
                         {unique_dir("alias_flex")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineChunk1Test, Alias_ProducerNoFz_NoFlexFrameAlias)
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

TEST_F(LuaEngineChunk1Test, InvokeProduce_CommitOnTrue)
{
    // Strengthened: additionally asserts script_error_count == 0 (a Commit
    // path that silently logged a script error would slip through the
    // original body's check).
    auto w = SpawnWorker("lua_engine.invoke_produce_commit_on_true",
                         {unique_dir("produce_commit")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineChunk1Test, InvokeProduce_DiscardOnFalse)
{
    // Strengthened: buf is initialized to the sentinel value 777.0f; the
    // test asserts the engine did NOT overwrite it on the Discard path
    // (the original test initialized buf = 0.0f and couldn't tell the
    // difference between "engine left it alone" and "engine wrote 0.0").
    auto w = SpawnWorker("lua_engine.invoke_produce_discard_on_false",
                         {unique_dir("produce_discard")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineChunk1Test, InvokeProduce_NilReturn_IsError)
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

TEST_F(LuaEngineChunk1Test, InvokeProduce_NilSlot)
{
    // Strengthened: additionally asserts script_error_count == 0 to
    // confirm the Lua-side assert(tx.slot == nil, ...) actually passed.
    // A failing Lua assert would bump the count but still return Discard
    // from the engine — the original test couldn't distinguish the two.
    auto w = SpawnWorker("lua_engine.invoke_produce_nil_slot",
                         {unique_dir("produce_nil_slot")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineChunk1Test, InvokeProduce_ScriptError)
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
TEST_F(LuaEngineChunk1Test, InvokeProduce_DiscardOnFalse_ButLuaWroteSlot)
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

TEST_F(LuaEngineChunk1Test, InvokeConsume_ReceivesSlot)
{
    auto w = SpawnWorker("lua_engine.invoke_consume_receives_slot",
                         {unique_dir("consume_receives")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineChunk1Test, InvokeConsume_NilSlot)
{
    auto w = SpawnWorker("lua_engine.invoke_consume_nil_slot",
                         {unique_dir("consume_nil_slot")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineChunk1Test, InvokeConsume_ScriptErrorDetected)
{
    auto w = SpawnWorker("lua_engine.invoke_consume_script_error_detected",
                         {unique_dir("consume_script_error")});
    // Engine logs the Lua `error("consume error")` at ERROR level.
    ExpectWorkerOk(w, /*required=*/{},
                   /*expected_error_substrings=*/{"consume error"});
}

// NEW: concretely verifies the "read-only slot" part of the consumer
// contract that was named but not tested by the pre-conversion
// ReceivesReadOnlySlot test.
TEST_F(LuaEngineChunk1Test, InvokeConsume_RxSlot_IsReadOnly)
{
    auto w = SpawnWorker("lua_engine.invoke_consume_rx_slot_is_read_only",
                         {unique_dir("consume_read_only")});
    // Verified against the actual log format in
    // src/scripting/lua_state.cpp:379 → "[{tag}] Lua error: {err}" with
    // tag = "on_consume" for this callback path.  The substring
    // "[on_consume] Lua error" is narrow enough that a reworded
    // diagnostic (e.g. dropping the callback tag) would be caught.
    //
    // The engine raises a Lua error on the attempted write to the
    // const-qualified ffi pointer — confirmed observationally.  The
    // body's EXPECT_FLOAT_EQ(data, 99.5f) is the load-bearing invariant
    // either way; the log assertion strengthens the test by also
    // confirming the error was the expected kind, not some other.
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

TEST_F(LuaEngineChunk1Test, InvokeProcess_DualSlots)
{
    // Strengthened: also asserts rx input buffer unchanged (read-only in
    // the dual-slot path) and script_error_count == 0.
    auto w = SpawnWorker("lua_engine.invoke_process_dual_slots",
                         {unique_dir("process_dual")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineChunk1Test, InvokeProcess_BothSlotsNil)
{
    // Renamed from InvokeProcess_NilInput to describe the slot-state
    // rather than a misreadable role-semantic. Both rx and tx arrive
    // nil; Lua asserts nil for both and returns Discard.
    auto w = SpawnWorker("lua_engine.invoke_process_both_slots_nil",
                         {unique_dir("process_both_nil")});
    ExpectWorkerOk(w);
}

TEST_F(LuaEngineChunk1Test, InvokeProcess_RxPresent_TxNil)
{
    // Renamed from InvokeProcess_InputOnlyNoOutput. Represents tx
    // backpressure while input data is available. Lua drops the
    // iteration (return false → Discard).
    auto w = SpawnWorker("lua_engine.invoke_process_rx_present_tx_nil",
                         {unique_dir("process_rx_tx_nil")});
    ExpectWorkerOk(w);
}

// NEW: rx read-only contract in the processor's dual-slot code path
// (invoke_process is a separate engine entry from invoke_consume, so
// the consumer-path chunk 3 test does not cover this code).
TEST_F(LuaEngineChunk1Test, InvokeProcess_RxSlot_IsReadOnly)
{
    auto w = SpawnWorker("lua_engine.invoke_process_rx_slot_is_read_only",
                         {unique_dir("process_rx_ro")});
    // Same rationale as the consumer-path variant: the engine may
    // either raise a Lua error on the const-write attempt or LuaJIT
    // may silently no-op it. The callback log tag here is "on_process".
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

TEST_F(LuaEngineChunk1Test, InvokeProduce_ReceivesMessages_EventWithDetails)
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
TEST_F(LuaEngineChunk1Test, InvokeProduce_ReceivesMessages_EmptyVector)
{
    auto w = SpawnWorker(
        "lua_engine.invoke_produce_receives_messages_empty_vector",
        {unique_dir("msgs_empty")});
    ExpectWorkerOk(w);
}

// NEW: data-message shape on the producer path. Verifies the
// sender-to-hex projection and the data-to-byte-string projection
// (previously untested).
TEST_F(LuaEngineChunk1Test, InvokeProduce_ReceivesMessages_DataMessage)
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
TEST_F(LuaEngineChunk1Test, InvokeConsume_ReceivesMessages_DataBareFormat)
{
    auto w = SpawnWorker(
        "lua_engine.invoke_consume_receives_messages_data_bare_format",
        {unique_dir("msgs_data_consumer")});
    ExpectWorkerOk(w);
}

// ============================================================================
// 7. API closures
// ============================================================================

TEST_F(LuaEngineTest, ApiVersionInfo_ReturnsNonEmptyString)
{
    // Script asserts that version_info returns a non-empty string containing '{'.
    write_script(R"(
        function on_produce(tx, msgs, api)
            local info = api.version_info()
            assert(type(info) == "string", "version_info should return string")
            assert(#info > 10, "version_info too short: " .. info)
            assert(info:find("{") ~= nil, "version_info should be JSON: " .. info)
            return false
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(result, InvokeResult::Discard);
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "Script assertion failed — version_info returned unexpected value";

    engine.finalize();
}

// ============================================================================
// 8. Error handling
// ============================================================================

TEST_F(LuaEngineTest, MultipleErrors_CountAccumulates)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            error("oops")
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;

    for (int i = 0; i < 5; ++i)
    {
        engine.invoke_produce(InvokeTx{&buf, sizeof(buf)}, msgs);
    }
    EXPECT_EQ(engine.script_error_count(), 5u);

    engine.finalize();
}

// ============================================================================
// 9. supports_multi_state
// ============================================================================

TEST_F(LuaEngineTest, SupportsMultiState_ReturnsTrue)
{
    write_script("function on_produce(tx, msgs, api) return true end");
    LuaEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    EXPECT_TRUE(engine.supports_multi_state());
    engine.finalize();
}

// ============================================================================
// 10. has_callback
// ============================================================================

TEST_F(LuaEngineTest, HasCallback_DetectsPresenceAbsence)
{
    write_script(R"(
        function on_produce(tx, msgs, api) return true end
        function on_init(api) end
        -- on_stop intentionally absent
    )");

    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_, "init.lua", "on_produce"));

    EXPECT_TRUE(engine.has_callback("on_produce"));
    EXPECT_TRUE(engine.has_callback("on_init"));
    EXPECT_FALSE(engine.has_callback("on_stop"));
    EXPECT_FALSE(engine.has_callback("on_consume"));

    engine.finalize();
}

// ============================================================================
// 11. Flexzone
// ============================================================================

TEST_F(LuaEngineTest, InvokeProduce_SlotOnly_NoFlexzoneOnInvoke)
{
    // Script writes to slot only — InvokeTx no longer carries flexzone.
    write_script(R"(
        function on_produce(tx, msgs, api)
            assert(tx.slot ~= nil, "expected slot")
            tx.slot.value = 10.0
            return true
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_, "init.lua", "on_produce"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame", "aligned"));
    ASSERT_TRUE(engine.register_slot_type(spec, "OutFlexFrame", "aligned"));

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
// 12. invoke_on_inbox
// ============================================================================

TEST_F(LuaEngineTest, InvokeOnInbox_TypedData)
{
    // Script asserts the inbox data value and sender string.
    write_script(R"(
        function on_produce(tx, msgs, api) return false end
        function on_inbox(msg, api)
            assert(msg.data ~= nil, "expected inbox data")
            assert(math.abs(msg.data.value - 77.0) < 0.01,
                   "expected ~77.0, got " .. tostring(msg.data.value))
            assert(msg.sender_uid == "PROD-SENDER-00000001",
                   "expected sender UID, got " .. tostring(msg.sender_uid))
            return true
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_, "init.lua", "on_produce"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame", "aligned"));
    ASSERT_TRUE(engine.register_slot_type(spec, "InboxFrame", "aligned"));

    auto test_api = make_api(default_core_);
    ASSERT_TRUE(engine.build_api(*test_api));

    float inbox_data = 77.0f;
    engine.invoke_on_inbox({&inbox_data, sizeof(inbox_data), "PROD-SENDER-00000001", 1});
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "Script assertion failed — inbox data or sender incorrect";

    engine.finalize();
}

TEST_F(LuaEngineTest, TypeSizeof_InboxFrame_ReturnsCorrectSize)
{
    // Multi-type schema with alignment padding:
    //   uint8 flag (1) + pad(3) + float64 value (8) + uint16 count (2) +
    //   pad(2) + int32 status (4) + char[5] label (5) + pad(3) = 28 bytes aligned
    write_script(R"(
        function on_produce(tx, msgs, api) return true end
    )");

    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_, "init.lua", "on_produce"));

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

    size_t slot_sz  = engine.type_sizeof("SlotFrame");
    size_t inbox_sz = engine.type_sizeof("InboxFrame");
    EXPECT_GT(slot_sz, 0u) << "SlotFrame size must be > 0";
    EXPECT_GT(inbox_sz, 0u) << "InboxFrame size must be > 0";
    EXPECT_EQ(slot_sz, inbox_sz) << "Same schema → same size";
    // Aligned: 1 + 7pad + 8 + 2 + 2pad + 4 + 5 + 3pad = 32
    EXPECT_EQ(inbox_sz, 32u) << "Expected 28 bytes with alignment padding";

    engine.finalize();
}

TEST_F(LuaEngineTest, InvokeOnInbox_MissingType_ReportsError)
{
    // If InboxFrame is not registered, invoke_on_inbox must report an error.
    write_script(R"(
        function on_produce(tx, msgs, api) return false end
        function on_inbox(msg, api) return true end
    )");

    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_, "init.lua", "on_produce"));

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
// 13. State persistence across calls
// ============================================================================

TEST_F(LuaEngineTest, StatePersistsAcrossCalls)
{
    // Script maintains a counter across multiple invoke calls.
    write_script(R"(
        call_count = 0
        function on_produce(tx, msgs, api)
            call_count = call_count + 1
            if tx.slot then
                tx.slot.value = call_count
            end
            return true
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    std::vector<IncomingMessage> msgs;

    // Call 3 times — counter should increment.
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
// 14. Consumer bare messages format
// ============================================================================

TEST_F(LuaEngineTest, InvokeConsume_BareDataMessages)
{
    // Consumer data messages are bare byte strings (not {sender, data} tables).
    // Event messages are still tables.
    write_script(R"(
        function on_consume(rx, msgs, api)
            assert(#msgs == 2, "expected 2 messages, got " .. #msgs)
            -- First msg: data message → bare bytes string
            assert(type(msgs[1]) == "string", "data msg should be string, got " .. type(msgs[1]))
            -- Second msg: event message → table with .event
            assert(type(msgs[2]) == "table", "event msg should be table")
            assert(msgs[2].event == "channel_closing")
            return true
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_, "init.lua", "on_consume"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "InSlotFrame", "aligned"));

    auto test_api = make_api(default_core_, "cons");
    ASSERT_TRUE(engine.build_api(*test_api));

    std::vector<IncomingMessage> msgs;
    // Data message (has sender + data, no event).
    IncomingMessage dm;
    dm.sender = "sender-id";
    dm.data   = {std::byte{0x41}, std::byte{0x42}};
    msgs.push_back(std::move(dm));

    // Event message.
    IncomingMessage em;
    em.event = "channel_closing";
    msgs.push_back(std::move(em));

    engine.invoke_consume(InvokeRx{nullptr, 0}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "Script assertion failed — consumer message format incorrect";

    engine.finalize();
}

// ============================================================================
// 15. api.stop() triggers shutdown
// ============================================================================

TEST_F(LuaEngineTest, ApiStop_SetsShutdownRequested)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            api.stop()
            return false
        end
    )");

    RoleHostCore core;
    LuaEngine engine;
    ASSERT_TRUE(setup_engine_with_core(engine, core));

    EXPECT_FALSE(core.is_shutdown_requested());

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce(InvokeTx{&buf, sizeof(buf)}, msgs);

    EXPECT_TRUE(core.is_shutdown_requested())
        << "api.stop() should set shutdown_requested";

    engine.finalize();
}

// ============================================================================
// 16. api.set_critical_error() triggers critical shutdown
// ============================================================================

TEST_F(LuaEngineTest, ApiSetCriticalError_SetsCriticalFlag)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            api.set_critical_error("test critical error")
            return false
        end
    )");

    RoleHostCore core;
    LuaEngine engine;
    ASSERT_TRUE(setup_engine_with_core(engine, core));

    EXPECT_FALSE(core.is_critical_error());
    EXPECT_FALSE(core.is_shutdown_requested());

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce(InvokeTx{&buf, sizeof(buf)}, msgs);

    EXPECT_TRUE(core.is_critical_error())
        << "api.set_critical_error() should set critical_error_";
    EXPECT_TRUE(core.is_shutdown_requested())
        << "api.set_critical_error() should also set shutdown_requested";

    engine.finalize();
}

// ============================================================================
// 17. api.stop_reason() returns correct values
// ============================================================================

TEST_F(LuaEngineTest, ApiStopReason_DefaultIsNormal)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            local reason = api.stop_reason()
            assert(reason == "normal",
                   "expected 'normal', got '" .. tostring(reason) .. "'")
            return false
        end
    )");

    RoleHostCore core;
    LuaEngine engine;
    ASSERT_TRUE(setup_engine_with_core(engine, core));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce(InvokeTx{&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u) << "stop_reason should be 'normal'";

    engine.finalize();
}

TEST_F(LuaEngineTest, ApiStopReason_ReflectsPeerDead)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            local reason = api.stop_reason()
            assert(reason == "peer_dead",
                   "expected 'peer_dead', got '" .. tostring(reason) .. "'")
            return false
        end
    )");

    RoleHostCore core;
    core.set_stop_reason(RoleHostCore::StopReason::PeerDead); // PeerDead

    LuaEngine engine;
    ASSERT_TRUE(setup_engine_with_core(engine, core));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce(InvokeTx{&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u) << "stop_reason should be 'peer_dead'";

    engine.finalize();
}

// ============================================================================
// 18. Metrics closures read from RoleHostCore counters
// ============================================================================

TEST_F(LuaEngineTest, MetricsClosures_ReadFromRoleHostCounters)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            local ow = api.out_slots_written()
            local dr = api.out_drop_count()
            assert(ow == 42, "expected out_written=42, got " .. tostring(ow))
            assert(dr == 7,  "expected drops=7, got " .. tostring(dr))
            return false
        end
    )");

    RoleHostCore core;
    core.test_set_out_slots_written(42);
    core.test_set_out_drop_count(7);

    LuaEngine engine;
    ASSERT_TRUE(setup_engine_with_core(engine, core));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce(InvokeTx{&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "Script assertion failed — metrics values incorrect";

    engine.finalize();
}

// ============================================================================
// 19. Wrong return type detection
// ============================================================================

TEST_F(LuaEngineTest, InvokeProduce_WrongReturnType_IsError)
{
    // Returning a number instead of boolean should be an Error.
    write_script(R"(
        function on_produce(tx, msgs, api)
            return 42
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;

    auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(result, InvokeResult::Error)
        << "Returning a number should be Error, not Commit";
    EXPECT_EQ(engine.script_error_count(), 1u);

    engine.finalize();
}

TEST_F(LuaEngineTest, InvokeProduce_WrongReturnString_IsError)
{
    // Returning a string instead of boolean should be an Error.
    write_script(R"(
        function on_produce(tx, msgs, api)
            return "ok"
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;

    auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(result, InvokeResult::Error)
        << "Returning a string should be Error, not Commit";
    EXPECT_EQ(engine.script_error_count(), 1u);

    engine.finalize();
}

// ============================================================================
// 20. stop_on_script_error at engine level
// ============================================================================

TEST_F(LuaEngineTest, StopOnScriptError_SetsShutdownOnError)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            error("intentional error")
        end
    )");

    RoleHostCore core;
    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test", &core));
    ASSERT_TRUE(engine.load_script(tmp_, "init.lua", "on_produce"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame", "aligned"));

    auto test_api = make_api(core);
    test_api->set_stop_on_script_error(true);
    ASSERT_TRUE(engine.build_api(*test_api));

    EXPECT_FALSE(core.is_shutdown_requested());

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf)}, msgs);

    EXPECT_EQ(result, InvokeResult::Error);
    EXPECT_TRUE(core.is_shutdown_requested())
        << "stop_on_script_error should set shutdown_requested on error";

    engine.finalize();
}

// ============================================================================
// 21. Negative paths: load_script failures
// ============================================================================

TEST_F(LuaEngineTest, LoadScript_MissingFile_ReturnsFalse)
{
    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    EXPECT_FALSE(engine.load_script(tmp_, "nonexistent.lua", "on_produce"));
    engine.finalize();
}

TEST_F(LuaEngineTest, LoadScript_MissingRequiredCallback_ReturnsFalse)
{
    write_script(R"(
        function on_init(api) end
        -- on_produce intentionally absent
    )");

    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    EXPECT_FALSE(engine.load_script(tmp_, "init.lua", "on_produce"));
    engine.finalize();
}

TEST_F(LuaEngineTest, RegisterSlotType_BadFieldType_ReturnsFalse)
{
    write_script("function on_produce(tx, msgs, api) return true end");

    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_, "init.lua", "on_produce"));

    SchemaSpec bad_spec;
    FieldDef f;
    f.name = "x";
    f.type_str = "complex128";  // unsupported type
    f.count = 1;
    f.length = 0;
    bad_spec.fields.push_back(f);

    EXPECT_FALSE(engine.register_slot_type(bad_spec, "BadFrame", "aligned"));
    engine.finalize();
}

TEST_F(LuaEngineTest, LoadScript_SyntaxError_ReturnsFalse)
{
    write_script("function on_produce(tx, msgs, api)  -- unterminated");

    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    EXPECT_FALSE(engine.load_script(tmp_, "init.lua", "on_produce"));
    engine.finalize();
}

TEST_F(LuaEngineTest, MetricsClosures_InReceivedWorks)
{
    write_script(R"(
        function on_consume(rx, msgs, api)
            local ir = api.in_slots_received()
            assert(ir == 15, "expected in_received=15, got " .. tostring(ir))
            return true
        end
    )");

    RoleHostCore core;
    core.test_set_in_slots_received(15);

    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test", &core));
    ASSERT_TRUE(engine.load_script(tmp_, "init.lua", "on_consume"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "InSlotFrame", "aligned"));

    auto test_api = make_api(core, "cons");
    ASSERT_TRUE(engine.build_api(*test_api));

    std::vector<IncomingMessage> msgs;
    engine.invoke_consume(InvokeRx{nullptr, 0}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "Script assertion failed — in_received value incorrect";

    engine.finalize();
}

// ============================================================================
// Generic invoke() tests
// ============================================================================

TEST_F(LuaEngineTest, Invoke_ExistingFunction_ReturnsTrue)
{
    write_script(R"(
function on_produce(tx, msgs, api) return true end
function on_heartbeat() end
)");
    LuaEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    EXPECT_TRUE(engine.invoke("on_heartbeat"));
    engine.finalize();
}

TEST_F(LuaEngineTest, Invoke_NonExistentFunction_ReturnsFalse)
{
    write_script("function on_produce(tx, msgs, api) return true end");
    LuaEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    EXPECT_FALSE(engine.invoke("no_such_function"));
    engine.finalize();
}

TEST_F(LuaEngineTest, Invoke_EmptyName_ReturnsFalse)
{
    write_script("function on_produce(tx, msgs, api) return true end");
    LuaEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    EXPECT_FALSE(engine.invoke(""));
    engine.finalize();
}

TEST_F(LuaEngineTest, Invoke_ScriptError_ReturnsFalseAndIncrementsErrors)
{
    write_script(R"(
function on_produce(tx, msgs, api) return true end
function bad_func() error("intentional test error") end
)");
    RoleHostCore core;
    LuaEngine engine;
    ASSERT_TRUE(setup_engine_with_core(engine, core));

    EXPECT_FALSE(engine.invoke("bad_func"));
    EXPECT_EQ(core.script_error_count(), 1u);
    engine.finalize();
}

TEST_F(LuaEngineTest, Invoke_FromNonOwnerThread_Works)
{
    write_script(R"(
function on_produce(tx, msgs, api) return true end
function on_heartbeat() end
)");
    LuaEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    bool result = false;
    std::thread t([&] { result = engine.invoke("on_heartbeat"); });
    t.join();

    EXPECT_TRUE(result);
    engine.finalize();
}

TEST_F(LuaEngineTest, Invoke_WithArgs_ReturnsTrue)
{
    write_script(R"(
function on_produce(tx, msgs, api) return true end
function greet() end
)");
    LuaEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    nlohmann::json args = {{"name", "test"}};
    EXPECT_TRUE(engine.invoke("greet", args));
    engine.finalize();
}

TEST_F(LuaEngineTest, Invoke_NonOwnerThread_UsesIndependentState)
{
    // Verify that a non-owner thread gets its own lua_State:
    // setting a global on the non-owner thread should NOT be visible
    // on the owner's state.
    write_script(R"(
function on_produce(tx, msgs, api) return true end
function set_marker() marker_set = true end
)");
    LuaEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    // Non-owner thread sets a global variable.
    std::thread t([&] { engine.invoke("set_marker"); });
    t.join();

    // Owner thread: marker should NOT be visible (different state).
    auto result = engine.eval("return marker_set");
    // marker_set is nil in owner's state → eval returns null JSON.
    EXPECT_EQ(result.status, InvokeStatus::Ok);
    EXPECT_TRUE(result.value.is_null()) << "Non-owner global leaked to owner state";

    engine.finalize();
}

TEST_F(LuaEngineTest, Invoke_ConcurrentOwnerAndNonOwner)
{
    // Verify: owner and non-owner invoke concurrently, both complete
    // their work, shared_data reflects both threads' contributions.
    write_script(R"(
function on_produce(tx, msgs, api) return true end
function inc_owner()
    local v = api.get_shared_data("owner_count") or 0
    api.set_shared_data("owner_count", v + 1)
end
function inc_child()
    local v = api.get_shared_data("child_count") or 0
    api.set_shared_data("child_count", v + 1)
end
)");
    RoleHostCore core;
    LuaEngine engine;
    ASSERT_TRUE(setup_engine_with_core(engine, core));

    constexpr int kCalls = 20;
    std::atomic<bool> barrier{false};
    std::atomic<int> child_ok{0};

    std::thread t([&]
    {
        // Wait for barrier so both threads start together.
        while (!barrier.load(std::memory_order_acquire)) {}
        for (int i = 0; i < kCalls; ++i)
        {
            if (engine.invoke("inc_child"))
                child_ok.fetch_add(1, std::memory_order_relaxed);
        }
    });

    int owner_ok = 0;
    barrier.store(true, std::memory_order_release); // release both
    for (int i = 0; i < kCalls; ++i)
    {
        if (engine.invoke("inc_owner"))
            ++owner_ok;
    }

    t.join();

    // Both threads completed their calls.
    EXPECT_EQ(owner_ok, kCalls);
    EXPECT_EQ(child_ok.load(), kCalls);

    // shared_data has evidence from both threads.
    auto ov = core.get_shared_data("owner_count");
    auto cv = core.get_shared_data("child_count");
    ASSERT_TRUE(ov.has_value());
    ASSERT_TRUE(cv.has_value());
    EXPECT_EQ(std::get<int64_t>(*ov), kCalls);
    EXPECT_EQ(std::get<int64_t>(*cv), kCalls);

    engine.finalize();
}

TEST_F(LuaEngineTest, Eval_ReturnsScalarResult)
{
    write_script("function on_produce(tx, msgs, api) return true end");
    LuaEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    auto r1 = engine.eval("return 42");
    EXPECT_EQ(r1.status, InvokeStatus::Ok);
    EXPECT_EQ(r1.value, 42);

    auto r2 = engine.eval("return 'hello'");
    EXPECT_EQ(r2.status, InvokeStatus::Ok);
    EXPECT_EQ(r2.value, "hello");

    auto r3 = engine.eval("return true");
    EXPECT_EQ(r3.status, InvokeStatus::Ok);
    EXPECT_EQ(r3.value, true);

    engine.finalize();
}

TEST_F(LuaEngineTest, Invoke_AfterFinalize_ReturnsFalse)
{
    write_script(R"(
function on_produce(tx, msgs, api) return true end
function on_heartbeat() end
)");
    LuaEngine engine;
    ASSERT_TRUE(setup_engine(engine));
    engine.finalize();

    EXPECT_FALSE(engine.invoke("on_heartbeat"));
}

// ============================================================================
// Shared data tests (api.get_shared_data / api.set_shared_data)
// ============================================================================

TEST_F(LuaEngineTest, SharedData_SetAndGetFromScript)
{
    write_script(R"(
function on_produce(tx, msgs, api)
    api.set_shared_data("counter", 42)
    api.set_shared_data("label", "hello")
    api.set_shared_data("flag", true)
    api.set_shared_data("ratio", 3.14)
    return true
end
)");
    RoleHostCore core;
    LuaEngine engine;
    ASSERT_TRUE(setup_engine_with_core(engine, core));

    std::vector<IncomingMessage> msgs;
    engine.invoke_produce(InvokeTx{nullptr, 0}, msgs);

    // Verify values stored in core_ shared data.
    auto v1 = core.get_shared_data("counter");
    ASSERT_TRUE(v1.has_value());
    EXPECT_EQ(std::get<int64_t>(*v1), 42);

    auto v2 = core.get_shared_data("label");
    ASSERT_TRUE(v2.has_value());
    EXPECT_EQ(std::get<std::string>(*v2), "hello");

    auto v3 = core.get_shared_data("flag");
    ASSERT_TRUE(v3.has_value());
    EXPECT_TRUE(std::get<bool>(*v3));

    auto v4 = core.get_shared_data("ratio");
    ASSERT_TRUE(v4.has_value());
    EXPECT_DOUBLE_EQ(std::get<double>(*v4), 3.14);

    engine.finalize();
}

TEST_F(LuaEngineTest, SharedData_GetReturnsNilForMissingKey)
{
    write_script(R"(
function on_produce(tx, msgs, api)
    local val = api.get_shared_data("nonexistent")
    if val ~= nil then error("expected nil") end
    return true
end
)");
    RoleHostCore core;
    LuaEngine engine;
    ASSERT_TRUE(setup_engine_with_core(engine, core));

    std::vector<IncomingMessage> msgs;
    auto r = engine.invoke_produce(InvokeTx{nullptr, 0}, msgs);
    EXPECT_EQ(r, InvokeResult::Commit);
    EXPECT_EQ(core.script_error_count(), 0u);
    engine.finalize();
}

TEST_F(LuaEngineTest, SharedData_NilRemovesKey)
{
    write_script(R"(
function on_produce(tx, msgs, api)
    api.set_shared_data("temp", 99)
    api.set_shared_data("temp", nil)
    return true
end
)");
    RoleHostCore core;
    LuaEngine engine;
    ASSERT_TRUE(setup_engine_with_core(engine, core));

    std::vector<IncomingMessage> msgs;
    engine.invoke_produce(InvokeTx{nullptr, 0}, msgs);

    EXPECT_FALSE(core.get_shared_data("temp").has_value());
    engine.finalize();
}

TEST_F(LuaEngineTest, SharedData_CrossThread_Visible)
{
    write_script(R"(
function on_produce(tx, msgs, api) return true end
function set_marker() api.set_shared_data("from_thread", 123) end
)");
    RoleHostCore core;
    LuaEngine engine;
    ASSERT_TRUE(setup_engine_with_core(engine, core));

    // Non-owner thread sets shared data via its own Lua state.
    std::thread t([&] { engine.invoke("set_marker"); });
    t.join();

    // Verify from C++ (shared map in core_).
    auto val = core.get_shared_data("from_thread");
    ASSERT_TRUE(val.has_value());
    EXPECT_EQ(std::get<int64_t>(*val), 123);
    engine.finalize();
}

// ============================================================================
// Lua api.metrics() — hierarchical table structure
// ============================================================================

TEST_F(LuaEngineTest, Metrics_HierarchicalTable_Producer)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            local m = api.metrics()
            -- Top-level must have "loop" and "role" (no "queue" — no queue connected)
            assert(type(m) == "table", "metrics() must return a table")
            assert(type(m.loop) == "table", "loop group must be a table")
            assert(type(m.role) == "table", "role group must be a table")
            assert(m.queue == nil, "queue must be absent when no queue connected")
            assert(m.inbox == nil, "inbox must be absent when no inbox connected")

            -- Loop fields
            assert(type(m.loop.iteration_count) == "number",
                   "loop.iteration_count must be a number")
            assert(type(m.loop.loop_overrun_count) == "number",
                   "loop.loop_overrun_count must be a number")
            assert(type(m.loop.last_cycle_work_us) == "number",
                   "loop.last_cycle_work_us must be a number")

            -- Role fields
            assert(m.role.out_slots_written == 5,
                   "role.out_slots_written expected 5, got " .. tostring(m.role.out_slots_written))
            assert(m.role.out_drop_count == 2,
                   "role.out_drop_count expected 2, got " .. tostring(m.role.out_drop_count))
            assert(type(m.role.script_error_count) == "number",
                   "role.script_error_count must be a number")
            return false
        end
    )");

    RoleHostCore core;
    core.test_set_out_slots_written(5);
    core.test_set_out_drop_count(2);

    LuaEngine engine;
    ASSERT_TRUE(setup_engine_with_core(engine, core));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce(InvokeTx{&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "Lua assertion failed — hierarchical metrics structure incorrect";

    engine.finalize();
}

TEST_F(LuaEngineTest, Metrics_HierarchicalTable_Consumer)
{
    write_script(R"(
        function on_consume(rx, msgs, api)
            local m = api.metrics()
            assert(type(m.loop) == "table", "loop group must be a table")
            assert(type(m.role) == "table", "role group must be a table")
            assert(m.queue == nil, "queue absent without connection")

            assert(m.role.in_slots_received == 10,
                   "role.in_slots_received expected 10, got " .. tostring(m.role.in_slots_received))
            return true
        end
    )");

    RoleHostCore core;
    core.test_set_in_slots_received(10);

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
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "Lua assertion failed — consumer hierarchical metrics incorrect";

    engine.finalize();
}

// ============================================================================
// New API closures — diagnostics, queue-state, custom metrics, environment
// ============================================================================

TEST_F(LuaEngineTest, Api_LoopOverrunCount_ReadsFromCore)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            local v = api.loop_overrun_count()
            assert(v == 3, "expected 3, got " .. tostring(v))
            return false
        end
    )");

    RoleHostCore core;
    core.inc_loop_overrun();
    core.inc_loop_overrun();
    core.inc_loop_overrun();

    LuaEngine engine;
    ASSERT_TRUE(setup_engine_with_core(engine, core));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce(InvokeTx{&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u);
    engine.finalize();
}

TEST_F(LuaEngineTest, Api_LastCycleWorkUs_ReadsFromCore)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            local v = api.last_cycle_work_us()
            assert(v == 12345, "expected 12345, got " .. tostring(v))
            return false
        end
    )");

    RoleHostCore core;
    core.set_last_cycle_work_us(12345);

    LuaEngine engine;
    ASSERT_TRUE(setup_engine_with_core(engine, core));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce(InvokeTx{&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u);
    engine.finalize();
}

TEST_F(LuaEngineTest, Api_CriticalError_DefaultIsFalse)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            assert(api.critical_error() == false,
                   "critical_error should be false by default")
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

TEST_F(LuaEngineTest, Api_CustomMetrics_ReportAndReadInMetrics)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            api.report_metric("latency_ms", 42.5)
            api.report_metric("throughput", 100)

            local m = api.metrics()
            assert(m.custom ~= nil, "custom metrics group must exist")
            assert(m.custom.latency_ms == 42.5,
                   "latency_ms expected 42.5, got " .. tostring(m.custom.latency_ms))
            assert(m.custom.throughput == 100,
                   "throughput expected 100, got " .. tostring(m.custom.throughput))
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

TEST_F(LuaEngineTest, Api_CustomMetrics_ReportMetricsBatch)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            api.report_metrics({a = 1.0, b = 2.0, c = 3.0})

            local m = api.metrics()
            assert(m.custom.a == 1.0, "a expected 1.0")
            assert(m.custom.b == 2.0, "b expected 2.0")
            assert(m.custom.c == 3.0, "c expected 3.0")
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

TEST_F(LuaEngineTest, Api_CustomMetrics_ClearRemovesAll)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            api.report_metric("x", 99)
            api.clear_custom_metrics()

            local m = api.metrics()
            assert(m.custom == nil, "custom group should be nil after clear")
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
// 22. on_init / on_stop / on_inbox script error detection
// ============================================================================

TEST_F(LuaEngineTest, InvokeOnInit_ScriptError)
{
    write_script(R"(
        function on_produce(tx, msgs, api) return true end
        function on_init(api)
            error("init failed")
        end
    )");

    RoleHostCore core;
    LuaEngine engine;
    ASSERT_TRUE(setup_engine_with_core(engine, core, "on_produce"));

    EXPECT_EQ(engine.script_error_count(), 0u);
    engine.invoke_on_init();
    EXPECT_GE(engine.script_error_count(), 1u)
        << "on_init error should increment script_error_count";

    engine.finalize();
}

TEST_F(LuaEngineTest, InvokeOnStop_ScriptError)
{
    write_script(R"(
        function on_produce(tx, msgs, api) return true end
        function on_stop(api)
            error("stop failed")
        end
    )");

    RoleHostCore core;
    LuaEngine engine;
    ASSERT_TRUE(setup_engine_with_core(engine, core, "on_produce"));

    EXPECT_EQ(engine.script_error_count(), 0u);
    engine.invoke_on_stop();
    EXPECT_GE(engine.script_error_count(), 1u)
        << "on_stop error should increment script_error_count";

    engine.finalize();
}

TEST_F(LuaEngineTest, InvokeOnInbox_ScriptError)
{
    write_script(R"(
        function on_produce(tx, msgs, api) return false end
        function on_inbox(msg, api)
            error("inbox failed")
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_, "init.lua", "on_produce"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame", "aligned"));
    ASSERT_TRUE(engine.register_slot_type(spec, "InboxFrame", "aligned"));

    auto test_api = make_api(default_core_);
    ASSERT_TRUE(engine.build_api(*test_api));

    EXPECT_EQ(engine.script_error_count(), 0u);

    float inbox_data = 1.0f;
    engine.invoke_on_inbox({&inbox_data, sizeof(inbox_data), "SENDER-00000001", 1});
    EXPECT_GE(engine.script_error_count(), 1u)
        << "on_inbox error should increment script_error_count";

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
// 24. Identity fields match context
// ============================================================================

TEST_F(LuaEngineTest, Api_IdentityFields_MatchContext)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            assert(api.uid() == "TEST-ENGINE-00000001",
                   "uid mismatch: " .. tostring(api.uid()))
            assert(api.name() == "TestEngine",
                   "name mismatch: " .. tostring(api.name()))
            assert(api.channel() == "test.channel",
                   "channel mismatch: " .. tostring(api.channel()))
            return false
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce(InvokeTx{&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "Script assertion failed — identity fields do not match context";
    engine.finalize();
}

// ============================================================================
// 26. Metrics — all loop fields present with non-zero verification
// ============================================================================

TEST_F(LuaEngineTest, Metrics_AllLoopFields_Present)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            local m = api.metrics()
            assert(type(m.loop) == "table", "loop group must be a table")

            assert(type(m.loop.iteration_count) == "number",
                   "iteration_count must be a number")
            assert(m.loop.iteration_count == 10,
                   "iteration_count expected 10, got " .. tostring(m.loop.iteration_count))

            assert(type(m.loop.loop_overrun_count) == "number",
                   "loop_overrun_count must be a number")
            assert(m.loop.loop_overrun_count == 3,
                   "loop_overrun_count expected 3, got " .. tostring(m.loop.loop_overrun_count))

            assert(type(m.loop.last_cycle_work_us) == "number",
                   "last_cycle_work_us must be a number")
            assert(m.loop.last_cycle_work_us == 500,
                   "last_cycle_work_us expected 500, got " .. tostring(m.loop.last_cycle_work_us))

            assert(type(m.loop.configured_period_us) == "number",
                   "configured_period_us must be a number")
            assert(m.loop.configured_period_us == 1000,
                   "configured_period_us expected 1000, got " .. tostring(m.loop.configured_period_us))

            return false
        end
    )");

    RoleHostCore core;
    for (int i = 0; i < 10; ++i) core.inc_iteration_count();
    for (int i = 0; i < 3; ++i) core.inc_loop_overrun();
    core.set_last_cycle_work_us(500);
    core.set_configured_period(1000);

    LuaEngine engine;
    ASSERT_TRUE(setup_engine_with_core(engine, core));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce(InvokeTx{&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "Lua assertion failed — loop metrics fields incorrect";
    engine.finalize();
}

// ============================================================================
// 27. Custom metrics — overwrite same key
// ============================================================================

TEST_F(LuaEngineTest, Api_CustomMetrics_OverwriteSameKey)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            api.report_metric("x", 1)
            api.report_metric("x", 2)
            local m = api.metrics()
            assert(m.custom ~= nil, "custom metrics must exist")
            assert(m.custom.x == 2,
                   "x should be overwritten to 2, got " .. tostring(m.custom.x))
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

// ============================================================================
// 28. Custom metrics — zero value is preserved (not nil)
// ============================================================================

TEST_F(LuaEngineTest, Api_CustomMetrics_ZeroValue)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            api.report_metric("x", 0.0)
            local m = api.metrics()
            assert(m.custom ~= nil, "custom metrics must exist")
            assert(m.custom.x ~= nil, "x must not be nil")
            assert(m.custom.x == 0, "x should be 0, got " .. tostring(m.custom.x))
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

// ============================================================================
// 29. invoke_produce with empty messages list
// ============================================================================

TEST_F(LuaEngineTest, InvokeProduce_EmptyMessagesList)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            assert(#msgs == 0, "expected 0 msgs, got " .. tostring(#msgs))
            return false
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs; // empty

    auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(result, InvokeResult::Discard);
    EXPECT_EQ(engine.script_error_count(), 0u);
    engine.finalize();
}

// ============================================================================
// 30. open_inbox without broker returns nil
// ============================================================================

TEST_F(LuaEngineTest, Api_OpenInbox_WithoutBroker)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            local handle = api.open_inbox("some-uid")
            assert(handle == nil,
                   "open_inbox without broker should return nil")
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
// 32. Eval syntax error returns ScriptError
// ============================================================================

TEST_F(LuaEngineTest, Eval_SyntaxError_ReturnsScriptError)
{
    write_script(R"(function on_produce(tx, msgs, api) return false end)");
    LuaEngine engine;
    ASSERT_TRUE(setup_engine(engine));
    auto result = engine.eval("invalid syntax {{{");
    EXPECT_EQ(result.status, InvokeStatus::ScriptError);
    engine.finalize();
}

// ============================================================================
// 33. api.stop_reason() reflects CriticalError
// ============================================================================

TEST_F(LuaEngineTest, ApiStopReason_AfterCriticalError)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            local reason = api.stop_reason()
            assert(reason == "critical_error",
                   "expected 'critical_error', got '" .. tostring(reason) .. "'")
            return false
        end
    )");

    RoleHostCore core;
    core.set_stop_reason(RoleHostCore::StopReason::CriticalError);

    LuaEngine engine;
    ASSERT_TRUE(setup_engine_with_core(engine, core));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce(InvokeTx{&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u) << "stop_reason should be 'critical_error'";

    engine.finalize();
}

// ============================================================================
// 34. api.report_metrics() with non-table argument is an error
// ============================================================================

TEST_F(LuaEngineTest, Api_ReportMetrics_NonTableArg_IsError)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            api.report_metrics(42)  -- wrong type, should error
            return false
        end
    )");
    LuaEngine engine;
    ASSERT_TRUE(setup_engine(engine));
    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce(InvokeTx{&buf, sizeof(buf)}, msgs);
    EXPECT_GE(engine.script_error_count(), 1u);
    engine.finalize();
}

// ============================================================================
// 35. Full lifecycle verifies callback execution via shared_data
// ============================================================================

TEST_F(LuaEngineTest, FullLifecycle_VerifiesCallbackExecution)
{
    write_script(R"(
        function on_produce(tx, msgs, api) return false end
        function on_init(api)
            api.set_shared_data("init_ran", true)
        end
        function on_stop(api)
            api.set_shared_data("stop_ran", true)
        end
    )");
    RoleHostCore core;
    LuaEngine engine;
    ASSERT_TRUE(setup_engine_with_core(engine, core));

    EXPECT_FALSE(core.get_shared_data("init_ran").has_value());
    engine.invoke_on_init();
    auto init_val = core.get_shared_data("init_ran");
    ASSERT_TRUE(init_val.has_value());
    EXPECT_TRUE(std::get<bool>(*init_val));

    EXPECT_FALSE(core.get_shared_data("stop_ran").has_value());
    engine.invoke_on_stop();
    auto stop_val = core.get_shared_data("stop_ran");
    ASSERT_TRUE(stop_val.has_value());
    EXPECT_TRUE(std::get<bool>(*stop_val));

    engine.finalize();
}

// ============================================================================
// Group F: Spinlocks — L2 tests (no SHM, safe defaults)
// ============================================================================

TEST_F(LuaEngineTest, Api_SpinlockCount_WithoutSHM)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            assert(api.spinlock_count() == 0,
                   "spinlock_count should be 0 without SHM")
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

TEST_F(LuaEngineTest, Api_Spinlock_WithoutSHM_IsError)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            local ok, err = pcall(api.spinlock, 0)
            assert(not ok, "spinlock(0) should error without producer/consumer")
            return false
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce(InvokeTx{&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "pcall should catch the error, not propagate";
    engine.finalize();
}

// ============================================================================
// Group G: Flexzone — L2 tests (no SHM, safe defaults)
// ============================================================================

TEST_F(LuaEngineTest, Api_Flexzone_WithoutSHM_ReturnsNil)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            local fz_obj = api.flexzone()
            assert(fz_obj == nil, "flexzone should be nil without SHM")
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

// ============================================================================
// Schema logical size — matching Python engine test coverage
// ============================================================================

TEST_F(LuaEngineTest, SlotLogicalSize_Aligned_PaddingSensitive)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            local sz = api.slot_logical_size()
            assert(sz == 16, "expected 16, got " .. tostring(sz))
            return false
        end
    )");

    LuaEngine engine;
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
    params.script_dir        = tmp_;
    params.entry_point       = "init.lua";
    params.required_callback = "on_produce";
    params.out_slot_spec     = slot_spec;
    params.out_packing       = "aligned";

    ASSERT_NO_THROW(pylabhub::scripting::engine_lifecycle_startup(nullptr, &params));

    size_t logical = pylabhub::hub::compute_schema_size(slot_spec, "aligned");
    EXPECT_EQ(logical, 16u);
    EXPECT_EQ(engine.type_sizeof("OutSlotFrame"), logical);

    float buf[4] = {};
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce(InvokeTx{buf, sizeof(buf)}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u);

    pylabhub::scripting::engine_lifecycle_shutdown(nullptr, &params);
}

TEST_F(LuaEngineTest, SlotLogicalSize_Packed_NoPadding)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            local sz = api.slot_logical_size()
            assert(sz == 13, "expected 13, got " .. tostring(sz))
            return false
        end
    )");

    LuaEngine engine;
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
    params.script_dir        = tmp_;
    params.entry_point       = "init.lua";
    params.required_callback = "on_produce";
    params.out_slot_spec     = slot_spec;
    params.out_packing       = "packed";

    ASSERT_NO_THROW(pylabhub::scripting::engine_lifecycle_startup(nullptr, &params));

    EXPECT_EQ(pylabhub::hub::compute_schema_size(slot_spec, "packed"), 13u);
    EXPECT_EQ(engine.type_sizeof("OutSlotFrame"), 13u);

    uint8_t buf[16] = {};
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce(InvokeTx{buf, sizeof(buf)}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u);

    pylabhub::scripting::engine_lifecycle_shutdown(nullptr, &params);
}

TEST_F(LuaEngineTest, SlotLogicalSize_ComplexMixed_Aligned)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            local sz = api.slot_logical_size()
            assert(sz == 56, "expected 56, got " .. tostring(sz))
            return false
        end
    )");

    LuaEngine engine;
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
    params.script_dir        = tmp_;
    params.entry_point       = "init.lua";
    params.required_callback = "on_produce";
    params.out_slot_spec     = slot_spec;
    params.out_packing       = "aligned";

    ASSERT_NO_THROW(pylabhub::scripting::engine_lifecycle_startup(nullptr, &params));

    EXPECT_EQ(pylabhub::hub::compute_schema_size(slot_spec, "aligned"), 56u);
    EXPECT_EQ(engine.type_sizeof("OutSlotFrame"), 56u);

    uint8_t buf[64] = {};
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce(InvokeTx{buf, sizeof(buf)}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u);

    pylabhub::scripting::engine_lifecycle_shutdown(nullptr, &params);
}

TEST_F(LuaEngineTest, FlexzoneLogicalSize_ArrayFields)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            local slot_sz = api.slot_logical_size()
            local fz_sz = api.flexzone_logical_size()
            assert(slot_sz == 16, "slot: expected 16, got " .. tostring(slot_sz))
            assert(fz_sz == 24, "fz: expected 24, got " .. tostring(fz_sz))
            return false
        end
    )");

    LuaEngine engine;
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
    params.script_dir        = tmp_;
    params.entry_point       = "init.lua";
    params.required_callback = "on_produce";
    params.out_slot_spec     = slot_spec;
    params.out_fz_spec       = fz_spec;
    params.out_packing       = "aligned";

    ASSERT_NO_THROW(pylabhub::scripting::engine_lifecycle_startup(nullptr, &params));

    EXPECT_EQ(pylabhub::hub::compute_schema_size(slot_spec, "aligned"), 16u);
    EXPECT_EQ(pylabhub::hub::compute_schema_size(fz_spec, "aligned"), 24u);
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
// Band pub/sub API — L2 (no broker, methods return nil/false gracefully)
// ============================================================================

TEST_F(LuaEngineTest, Api_Channel_JoinReturnsNilWithoutBroker)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            local result = api.band_join("#test_ch")
            assert(result == nil, "Expected nil from band_join without broker")
            return true
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0;
    std::vector<IncomingMessage> msgs;
    auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(result, InvokeResult::Commit);
    EXPECT_EQ(engine.script_error_count(), 0u);
    engine.finalize();
}

TEST_F(LuaEngineTest, Api_Channel_LeaveReturnsFalseWithoutBroker)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            local result = api.band_leave("#test_ch")
            assert(result == false, "Expected false from band_leave without broker")
            return true
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0;
    std::vector<IncomingMessage> msgs;
    auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(result, InvokeResult::Commit);
    EXPECT_EQ(engine.script_error_count(), 0u);
    engine.finalize();
}

TEST_F(LuaEngineTest, Api_Channel_SendMsgNoErrorWithoutBroker)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            api.band_broadcast("#test_ch", {hello = "world", value = 42})
            return true
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0;
    std::vector<IncomingMessage> msgs;
    auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(result, InvokeResult::Commit);
    EXPECT_EQ(engine.script_error_count(), 0u);
    engine.finalize();
}

TEST_F(LuaEngineTest, Api_Channel_MembersReturnsNilWithoutBroker)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            local result = api.band_members("#test_ch")
            assert(result == nil, "Expected nil from band_members without broker")
            return true
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0;
    std::vector<IncomingMessage> msgs;
    auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf)}, msgs);
    EXPECT_EQ(result, InvokeResult::Commit);
    EXPECT_EQ(engine.script_error_count(), 0u);
    engine.finalize();
}

} // anonymous namespace
