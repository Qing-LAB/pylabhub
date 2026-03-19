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
 *   4. invoke_consume: read-only slot, no return value, error detection
 *   5. invoke_process: dual slots, nil combinations
 *   6. Messages: table format for producer/consumer
 *   7. API closures: log, stop, version_info
 *   8. Error handling: script_error_count, stop_on_script_error
 *   9. create_thread_state: independent engine
 */
#include <gtest/gtest.h>

#include "lua_engine.hpp"
#include "role_host_core.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using pylabhub::scripting::LuaEngine;
using pylabhub::scripting::ScriptEngine;
using pylabhub::scripting::InvokeResult;
using pylabhub::scripting::RoleContext;
using pylabhub::scripting::RoleHostCore;
using pylabhub::scripting::IncomingMessage;
using pylabhub::scripting::SchemaSpec;
using pylabhub::scripting::FieldDef;

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
    SchemaSpec simple_schema()
    {
        SchemaSpec spec;
        FieldDef f;
        f.name     = "value";
        f.type_str = "float32";
        f.count    = 1;
        f.length   = 0;
        spec.fields.push_back(f);
        return spec;
    }

    /// Build a minimal RoleContext for producer.
    RoleContext producer_context()
    {
        RoleContext ctx{};
        ctx.role_tag  = "test";
        ctx.uid       = "TEST-ENGINE-00000001";
        ctx.name      = "TestEngine";
        ctx.channel   = "test.channel";
        ctx.log_level = "error";
        ctx.stop_on_script_error = false;
        return ctx;
    }

    /// Initialize engine, load script, register type, build API.
    /// Returns true if all steps succeed.
    bool setup_engine(LuaEngine &engine, const std::string &required_cb = "on_produce")
    {
        if (!engine.initialize("test"))
            return false;
        if (!engine.load_script(tmp_, "init.lua", required_cb.c_str()))
            return false;

        auto spec = simple_schema();
        if (!engine.register_slot_type(spec, "SlotFrame", "aligned"))
            return false;

        auto ctx = producer_context();
        engine.build_api(ctx);
        return true;
    }

    /// Setup engine with a RoleHostCore wired into the context.
    bool setup_engine_with_core(LuaEngine &engine, RoleHostCore &core,
                                 const std::string &required_cb = "on_produce")
    {
        if (!engine.initialize("test"))
            return false;
        if (!engine.load_script(tmp_, "init.lua", required_cb.c_str()))
            return false;

        auto spec = simple_schema();
        if (!engine.register_slot_type(spec, "SlotFrame", "aligned"))
            return false;

        auto ctx = producer_context();
        ctx.core = &core;
        engine.build_api(ctx);
        return true;
    }

    fs::path tmp_;
};

// ============================================================================
// 1. Lifecycle
// ============================================================================

TEST_F(LuaEngineTest, FullLifecycle)
{
    write_script(R"(
        function on_produce(out_slot, fz, msgs, api)
            return true
        end
        function on_init(api) end
        function on_stop(api) end
    )");

    LuaEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    engine.invoke_on_init();
    engine.invoke_on_stop();
    engine.finalize();
}

TEST_F(LuaEngineTest, InitializeFailsGracefully)
{
    // Double initialize should not crash (engine manages state).
    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test"));
    engine.finalize();
}

// ============================================================================
// 2. Type Registration
// ============================================================================

TEST_F(LuaEngineTest, RegisterSlotType_SizeofCorrect)
{
    write_script("function on_produce(out_slot, fz, msgs, api) return true end");

    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test"));
    ASSERT_TRUE(engine.load_script(tmp_, "init.lua", "on_produce"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "SlotFrame", "aligned"));

    // float32 = 4 bytes
    EXPECT_EQ(engine.type_sizeof("SlotFrame"), 4u);

    engine.finalize();
}

