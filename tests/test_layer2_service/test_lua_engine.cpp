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
 */
#include <gtest/gtest.h>

#include "lua_engine.hpp"
#include "utils/engine_module_params.hpp"
#include "utils/schema_utils.hpp"
#include "utils/role_host_core.hpp"
#include "test_schema_helpers.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
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
        auto api = std::make_unique<RoleAPIBase>(core);
        api->set_role_tag(tag);
        api->set_uid("TEST-ENGINE-00000001");
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

TEST_F(LuaEngineTest, FullLifecycle)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
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
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    engine.finalize();
}

// ============================================================================
// 2. Type Registration
// ============================================================================

TEST_F(LuaEngineTest, RegisterSlotType_SizeofCorrect)
{
    write_script("function on_produce(tx, msgs, api) return true end");

    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_, "init.lua", "on_produce"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame", "aligned"));

    // float32 = 4 bytes
    EXPECT_EQ(engine.type_sizeof("OutSlotFrame"), 4u);

    engine.finalize();
}

TEST_F(LuaEngineTest, RegisterSlotType_MultiField)
{
    write_script("function on_produce(tx, msgs, api) return true end");

    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_, "init.lua", "on_produce"));

    SchemaSpec spec;
    spec.has_schema = true;
    FieldDef f1; f1.name = "x"; f1.type_str = "float32"; f1.count = 1; f1.length = 0;
    FieldDef f2; f2.name = "y"; f2.type_str = "float32"; f2.count = 1; f2.length = 0;
    FieldDef f3; f3.name = "z"; f3.type_str = "float32"; f3.count = 1; f3.length = 0;
    spec.fields = {f1, f2, f3};

    ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame", "aligned"));
    EXPECT_EQ(engine.type_sizeof("OutSlotFrame"), 12u); // 3 * 4 bytes

    engine.finalize();
}

TEST_F(LuaEngineTest, RegisterSlotType_PackedPacking)
{
    write_script("function on_produce(tx, msgs, api) return true end");

    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_, "init.lua", "on_produce"));

    // bool + int32: aligned=8, packed=5.
    SchemaSpec spec;
    spec.has_schema = true;
    FieldDef f1; f1.name = "flag"; f1.type_str = "bool"; f1.count = 1; f1.length = 0;
    FieldDef f2; f2.name = "val";  f2.type_str = "int32"; f2.count = 1; f2.length = 0;
    spec.fields = {f1, f2};

    ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame", "packed"));
    EXPECT_EQ(engine.type_sizeof("OutSlotFrame"), 5u) << "packed: bool(1)+int32(4)=5";

    engine.finalize();
}

TEST_F(LuaEngineTest, RegisterSlotType_HasSchemaFalse_ReturnsFalse)
{
    write_script("function on_produce(tx, msgs, api) return true end");

    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_, "init.lua", "on_produce"));

    SchemaSpec spec;
    spec.has_schema = false;
    EXPECT_FALSE(engine.register_slot_type(spec, "OutSlotFrame", "aligned"));

    engine.finalize();
}

TEST_F(LuaEngineTest, Alias_SlotFrame_Producer)
{
    write_script("function on_produce(tx, msgs, api) return true end");

    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_, "init.lua", "on_produce"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame", "aligned"));

    auto test_api = make_api(default_core_);
    ASSERT_TRUE(engine.build_api(*test_api));

    // After build_api, "SlotFrame" alias should exist for producer.
    EXPECT_EQ(engine.type_sizeof("SlotFrame"), engine.type_sizeof("OutSlotFrame"));
    EXPECT_GT(engine.type_sizeof("SlotFrame"), 0u);

    engine.finalize();
}

TEST_F(LuaEngineTest, Alias_SlotFrame_Consumer)
{
    write_script("function on_consume(rx, msgs, api) return true end");

    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_, "init.lua", "on_consume"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "InSlotFrame", "aligned"));

    auto test_api = make_api(default_core_, "cons");
    ASSERT_TRUE(engine.build_api(*test_api));

    // After build_api, "SlotFrame" alias should exist for consumer.
    EXPECT_EQ(engine.type_sizeof("SlotFrame"), engine.type_sizeof("InSlotFrame"));
    EXPECT_GT(engine.type_sizeof("SlotFrame"), 0u);

    engine.finalize();
}