TEST_F(LuaEngineTest, RegisterSlotType_MultiField)
{
    write_script("function on_produce(out_slot, fz, msgs, api) return true end");

    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test"));
    ASSERT_TRUE(engine.load_script(tmp_, "init.lua", "on_produce"));

    SchemaSpec spec;
    FieldDef f1; f1.name = "x"; f1.type_str = "float32"; f1.count = 1; f1.length = 0;
    FieldDef f2; f2.name = "y"; f2.type_str = "float32"; f2.count = 1; f2.length = 0;
    FieldDef f3; f3.name = "z"; f3.type_str = "float32"; f3.count = 1; f3.length = 0;
    spec.fields = {f1, f2, f3};

    ASSERT_TRUE(engine.register_slot_type(spec, "SlotFrame", "aligned"));
    EXPECT_EQ(engine.type_sizeof("SlotFrame"), 12u); // 3 * 4 bytes

    engine.finalize();
}

// ============================================================================
// 3. invoke_produce
// ============================================================================

TEST_F(LuaEngineTest, InvokeProduce_CommitOnTrue)
{
    write_script(R"(
        function on_produce(out_slot, fz, msgs, api)
            if out_slot then
                out_slot.value = 42.0
            end
            return true
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;

    auto result = engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);
    EXPECT_EQ(result, InvokeResult::Commit);
    EXPECT_FLOAT_EQ(buf, 42.0f);

    engine.finalize();
}

TEST_F(LuaEngineTest, InvokeProduce_DiscardOnFalse)
{
    write_script(R"(
        function on_produce(out_slot, fz, msgs, api)
            return false
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;

    auto result = engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);
    EXPECT_EQ(result, InvokeResult::Discard);

    engine.finalize();
}

TEST_F(LuaEngineTest, InvokeProduce_NilReturn_IsError)
{
    // Missing return value is an error — explicit return true/false required.
    write_script(R"(
        function on_produce(out_slot, fz, msgs, api)
            -- no explicit return → nil → error (must be explicit)
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;

    auto result = engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);
    EXPECT_EQ(result, InvokeResult::Error)
        << "Missing return should be Error, not Commit";
    EXPECT_EQ(engine.script_error_count(), 1u);

    engine.finalize();
}