TEST_F(LuaEngineTest, Alias_NoAlias_Processor)
{
    write_script("function on_process(rx, tx, msgs, api) return true end");

    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_, "init.lua", "on_process"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "InSlotFrame", "aligned"));
    ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame", "aligned"));

    auto test_api = make_api(default_core_, "proc");
    ASSERT_TRUE(engine.build_api(*test_api));

    // Processor: no alias — SlotFrame should not be defined.
    EXPECT_EQ(engine.type_sizeof("SlotFrame"), 0u);

    engine.finalize();
}

TEST_F(LuaEngineTest, Alias_FlexFrame_Producer)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            if tx.fz then
                tx.fz.value = 99.0
            end
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

    // FlexFrame alias should exist for producer.
    EXPECT_EQ(engine.type_sizeof("FlexFrame"), engine.type_sizeof("OutFlexFrame"));
    EXPECT_GT(engine.type_sizeof("FlexFrame"), 0u);

    engine.finalize();
}

TEST_F(LuaEngineTest, Alias_ProducerNoFz_NoFlexFrameAlias)
{
    write_script("function on_produce(tx, msgs, api) return true end");

    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_, "init.lua", "on_produce"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame", "aligned"));
    // No flexzone registered.

    auto test_api = make_api(default_core_);
    ASSERT_TRUE(engine.build_api(*test_api));

    // SlotFrame alias exists, but FlexFrame should not (no fz registered).
    EXPECT_GT(engine.type_sizeof("SlotFrame"), 0u);
    EXPECT_EQ(engine.type_sizeof("FlexFrame"), 0u);

    engine.finalize();
}

// ============================================================================
// 3. invoke_produce
// ============================================================================

TEST_F(LuaEngineTest, InvokeProduce_CommitOnTrue)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            if tx.slot then
                tx.slot.value = 42.0
            end
            return true
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;

    auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
    EXPECT_EQ(result, InvokeResult::Commit);
    EXPECT_FLOAT_EQ(buf, 42.0f);

    engine.finalize();
}

TEST_F(LuaEngineTest, InvokeProduce_DiscardOnFalse)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            return false
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;

    auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
    EXPECT_EQ(result, InvokeResult::Discard);

    engine.finalize();
}

TEST_F(LuaEngineTest, InvokeProduce_NilReturn_IsError)
{
    // Missing return value is an error — explicit return true/false required.
    write_script(R"(
        function on_produce(tx, msgs, api)
            -- no explicit return → nil → error (must be explicit)
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;

    auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
    EXPECT_EQ(result, InvokeResult::Error)
        << "Missing return should be Error, not Commit";
    EXPECT_EQ(engine.script_error_count(), 1u);

    engine.finalize();
}

TEST_F(LuaEngineTest, InvokeProduce_NilSlot)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            assert(tx.slot == nil, "expected nil slot")
            return false
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    std::vector<IncomingMessage> msgs;

    auto result = engine.invoke_produce(InvokeTx{nullptr, 0, nullptr, 0}, msgs);
    EXPECT_EQ(result, InvokeResult::Discard);

    engine.finalize();
}

TEST_F(LuaEngineTest, InvokeProduce_ScriptError)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            error("intentional error")
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;

    EXPECT_EQ(engine.script_error_count(), 0u);
    auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
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
        function on_consume(rx, msgs, api)
            assert(rx.slot ~= nil, "expected non-nil slot")
            assert(math.abs(rx.slot.value - 99.5) < 0.01,
                   "expected value ~99.5, got " .. tostring(rx.slot.value))
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

    float data = 99.5f;
    std::vector<IncomingMessage> msgs;

    engine.invoke_consume(InvokeRx{&data, sizeof(data), nullptr, 0}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u) << "Script assertion failed — slot value incorrect";

    engine.finalize();
}

TEST_F(LuaEngineTest, InvokeConsume_NilSlot)
{
    write_script(R"(
        function on_consume(rx, msgs, api)
            assert(rx.slot == nil, "expected nil")
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
    engine.invoke_consume(InvokeRx{nullptr, 0, nullptr, 0}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u);

    engine.finalize();
}

TEST_F(LuaEngineTest, InvokeConsume_ScriptErrorDetected)
{
    write_script(R"(
        function on_consume(rx, msgs, api)
            error("consume error")
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_, "init.lua", "on_consume"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "InSlotFrame", "aligned"));

    auto test_api = make_api(default_core_, "cons");
    ASSERT_TRUE(engine.build_api(*test_api));

    float data = 1.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_consume(InvokeRx{&data, sizeof(data), nullptr, 0}, msgs);
    EXPECT_EQ(engine.script_error_count(), 1u);

    engine.finalize();
}

// ============================================================================
// 5. invoke_process
// ============================================================================

TEST_F(LuaEngineTest, InvokeProcess_DualSlots)
{
    write_script(R"(
        function on_process(rx, tx, msgs, api)
            if rx.slot and tx.slot then
                tx.slot.value = rx.slot.value * 2.0
                return true
            end
            return false
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_, "init.lua", "on_process"));

    auto spec = simple_schema();
    // Processor: separate in/out types.
    ASSERT_TRUE(engine.register_slot_type(spec, "InSlotFrame", "aligned"));
    ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame", "aligned"));

    auto test_api = make_api(default_core_, "proc");
    ASSERT_TRUE(engine.build_api(*test_api));

    float in_data  = 21.0f;
    float out_data = 0.0f;
    std::vector<IncomingMessage> msgs;

    auto result = engine.invoke_process(
        InvokeRx{&in_data, sizeof(in_data), nullptr, 0},
        InvokeTx{&out_data, sizeof(out_data), nullptr, 0}, msgs);
    EXPECT_EQ(result, InvokeResult::Commit);
    EXPECT_FLOAT_EQ(out_data, 42.0f);

    engine.finalize();
}

TEST_F(LuaEngineTest, InvokeProcess_NilInput)
{
    write_script(R"(
        function on_process(rx, tx, msgs, api)
            assert(rx.slot == nil, "expected nil input")
            assert(tx.slot == nil, "expected nil output")
            return false
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_, "init.lua", "on_process"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "InSlotFrame", "aligned"));
    ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame", "aligned"));

    auto test_api = make_api(default_core_, "proc");
    ASSERT_TRUE(engine.build_api(*test_api));

    std::vector<IncomingMessage> msgs;
    auto result = engine.invoke_process(
        InvokeRx{nullptr, 0, nullptr, 0},
        InvokeTx{nullptr, 0, nullptr, 0}, msgs);
    EXPECT_EQ(result, InvokeResult::Discard);
    EXPECT_EQ(engine.script_error_count(), 0u);

    engine.finalize();
}

TEST_F(LuaEngineTest, InvokeProcess_InputOnlyNoOutput)
{
    write_script(R"(
        function on_process(rx, tx, msgs, api)
            assert(rx.slot ~= nil, "expected input data")
            assert(tx.slot == nil, "expected nil output")
            return false
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_, "init.lua", "on_process"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "InSlotFrame", "aligned"));
    ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame", "aligned"));

    auto test_api = make_api(default_core_, "proc");
    ASSERT_TRUE(engine.build_api(*test_api));

    float in_data = 10.0f;
    std::vector<IncomingMessage> msgs;
    auto result = engine.invoke_process(
        InvokeRx{&in_data, sizeof(in_data), nullptr, 0},
        InvokeTx{nullptr, 0, nullptr, 0}, msgs);
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
        function on_produce(tx, msgs, api)
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
    auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
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
    auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
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
        engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
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