TEST_F(LuaEngineTest, InvokeProduce_NilSlot)
{
    write_script(R"(
        function on_produce(out_slot, fz, msgs, api)
            assert(out_slot == nil, "expected nil slot")
            return false
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    std::vector<IncomingMessage> msgs;

    auto result = engine.invoke_produce(nullptr, 0, nullptr, 0, nullptr, msgs);
    EXPECT_EQ(result, InvokeResult::Discard);

    engine.finalize();
}

TEST_F(LuaEngineTest, InvokeProduce_ScriptError)
{
    write_script(R"(
        function on_produce(out_slot, fz, msgs, api)
            error("intentional error")
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;

    EXPECT_EQ(engine.script_error_count(), 0u);
    auto result = engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);
    EXPECT_EQ(result, InvokeResult::Error);
    EXPECT_EQ(engine.script_error_count(), 1u);

    engine.finalize();
}

// ============================================================================
// 4. invoke_consume
// ============================================================================

TEST_F(LuaEngineTest, InvokeConsume_ReceivesReadOnlySlot)
{
    // Script asserts the value — if wrong type or nil, assert fails → pcall error.
    write_script(R"(
        function on_consume(in_slot, fz, msgs, api)
            assert(in_slot ~= nil, "expected non-nil slot")
            assert(math.abs(in_slot.value - 99.5) < 0.01,
                   "expected value ~99.5, got " .. tostring(in_slot.value))
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test"));
    ASSERT_TRUE(engine.load_script(tmp_, "init.lua", "on_consume"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "SlotFrame", "aligned"));

    auto ctx = producer_context();
    engine.build_api(ctx);

    float data = 99.5f;
    std::vector<IncomingMessage> msgs;

    engine.invoke_consume(&data, sizeof(data), nullptr, 0, nullptr, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u) << "Script assertion failed — slot value incorrect";

    engine.finalize();
}

TEST_F(LuaEngineTest, InvokeConsume_NilSlot)
{
    write_script(R"(
        function on_consume(in_slot, fz, msgs, api)
            assert(in_slot == nil, "expected nil")
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test"));
    ASSERT_TRUE(engine.load_script(tmp_, "init.lua", "on_consume"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "SlotFrame", "aligned"));

    auto ctx = producer_context();
    engine.build_api(ctx);

    std::vector<IncomingMessage> msgs;
    engine.invoke_consume(nullptr, 0, nullptr, 0, nullptr, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u);

    engine.finalize();
}

TEST_F(LuaEngineTest, InvokeConsume_ScriptErrorDetected)
{
    write_script(R"(
        function on_consume(in_slot, fz, msgs, api)
            error("consume error")
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test"));
    ASSERT_TRUE(engine.load_script(tmp_, "init.lua", "on_consume"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "SlotFrame", "aligned"));

    auto ctx = producer_context();
    engine.build_api(ctx);

    float data = 1.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_consume(&data, sizeof(data), nullptr, 0, nullptr, msgs);
    EXPECT_EQ(engine.script_error_count(), 1u);

    engine.finalize();
}

// ============================================================================
// 5. invoke_process
// ============================================================================

TEST_F(LuaEngineTest, InvokeProcess_DualSlots)
{
    write_script(R"(
        function on_process(in_slot, out_slot, fz, msgs, api)
            if in_slot and out_slot then
                out_slot.value = in_slot.value * 2.0
                return true
            end
            return false
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test"));
    ASSERT_TRUE(engine.load_script(tmp_, "init.lua", "on_process"));

    auto spec = simple_schema();
    // Processor: separate in/out types.
    ASSERT_TRUE(engine.register_slot_type(spec, "InSlotFrame", "aligned"));
    ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame", "aligned"));

    auto ctx = producer_context();
    engine.build_api(ctx);

    float in_data  = 21.0f;
    float out_data = 0.0f;
    std::vector<IncomingMessage> msgs;

    auto result = engine.invoke_process(&in_data, sizeof(in_data),
                                         &out_data, sizeof(out_data),
                                         nullptr, 0, nullptr, msgs);
    EXPECT_EQ(result, InvokeResult::Commit);
    EXPECT_FLOAT_EQ(out_data, 42.0f);

    engine.finalize();
}

TEST_F(LuaEngineTest, InvokeProcess_NilInput)
{
    write_script(R"(
        function on_process(in_slot, out_slot, fz, msgs, api)
            assert(in_slot == nil, "expected nil input")
            assert(out_slot == nil, "expected nil output")
            return false
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test"));
    ASSERT_TRUE(engine.load_script(tmp_, "init.lua", "on_process"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "InSlotFrame", "aligned"));
    ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame", "aligned"));

    auto ctx = producer_context();
    engine.build_api(ctx);

    std::vector<IncomingMessage> msgs;
    auto result = engine.invoke_process(nullptr, 0, nullptr, 0,
                                         nullptr, 0, nullptr, msgs);
    EXPECT_EQ(result, InvokeResult::Discard);
    EXPECT_EQ(engine.script_error_count(), 0u);

    engine.finalize();
}

TEST_F(LuaEngineTest, InvokeProcess_InputOnlyNoOutput)
{
    write_script(R"(
        function on_process(in_slot, out_slot, fz, msgs, api)
            assert(in_slot ~= nil, "expected input data")
            assert(out_slot == nil, "expected nil output")
            return false
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test"));
    ASSERT_TRUE(engine.load_script(tmp_, "init.lua", "on_process"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "InSlotFrame", "aligned"));
    ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame", "aligned"));

    auto ctx = producer_context();
    engine.build_api(ctx);

    float in_data = 10.0f;
    std::vector<IncomingMessage> msgs;
    auto result = engine.invoke_process(&in_data, sizeof(in_data),
                                         nullptr, 0,
                                         nullptr, 0, nullptr, msgs);
    EXPECT_EQ(result, InvokeResult::Discard);

    engine.finalize();
}

// ============================================================================
// 6. Messages
// ============================================================================

TEST_F(LuaEngineTest, InvokeProduce_ReceivesMessages)
{
    // Script counts event messages and asserts the expected count.
    // If messages are not received, the assertion fails → pcall error.
    write_script(R"(
        function on_produce(out_slot, fz, msgs, api)
            local event_count = 0
            for _, m in ipairs(msgs) do
                if m.event then
                    event_count = event_count + 1
                end
            end
            assert(event_count == 2,
                   "expected 2 events, got " .. tostring(event_count))
            -- Verify specific event names.
            assert(msgs[1].event == "consumer_joined",
                   "expected consumer_joined, got " .. tostring(msgs[1].event))
            assert(msgs[2].event == "channel_closing",
                   "expected channel_closing, got " .. tostring(msgs[2].event))
            return false
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    std::vector<IncomingMessage> msgs;
    IncomingMessage m1;
    m1.event = "consumer_joined";
    m1.details["identity"] = "abc123";
    msgs.push_back(std::move(m1));

    IncomingMessage m2;
    m2.event = "channel_closing";
    msgs.push_back(std::move(m2));

    float buf = 0.0f;
    auto result = engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);
    EXPECT_EQ(result, InvokeResult::Discard);
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "Script assertion failed — messages not received correctly";

    engine.finalize();
}

// ============================================================================
// 7. API closures
// ============================================================================

TEST_F(LuaEngineTest, ApiVersionInfo_ReturnsNonEmptyString)
{
    // Script asserts that version_info returns a non-empty string containing '{'.
    write_script(R"(
        function on_produce(out_slot, fz, msgs, api)
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
    auto result = engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);
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
        function on_produce(out_slot, fz, msgs, api)
            error("oops")
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;

    for (int i = 0; i < 5; ++i)
    {
        engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);
    }
    EXPECT_EQ(engine.script_error_count(), 5u);

    engine.finalize();
}

// ============================================================================
// 9. create_thread_state
// ============================================================================

TEST_F(LuaEngineTest, CreateThreadState_IndependentEngine)
{
    write_script(R"(
        function on_produce(out_slot, fz, msgs, api)
            return true
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    EXPECT_TRUE(engine.supports_multi_state());

    auto child = engine.create_thread_state();
    ASSERT_NE(child, nullptr);
    EXPECT_TRUE(child->has_callback("on_produce"));

    // Child has independent error counter.
    EXPECT_EQ(child->script_error_count(), 0u);

    child->finalize();
    engine.finalize();
}

// ============================================================================
// 10. has_callback
// ============================================================================

TEST_F(LuaEngineTest, HasCallback_DetectsPresenceAbsence)
{
    write_script(R"(
        function on_produce(out_slot, fz, msgs, api) return true end
        function on_init(api) end
        -- on_stop intentionally absent
    )");

    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test"));
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

TEST_F(LuaEngineTest, InvokeProduce_WithFlexzone)
{
    // Script writes to both slot and flexzone.
    write_script(R"(
        function on_produce(out_slot, fz, msgs, api)
            assert(out_slot ~= nil, "expected slot")
            assert(fz ~= nil, "expected flexzone")
            out_slot.value = 10.0
            fz.value = 20.0
            return true
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test"));
    ASSERT_TRUE(engine.load_script(tmp_, "init.lua", "on_produce"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "SlotFrame", "aligned"));
    ASSERT_TRUE(engine.register_slot_type(spec, "FlexFrame", "aligned"));

    auto ctx = producer_context();
    engine.build_api(ctx);

    float slot_buf = 0.0f;
    float fz_buf   = 0.0f;
    std::vector<IncomingMessage> msgs;

    auto result = engine.invoke_produce(&slot_buf, sizeof(slot_buf),
                                         &fz_buf, sizeof(fz_buf), "FlexFrame", msgs);
    EXPECT_EQ(result, InvokeResult::Commit);
    EXPECT_FLOAT_EQ(slot_buf, 10.0f);
    EXPECT_FLOAT_EQ(fz_buf, 20.0f);

    engine.finalize();
}

// ============================================================================
// 12. invoke_on_inbox
// ============================================================================

TEST_F(LuaEngineTest, InvokeOnInbox_TypedData)
{
    // Script asserts the inbox data value and sender string.
    write_script(R"(
        function on_produce(out_slot, fz, msgs, api) return false end
        function on_inbox(slot, sender, api)
            assert(slot ~= nil, "expected inbox data")
            assert(math.abs(slot.value - 77.0) < 0.01,
                   "expected ~77.0, got " .. tostring(slot.value))
            assert(sender == "PROD-SENDER-00000001",
                   "expected sender UID, got " .. tostring(sender))
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test"));
    ASSERT_TRUE(engine.load_script(tmp_, "init.lua", "on_produce"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "SlotFrame", "aligned"));
    ASSERT_TRUE(engine.register_slot_type(spec, "InboxFrame", "aligned"));

    auto ctx = producer_context();
    engine.build_api(ctx);

    float inbox_data = 77.0f;
    engine.invoke_on_inbox(&inbox_data, sizeof(inbox_data),
                            "InboxFrame", "PROD-SENDER-00000001");
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "Script assertion failed — inbox data or sender incorrect";

    engine.finalize();
}

TEST_F(LuaEngineTest, InvokeOnInbox_RawBytes)
{
    // Script receives raw bytes (no type name).
    write_script(R"(
        function on_produce(out_slot, fz, msgs, api) return false end
        function on_inbox(data, sender, api)
            assert(type(data) == "string", "expected raw bytes as string")
            assert(#data == 4, "expected 4 bytes, got " .. #data)
            assert(sender == "CONS-SENDER-00000001")
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test"));
    ASSERT_TRUE(engine.load_script(tmp_, "init.lua", "on_produce"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "SlotFrame", "aligned"));

    auto ctx = producer_context();
    engine.build_api(ctx);

    float raw = 1.0f;
    engine.invoke_on_inbox(&raw, sizeof(raw), nullptr, "CONS-SENDER-00000001");
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "Script assertion failed — raw inbox data incorrect";

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
        function on_produce(out_slot, fz, msgs, api)
            call_count = call_count + 1
            if out_slot then
                out_slot.value = call_count
            end
            return true
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    std::vector<IncomingMessage> msgs;

    // Call 3 times — counter should increment.
    float buf1 = 0.0f;
    engine.invoke_produce(&buf1, sizeof(buf1), nullptr, 0, nullptr, msgs);
    EXPECT_FLOAT_EQ(buf1, 1.0f);

    float buf2 = 0.0f;
    engine.invoke_produce(&buf2, sizeof(buf2), nullptr, 0, nullptr, msgs);
    EXPECT_FLOAT_EQ(buf2, 2.0f);

    float buf3 = 0.0f;
    engine.invoke_produce(&buf3, sizeof(buf3), nullptr, 0, nullptr, msgs);
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
        function on_consume(in_slot, fz, msgs, api)
            assert(#msgs == 2, "expected 2 messages, got " .. #msgs)
            -- First msg: data message → bare bytes string
            assert(type(msgs[1]) == "string", "data msg should be string, got " .. type(msgs[1]))
            -- Second msg: event message → table with .event
            assert(type(msgs[2]) == "table", "event msg should be table")
            assert(msgs[2].event == "channel_closing")
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test"));
    ASSERT_TRUE(engine.load_script(tmp_, "init.lua", "on_consume"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "SlotFrame", "aligned"));

    auto ctx = producer_context();
    engine.build_api(ctx);

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

    engine.invoke_consume(nullptr, 0, nullptr, 0, nullptr, msgs);
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
        function on_produce(out_slot, fz, msgs, api)
            api.stop()
            return false
        end
    )");

    RoleHostCore core;
    LuaEngine engine;
    ASSERT_TRUE(setup_engine_with_core(engine, core));

    EXPECT_FALSE(core.shutdown_requested.load());

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);

    EXPECT_TRUE(core.shutdown_requested.load())
        << "api.stop() should set shutdown_requested";

    engine.finalize();
}

// ============================================================================
// 16. api.set_critical_error() triggers critical shutdown
// ============================================================================

TEST_F(LuaEngineTest, ApiSetCriticalError_SetsCriticalFlag)
{
    write_script(R"(
        function on_produce(out_slot, fz, msgs, api)
            api.set_critical_error("test critical error")
            return false
        end
    )");

    RoleHostCore core;
    LuaEngine engine;
    ASSERT_TRUE(setup_engine_with_core(engine, core));

    EXPECT_FALSE(core.critical_error_.load());
    EXPECT_FALSE(core.shutdown_requested.load());

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);

    EXPECT_TRUE(core.critical_error_.load())
        << "api.set_critical_error() should set critical_error_";
    EXPECT_TRUE(core.shutdown_requested.load())
        << "api.set_critical_error() should also set shutdown_requested";

    engine.finalize();
}

// ============================================================================
// 17. api.stop_reason() returns correct values
// ============================================================================

TEST_F(LuaEngineTest, ApiStopReason_DefaultIsNormal)
{
    write_script(R"(
        function on_produce(out_slot, fz, msgs, api)
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
    engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u) << "stop_reason should be 'normal'";

    engine.finalize();
}

TEST_F(LuaEngineTest, ApiStopReason_ReflectsPeerDead)
{
    write_script(R"(
        function on_produce(out_slot, fz, msgs, api)
            local reason = api.stop_reason()
            assert(reason == "peer_dead",
                   "expected 'peer_dead', got '" .. tostring(reason) .. "'")
            return false
        end
    )");

    RoleHostCore core;
    core.stop_reason_.store(1, std::memory_order_relaxed); // PeerDead

    LuaEngine engine;
    ASSERT_TRUE(setup_engine_with_core(engine, core));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u) << "stop_reason should be 'peer_dead'";

    engine.finalize();
}

// ============================================================================
// 18. Metrics closures read from RoleContext counters
// ============================================================================

TEST_F(LuaEngineTest, MetricsClosures_ReadFromRoleHostCounters)
{
    write_script(R"(
        function on_produce(out_slot, fz, msgs, api)
            local ow = api.out_written()
            local dr = api.drops()
            assert(ow == 42, "expected out_written=42, got " .. tostring(ow))
            assert(dr == 7,  "expected drops=7, got " .. tostring(dr))
            return false
        end
    )");

    RoleHostCore core;
    std::atomic<uint64_t> out_written{42};
    std::atomic<uint64_t> drops{7};

    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test"));
    ASSERT_TRUE(engine.load_script(tmp_, "init.lua", "on_produce"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "SlotFrame", "aligned"));

    auto ctx = producer_context();
    ctx.core        = &core;
    ctx.out_written = &out_written;
    ctx.drops       = &drops;
    engine.build_api(ctx);

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);
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
        function on_produce(out_slot, fz, msgs, api)
            return 42
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;

    auto result = engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);
    EXPECT_EQ(result, InvokeResult::Error)
        << "Returning a number should be Error, not Commit";
    EXPECT_EQ(engine.script_error_count(), 1u);

    engine.finalize();
}

TEST_F(LuaEngineTest, InvokeProduce_WrongReturnString_IsError)
{
    // Returning a string instead of boolean should be an Error.
    write_script(R"(
        function on_produce(out_slot, fz, msgs, api)
            return "ok"
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;

    auto result = engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);
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
        function on_produce(out_slot, fz, msgs, api)
            error("intentional error")
        end
    )");

    RoleHostCore core;
    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test"));
    ASSERT_TRUE(engine.load_script(tmp_, "init.lua", "on_produce"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "SlotFrame", "aligned"));

    auto ctx = producer_context();
    ctx.core = &core;
    ctx.stop_on_script_error = true;  // enable
    engine.build_api(ctx);

    EXPECT_FALSE(core.shutdown_requested.load());

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    auto result = engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);

    EXPECT_EQ(result, InvokeResult::Error);
    EXPECT_TRUE(core.shutdown_requested.load())
        << "stop_on_script_error should set shutdown_requested on error";

    engine.finalize();
}

// ============================================================================
// 21. Negative paths: load_script failures
// ============================================================================

TEST_F(LuaEngineTest, LoadScript_MissingFile_ReturnsFalse)
{
    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test"));
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
    ASSERT_TRUE(engine.initialize("test"));
    EXPECT_FALSE(engine.load_script(tmp_, "init.lua", "on_produce"));
    engine.finalize();
}

TEST_F(LuaEngineTest, RegisterSlotType_BadFieldType_ReturnsFalse)
{
    write_script("function on_produce(out_slot, fz, msgs, api) return true end");

    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test"));
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

} // anonymous namespace