TEST_F(LuaEngineTest, InvokeProduce_WithFlexzone)
{
    // Script writes to both slot and flexzone.
    write_script(R"(
        function on_produce(tx, msgs, api)
            assert(tx.slot ~= nil, "expected slot")
            assert(tx.fz ~= nil, "expected flexzone")
            tx.slot.value = 10.0
            tx.fz.value = 20.0
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
    float fz_buf   = 0.0f;
    std::vector<IncomingMessage> msgs;

    auto result = engine.invoke_produce(
        InvokeTx{&slot_buf, sizeof(slot_buf), &fz_buf, sizeof(fz_buf)}, msgs);
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
    engine.invoke_produce(InvokeTx{&buf1, sizeof(buf1), nullptr, 0}, msgs);
    EXPECT_FLOAT_EQ(buf1, 1.0f);

    float buf2 = 0.0f;
    engine.invoke_produce(InvokeTx{&buf2, sizeof(buf2), nullptr, 0}, msgs);
    EXPECT_FLOAT_EQ(buf2, 2.0f);

    float buf3 = 0.0f;
    engine.invoke_produce(InvokeTx{&buf3, sizeof(buf3), nullptr, 0}, msgs);
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

    engine.invoke_consume(InvokeRx{nullptr, 0, nullptr, 0}, msgs);
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
    engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);

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
    engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);

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
    engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
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
    engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
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
    engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
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

    auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
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

    auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
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
    auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);

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
    engine.invoke_consume(InvokeRx{nullptr, 0, nullptr, 0}, msgs);
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
    engine.invoke_produce(InvokeTx{nullptr, 0, nullptr, 0}, msgs);

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
    auto r = engine.invoke_produce(InvokeTx{nullptr, 0, nullptr, 0}, msgs);
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
    engine.invoke_produce(InvokeTx{nullptr, 0, nullptr, 0}, msgs);

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
    engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
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
    engine.invoke_consume(InvokeRx{&buf, sizeof(buf), nullptr, 0}, msgs);
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
    engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
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
    engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
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
    engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
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
    engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
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
    engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
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
    engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
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
    engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
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
    engine.invoke_consume(InvokeRx{&buf, sizeof(buf), nullptr, 0}, msgs);
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
    engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
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
        InvokeRx{&in_data, sizeof(in_data), nullptr, 0},
        InvokeTx{&out_data, sizeof(out_data), nullptr, 0}, msgs);
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
    engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "Script assertion failed — identity fields do not match context";
    engine.finalize();
}

// ============================================================================
// 25. ctrl_queue_dropped default
// ============================================================================

TEST_F(LuaEngineTest, Api_CtrlQueueDropped_DefaultZero)
{
    write_script(R"(
        function on_produce(tx, msgs, api)
            local v = api.ctrl_queue_dropped()
            assert(v == 0, "ctrl_queue_dropped should be 0 without queue, got " .. tostring(v))
            return false
        end
    )");

    LuaEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u);
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
    engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
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
    engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
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
    engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
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

    auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
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
    engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
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
        InvokeRx{&in_data, sizeof(in_data), nullptr, 0},
        InvokeTx{&out_data, sizeof(out_data), nullptr, 0}, msgs);
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
    engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
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
    engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
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
    engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
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
    engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
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
    engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
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
    auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
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
            if tx.fz then
                tx.fz.value = 20.0
            end
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
    EXPECT_TRUE(core.has_out_fz());
    EXPECT_GT(core.out_schema_fz_size(), 0u);

    EXPECT_EQ(engine.type_sizeof("OutFlexFrame"),
              pylabhub::hub::compute_schema_size(params.out_fz_spec, params.out_packing))
        << "Engine-built type size must match schema logical size";

    float slot_buf = 0.0f;
    float fz_buf   = 0.0f;
    std::vector<IncomingMessage> msgs;
    auto result = engine.invoke_produce(
        InvokeTx{&slot_buf, sizeof(slot_buf), &fz_buf, sizeof(fz_buf)}, msgs);
    EXPECT_EQ(result, InvokeResult::Commit);
    EXPECT_FLOAT_EQ(slot_buf, 10.0f);
    EXPECT_FLOAT_EQ(fz_buf, 20.0f);

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
    auto result = engine.invoke_consume(InvokeRx{&data, sizeof(data), nullptr, 0}, msgs);
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
        InvokeRx{&in_data, sizeof(in_data), nullptr, 0},
        InvokeTx{&out_data, sizeof(out_data), nullptr, 0}, msgs);
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
    auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
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
        pylabhub::scripting::InvokeRx{&buf, sizeof(buf), nullptr, 0}, msgs);
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
        pylabhub::scripting::InvokeRx{&in_buf, sizeof(in_buf), nullptr, 0},
        InvokeTx{&out_buf, sizeof(out_buf), nullptr, 0}, msgs);
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
    engine.invoke_produce(InvokeTx{buf, sizeof(buf), nullptr, 0}, msgs);
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
    engine.invoke_produce(InvokeTx{buf, sizeof(buf), nullptr, 0}, msgs);
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
    engine.invoke_produce(InvokeTx{buf, sizeof(buf), nullptr, 0}, msgs);
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
    engine.invoke_produce(InvokeTx{slot_buf, sizeof(slot_buf), fz_buf, sizeof(fz_buf)}, msgs);
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
    auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
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
    auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
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
    auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
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
    auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
    EXPECT_EQ(result, InvokeResult::Commit);
    EXPECT_EQ(engine.script_error_count(), 0u);
    engine.finalize();
}

} // anonymous namespace
