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
#include "test_schema_helpers.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
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
        auto api = std::make_unique<RoleAPIBase>(core);
        api->set_role_tag(tag);
        // Use role-appropriate UID format matching production conventions.
        if (tag == "prod")
            api->set_uid("PROD-TestEngine-00000001");
        else if (tag == "cons")
            api->set_uid("CONS-TestEngine-00000001");
        else if (tag == "proc")
            api->set_uid("PROC-TestEngine-00000001");
        else
            api->set_uid("TEST-" + tag + "-00000001");
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
// 1. Lifecycle
// ============================================================================

TEST_F(PythonEngineTest, FullLifecycle)
{
    write_script(
        "def on_produce(tx, msgs, api):\n"
        "    return True\n"
        "\n"
        "def on_init(api):\n"
        "    pass\n"
        "\n"
        "def on_stop(api):\n"
        "    pass\n");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    engine.invoke_on_init();
    engine.invoke_on_stop();
    engine.finalize();
}

TEST_F(PythonEngineTest, InitializeFailsGracefully)
{
    // Double initialize should not crash (engine manages state).
    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    engine.finalize();
}

// ============================================================================
// 2. Type Registration
// ============================================================================

TEST_F(PythonEngineTest, RegisterSlotType_SizeofCorrect)
{
    write_script("def on_produce(tx, msgs, api):\n    return True\n");

    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_produce"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame", "aligned"));

    // float32 = 4 bytes
    EXPECT_EQ(engine.type_sizeof("OutSlotFrame"), 4u);

    engine.finalize();
}

TEST_F(PythonEngineTest, RegisterSlotType_MultiField)
{
    write_script("def on_produce(tx, msgs, api):\n    return True\n");

    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_produce"));

    SchemaSpec spec;
    spec.has_schema = true;
    FieldDef f1;
    f1.name = "x"; f1.type_str = "float32"; f1.count = 1; f1.length = 0;
    FieldDef f2;
    f2.name = "y"; f2.type_str = "float32"; f2.count = 1; f2.length = 0;
    FieldDef f3;
    f3.name = "z"; f3.type_str = "float32"; f3.count = 1; f3.length = 0;
    spec.fields = {f1, f2, f3};

    ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame", "aligned"));
    EXPECT_EQ(engine.type_sizeof("OutSlotFrame"), 12u); // 3 * 4 bytes

    engine.finalize();
}

TEST_F(PythonEngineTest, RegisterSlotType_PackedPacking)
{
    write_script("def on_produce(tx, msgs, api):\n    return True\n");

    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_produce"));

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

TEST_F(PythonEngineTest, RegisterSlotType_HasSchemaFalse_ReturnsFalse)
{
    write_script("def on_produce(tx, msgs, api):\n    return True\n");

    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_produce"));

    SchemaSpec spec;
    spec.has_schema = false;  // No schema — should fail
    EXPECT_FALSE(engine.register_slot_type(spec, "OutSlotFrame", "aligned"));

    engine.finalize();
}

// ============================================================================
// 2b. Alias tests — role-specific type aliases
// ============================================================================

TEST_F(PythonEngineTest, Alias_SlotFrame_Producer)
{
    write_script("def on_produce(tx, msgs, api):\n    return True\n");

    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_produce"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame", "aligned"));

    auto test_api = make_api(default_core_);
    ASSERT_TRUE(engine.build_api(*test_api));

    // After build_api, "SlotFrame" alias should exist for producer.
    EXPECT_EQ(engine.type_sizeof("SlotFrame"), engine.type_sizeof("OutSlotFrame"));
    EXPECT_GT(engine.type_sizeof("SlotFrame"), 0u);

    engine.finalize();
}

TEST_F(PythonEngineTest, Alias_SlotFrame_Consumer)
{
    write_script("def on_consume(rx, msgs, api):\n    return True\n");

    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_consume"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "InSlotFrame", "aligned"));

    auto test_api = make_api(default_core_, "cons");
    ASSERT_TRUE(engine.build_api(*test_api));

    EXPECT_EQ(engine.type_sizeof("SlotFrame"), engine.type_sizeof("InSlotFrame"));
    EXPECT_GT(engine.type_sizeof("SlotFrame"), 0u);

    engine.finalize();
}

TEST_F(PythonEngineTest, Alias_NoAlias_Processor)
{
    write_script("def on_process(rx, tx, msgs, api):\n    return True\n");

    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_process"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "InSlotFrame", "aligned"));
    ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame", "aligned"));

    auto test_api = make_api(default_core_, "proc");
    ASSERT_TRUE(engine.build_api(*test_api));

    // Processor: no alias — SlotFrame should not resolve.
    EXPECT_EQ(engine.type_sizeof("SlotFrame"), 0u);

    engine.finalize();
}

TEST_F(PythonEngineTest, Alias_FlexFrame_Producer)
{
    write_script(
        "def on_produce(tx, msgs, api):\n"
        "    return True\n");

    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_produce"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame", "aligned"));
    ASSERT_TRUE(engine.register_slot_type(spec, "OutFlexFrame", "aligned"));

    auto test_api = make_api(default_core_);
    ASSERT_TRUE(engine.build_api(*test_api));

    EXPECT_EQ(engine.type_sizeof("FlexFrame"), engine.type_sizeof("OutFlexFrame"));
    EXPECT_GT(engine.type_sizeof("FlexFrame"), 0u);

    engine.finalize();
}

TEST_F(PythonEngineTest, Alias_ProducerNoFz_NoFlexFrameAlias)
{
    write_script("def on_produce(tx, msgs, api):\n    return True\n");

    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_produce"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame", "aligned"));
    // No flexzone registered.

    auto test_api = make_api(default_core_);
    ASSERT_TRUE(engine.build_api(*test_api));

    EXPECT_GT(engine.type_sizeof("SlotFrame"), 0u);
    EXPECT_EQ(engine.type_sizeof("FlexFrame"), 0u);

    engine.finalize();
}

// ============================================================================
// 3. invoke_produce
// ============================================================================

TEST_F(PythonEngineTest, InvokeProduce_CommitOnTrue)
{
    write_script(
        "def on_produce(tx, msgs, api):\n"
        "    if tx.slot is not None:\n"
        "        tx.slot.value = 42.0\n"
        "    return True\n");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;

    auto result =
        engine.invoke_produce({&buf, sizeof(buf), nullptr, 0}, msgs);
    EXPECT_EQ(result, InvokeResult::Commit);
    EXPECT_FLOAT_EQ(buf, 42.0f);

    engine.finalize();
}

TEST_F(PythonEngineTest, InvokeProduce_DiscardOnFalse)
{
    write_script(
        "def on_produce(tx, msgs, api):\n"
        "    return False\n");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;

    auto result =
        engine.invoke_produce({&buf, sizeof(buf), nullptr, 0}, msgs);
    EXPECT_EQ(result, InvokeResult::Discard);

    engine.finalize();
}

TEST_F(PythonEngineTest, InvokeProduce_NoneReturn_IsError)
{
    // Missing return value (implicit None) is an error.
    write_script(
        "def on_produce(tx, msgs, api):\n"
        "    pass  # no explicit return -> None -> error\n");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;

    auto result =
        engine.invoke_produce({&buf, sizeof(buf), nullptr, 0}, msgs);
    EXPECT_EQ(result, InvokeResult::Error)
        << "Missing return should be Error, not Commit";
    EXPECT_EQ(engine.script_error_count(), 1u);

    engine.finalize();
}

TEST_F(PythonEngineTest, InvokeProduce_NoneSlot)
{
    write_script(
        "def on_produce(tx, msgs, api):\n"
        "    assert tx.slot is None, 'expected None slot'\n"
        "    return False\n");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    std::vector<IncomingMessage> msgs;

    auto result =
        engine.invoke_produce({nullptr, 0, nullptr, 0}, msgs);
    EXPECT_EQ(result, InvokeResult::Discard);

    engine.finalize();
}

TEST_F(PythonEngineTest, InvokeProduce_ScriptError)
{
    write_script(
        "def on_produce(tx, msgs, api):\n"
        "    raise RuntimeError('intentional error')\n");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;

    EXPECT_EQ(engine.script_error_count(), 0u);
    auto result =
        engine.invoke_produce({&buf, sizeof(buf), nullptr, 0}, msgs);
    EXPECT_EQ(result, InvokeResult::Error);
    EXPECT_EQ(engine.script_error_count(), 1u);

    engine.finalize();
}

TEST_F(PythonEngineTest, InvokeProduce_WrongReturnType_IsError)
{
    // Returning a number instead of boolean should be an Error.
    write_script(
        "def on_produce(tx, msgs, api):\n"
        "    return 42\n");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;

    auto result =
        engine.invoke_produce({&buf, sizeof(buf), nullptr, 0}, msgs);
    EXPECT_EQ(result, InvokeResult::Error)
        << "Returning a number should be Error, not Commit";
    EXPECT_EQ(engine.script_error_count(), 1u);

    engine.finalize();
}

TEST_F(PythonEngineTest, InvokeProduce_WrongReturnString_IsError)
{
    // Returning a string instead of boolean should be an Error.
    write_script(
        "def on_produce(tx, msgs, api):\n"
        "    return 'ok'\n");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;

    auto result =
        engine.invoke_produce({&buf, sizeof(buf), nullptr, 0}, msgs);
    EXPECT_EQ(result, InvokeResult::Error)
        << "Returning a string should be Error, not Commit";
    EXPECT_EQ(engine.script_error_count(), 1u);

    engine.finalize();
}

// ============================================================================
// 4. invoke_consume
// ============================================================================

TEST_F(PythonEngineTest, InvokeConsume_ReceivesReadOnlySlot)
{
    // Script asserts the value -- if wrong, AssertionError is raised.
    write_script(
        "def on_consume(rx, msgs, api):\n"
        "    assert rx.slot is not None, 'expected non-None slot'\n"
        "    assert abs(rx.slot.value - 99.5) < 0.01, (\n"
        "        f'expected value ~99.5, got {rx.slot.value}')\n"
        "    return True\n");

    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_consume"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "InSlotFrame", "aligned"));

    auto test_api = make_api(default_core_, "cons");
    ASSERT_TRUE(engine.build_api(*test_api));

    float data = 99.5f;
    std::vector<IncomingMessage> msgs;

    engine.invoke_consume(InvokeRx{&data, sizeof(data), nullptr, 0}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "Script assertion failed -- slot value incorrect";

    engine.finalize();
}

TEST_F(PythonEngineTest, InvokeConsume_NoneSlot)
{
    write_script(
        "def on_consume(rx, msgs, api):\n"
        "    assert rx.slot is None, 'expected None'\n"
        "    return True\n");

    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_consume"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "InSlotFrame", "aligned"));

    auto test_api = make_api(default_core_, "cons");
    ASSERT_TRUE(engine.build_api(*test_api));

    std::vector<IncomingMessage> msgs;
    engine.invoke_consume(InvokeRx{nullptr, 0, nullptr, 0}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u);

    engine.finalize();
}

TEST_F(PythonEngineTest, InvokeConsume_ScriptErrorDetected)
{
    write_script(
        "def on_consume(rx, msgs, api):\n"
        "    raise RuntimeError('consume error')\n");

    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_consume"));

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

TEST_F(PythonEngineTest, InvokeProcess_DualSlots)
{
    write_script(
        "def on_process(rx, tx, msgs, api):\n"
        "    if rx.slot is not None and tx.slot is not None:\n"
        "        tx.slot.value = rx.slot.value * 2.0\n"
        "        return True\n"
        "    return False\n");

    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_process"));

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

TEST_F(PythonEngineTest, InvokeProcess_NoneInput)
{
    write_script(
        "def on_process(rx, tx, msgs, api):\n"
        "    assert rx.slot is None, 'expected None input'\n"
        "    assert tx.slot is None, 'expected None output'\n"
        "    return False\n");

    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_process"));

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

TEST_F(PythonEngineTest, InvokeProcess_InputOnlyNoOutput)
{
    write_script(
        "def on_process(rx, tx, msgs, api):\n"
        "    assert rx.slot is not None, 'expected input data'\n"
        "    assert tx.slot is None, 'expected None output'\n"
        "    return False\n");

    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_process"));

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

TEST_F(PythonEngineTest, InvokeProduce_ReceivesMessages)
{
    // Producer messages: events are dicts, data messages are (sender_hex, bytes) tuples.
    write_script(
        "def on_produce(tx, msgs, api):\n"
        "    event_count = sum(1 for m in msgs if isinstance(m, dict) and 'event' in m)\n"
        "    assert event_count == 2, (\n"
        "        f'expected 2 events, got {event_count}')\n"
        "    assert msgs[0]['event'] == 'consumer_joined', (\n"
        "        f'expected consumer_joined, got {msgs[0][\"event\"]}')\n"
        "    assert msgs[1]['event'] == 'channel_closing', (\n"
        "        f'expected channel_closing, got {msgs[1][\"event\"]}')\n"
        "    return False\n");

    PythonEngine engine;
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
    auto result =
        engine.invoke_produce({&buf, sizeof(buf), nullptr, 0}, msgs);
    EXPECT_EQ(result, InvokeResult::Discard);
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "Script assertion failed -- messages not received correctly";

    engine.finalize();
}

TEST_F(PythonEngineTest, InvokeConsume_BareDataMessages)
{
    // Consumer data messages are bare bytes (not tuples). Events are still dicts.
    write_script(
        "def on_consume(rx, msgs, api):\n"
        "    assert len(msgs) == 2, f'expected 2 messages, got {len(msgs)}'\n"
        "    # First msg: data message -> bare bytes\n"
        "    assert isinstance(msgs[0], bytes), (\n"
        "        f'data msg should be bytes, got {type(msgs[0])}')\n"
        "    # Second msg: event message -> dict with .event\n"
        "    assert isinstance(msgs[1], dict), (\n"
        "        f'event msg should be dict, got {type(msgs[1])}')\n"
        "    assert msgs[1]['event'] == 'channel_closing'\n"
        "    return True\n");

    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_consume"));

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
        << "Script assertion failed -- consumer message format incorrect";

    engine.finalize();
}

// ============================================================================
// 7. API closures
// ============================================================================

TEST_F(PythonEngineTest, ApiVersionInfo)
{
    // version_info() is a module-level function in the pybind11 module,
    // not a method on the API object. Import it from the role's module.
    write_script(
        "import pylabhub_producer\n"
        "\n"
        "def on_produce(tx, msgs, api):\n"
        "    info = pylabhub_producer.version_info()\n"
        "    assert isinstance(info, str), 'version_info should return string'\n"
        "    assert len(info) > 10, f'version_info too short: {info}'\n"
        "    assert '{' in info, f'version_info should be JSON: {info}'\n"
        "    return False\n");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    auto result =
        engine.invoke_produce({&buf, sizeof(buf), nullptr, 0}, msgs);
    EXPECT_EQ(result, InvokeResult::Discard);
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "Script assertion failed -- version_info returned unexpected value";

    engine.finalize();
}

TEST_F(PythonEngineTest, WrongRoleModuleImport_RaisesError)
{
    // After build_api, importing the wrong role module must raise an error,
    // not segfault. build_api removes inactive modules from sys.modules.
    write_script(
        "def on_produce(tx, msgs, api):\n"
        "    return False\n");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    // Engine is set up as producer. Importing consumer/processor should fail.
    auto result = engine.eval("__import__('pylabhub_consumer')");
    EXPECT_EQ(result.status, InvokeStatus::ScriptError)
        << "Expected ScriptError from wrong module import";
    EXPECT_GE(engine.script_error_count(), 1u)
        << "Wrong module import should increment error count";

    engine.finalize();
}

TEST_F(PythonEngineTest, ApiStop_SetsShutdownRequested)
{
    write_script(
        "def on_produce(tx, msgs, api):\n"
        "    api.stop()\n"
        "    return False\n");

    RoleHostCore core;
    PythonEngine engine;
    ASSERT_TRUE(setup_engine_with_core(engine, core));

    EXPECT_FALSE(core.is_shutdown_requested());

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce({&buf, sizeof(buf), nullptr, 0}, msgs);

    EXPECT_TRUE(core.is_shutdown_requested())
        << "api.stop() should set shutdown_requested";

    engine.finalize();
}

TEST_F(PythonEngineTest, ApiSetCriticalError)
{
    // Python API's set_critical_error() takes no arguments (unlike Lua).
    write_script(
        "def on_produce(tx, msgs, api):\n"
        "    api.set_critical_error()\n"
        "    return False\n");

    RoleHostCore core;
    PythonEngine engine;
    ASSERT_TRUE(setup_engine_with_core(engine, core));

    EXPECT_FALSE(core.is_critical_error());
    EXPECT_FALSE(core.is_shutdown_requested());

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce({&buf, sizeof(buf), nullptr, 0}, msgs);

    EXPECT_TRUE(core.is_critical_error())
        << "api.set_critical_error() should set critical_error_";
    EXPECT_TRUE(core.is_shutdown_requested())
        << "api.set_critical_error() should also set shutdown_requested";

    engine.finalize();
}

TEST_F(PythonEngineTest, ApiStopReason_Default)
{
    write_script(
        "def on_produce(tx, msgs, api):\n"
        "    reason = api.stop_reason()\n"
        "    assert reason == 'normal', (\n"
        "        f\"expected 'normal', got '{reason}'\")\n"
        "    return False\n");

    RoleHostCore core;
    PythonEngine engine;
    ASSERT_TRUE(setup_engine_with_core(engine, core));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce({&buf, sizeof(buf), nullptr, 0}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "stop_reason should be 'normal'";

    engine.finalize();
}

TEST_F(PythonEngineTest, ApiStopReason_PeerDead)
{
    write_script(
        "def on_produce(tx, msgs, api):\n"
        "    reason = api.stop_reason()\n"
        "    assert reason == 'peer_dead', (\n"
        "        f\"expected 'peer_dead', got '{reason}'\")\n"
        "    return False\n");

    RoleHostCore core;
    core.set_stop_reason(RoleHostCore::StopReason::PeerDead); // PeerDead

    PythonEngine engine;
    ASSERT_TRUE(setup_engine_with_core(engine, core));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce({&buf, sizeof(buf), nullptr, 0}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "stop_reason should be 'peer_dead'";

    engine.finalize();
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
    engine.invoke_produce({&buf, sizeof(buf), nullptr, 0}, msgs);
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
    engine.invoke_consume(InvokeRx{nullptr, 0, nullptr, 0}, msgs);
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
        engine.invoke_produce({&buf, sizeof(buf), nullptr, 0}, msgs);
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
        engine.invoke_produce({&buf, sizeof(buf), nullptr, 0}, msgs);

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
// 12. Flexzone
// ============================================================================

TEST_F(PythonEngineTest, InvokeProduce_WithFlexzone)
{
    // Script writes to both slot and flexzone.
    write_script(
        "def on_produce(tx, msgs, api):\n"
        "    assert tx.slot is not None, 'expected slot'\n"
        "    assert tx.fz is not None, 'expected flexzone'\n"
        "    tx.slot.value = 10.0\n"
        "    tx.fz.value = 20.0\n"
        "    return True\n");

    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_produce"));

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
// 14. supports_multi_state
// ============================================================================

TEST_F(PythonEngineTest, SupportsMultiState_ReturnsFalse)
{
    PythonEngine engine;
    EXPECT_FALSE(engine.supports_multi_state());
}

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
        engine.invoke_produce({nullptr, 0, nullptr, 0}, msgs);
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
        engine.invoke_produce({nullptr, 0, nullptr, 0}, msgs);
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
        engine.invoke_produce({nullptr, 0, nullptr, 0}, msgs);

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
    engine.invoke_produce({&buf, sizeof(buf), nullptr, 0}, msgs);
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
    engine.invoke_produce({&buf, sizeof(buf), nullptr, 0}, msgs);
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
    engine.invoke_produce({&buf, sizeof(buf), nullptr, 0}, msgs);
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
    engine.invoke_produce({&buf, sizeof(buf), nullptr, 0}, msgs);
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
    engine.invoke_produce({&buf, sizeof(buf), nullptr, 0}, msgs);
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
    engine.invoke_produce({&buf, sizeof(buf), nullptr, 0}, msgs);
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
    engine.invoke_produce({&buf, sizeof(buf), nullptr, 0}, msgs);
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
        InvokeRx{nullptr, 0, nullptr, 0},
        InvokeTx{nullptr, 0, nullptr, 0}, msgs);
    EXPECT_EQ(result, InvokeResult::Discard);
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "Script assertion failed -- processor queue-state defaults incorrect";

    engine.finalize();
}

// ============================================================================
// 17. ctrl_queue_dropped default
// ============================================================================

TEST_F(PythonEngineTest, Api_CtrlQueueDropped_DefaultZero)
{
    write_script(R"(
def on_produce(tx, msgs, api):
    v = api.ctrl_queue_dropped()
    assert v == 0, f"expected ctrl_queue_dropped==0, got {v}"
    return False
)");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce({&buf, sizeof(buf), nullptr, 0}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "Script assertion failed -- ctrl_queue_dropped should be 0";

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
    engine.invoke_produce({&buf, sizeof(buf), nullptr, 0}, msgs);
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
    engine.invoke_produce({&buf, sizeof(buf), nullptr, 0}, msgs);
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
    engine.invoke_produce({&buf, sizeof(buf), nullptr, 0}, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "Script assertion failed -- zero-value custom metric incorrect";

    engine.finalize();
}

// ============================================================================
// 20. invoke_produce with empty messages list
// ============================================================================

TEST_F(PythonEngineTest, InvokeProduce_EmptyMessagesList)
{
    write_script(R"(
def on_produce(tx, msgs, api):
    assert len(msgs) == 0, f"expected empty msgs, got {len(msgs)}"
    return False
)");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs; // empty

    auto result =
        engine.invoke_produce({&buf, sizeof(buf), nullptr, 0}, msgs);
    EXPECT_EQ(result, InvokeResult::Discard);
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "Script assertion failed -- empty msgs list incorrect";

    engine.finalize();
}

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
    engine.invoke_produce({&buf, sizeof(buf), nullptr, 0}, msgs);
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
        InvokeRx{&in_data, sizeof(in_data), nullptr, 0},
        InvokeTx{&out_data, sizeof(out_data), nullptr, 0}, msgs);
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
        engine.invoke_produce({&buf, sizeof(buf), nullptr, 0}, msgs);
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
    engine.invoke_produce({&buf, sizeof(buf), nullptr, 0}, msgs);
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
    engine.invoke_consume(InvokeRx{&buf, sizeof(buf), nullptr, 0}, msgs);
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
    engine.invoke_produce({&buf, sizeof(buf), nullptr, 0}, msgs);
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
    engine.invoke_produce({&buf, sizeof(buf), nullptr, 0}, msgs);
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
    engine.invoke_produce({&buf, sizeof(buf), nullptr, 0}, msgs);
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
    engine.invoke_produce({&buf, sizeof(buf), nullptr, 0}, msgs);
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
    auto result = engine.invoke_produce({buf, sizeof(buf), nullptr, 0}, msgs);
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
    engine.invoke_produce({&buf, sizeof(buf), nullptr, 0}, msgs);
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
    auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
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
        "    if tx.fz is not None:\n"
        "        tx.fz.value = 20.0\n"
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
    EXPECT_TRUE(core.has_out_fz());
    EXPECT_GT(core.out_schema_fz_size(), 0u);

    // Cross-check: engine type size must match schema logical size.
    // core.out_schema_fz_size() is page-aligned (physical); engine type is logical.
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
    auto result = engine.invoke_consume(InvokeRx{&data, sizeof(data), nullptr, 0}, msgs);
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
        InvokeRx{&in_data, sizeof(in_data), nullptr, 0},
        InvokeTx{&out_data, sizeof(out_data), nullptr, 0}, msgs);
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
    engine.invoke_produce(InvokeTx{buf, sizeof(buf), nullptr, 0}, msgs);
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
    engine.invoke_produce(InvokeTx{buf, sizeof(buf), nullptr, 0}, msgs);
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
    engine.invoke_produce(InvokeTx{buf, sizeof(buf), nullptr, 0}, msgs);
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
    engine.invoke_produce(InvokeTx{slot_buf, sizeof(slot_buf), fz_buf, sizeof(fz_buf)}, msgs);
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
    auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
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
        pylabhub::scripting::InvokeRx{&buf, sizeof(buf), nullptr, 0}, msgs);
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
// Channel pub/sub API — L2 (no broker; the 4 api methods must be callable
// and return gracefully: None / False / no-op without any broker attached).
// ============================================================================

TEST_F(PythonEngineTest, Api_Channel_AllMethodsGraceful_NoBroker)
{
    write_script(
        "results = {}\n"
        "def on_produce(tx, msgs, api):\n"
        "    results['join'] = api.join_channel('#l2_test')\n"
        "    results['leave'] = api.leave_channel('#l2_test')\n"
        "    api.send_channel_msg('#l2_test', {'hello': 'world'})\n"
        "    results['send_ok'] = True\n"
        "    results['members'] = api.channel_members('#l2_test')\n"
        "    assert results['join'] is None, f\"join={results['join']}\"\n"
        "    assert results['leave'] == False, f\"leave={results['leave']}\"\n"
        "    assert results['members'] is None, f\"members={results['members']}\"\n"
        "    return True\n");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    auto result = engine.invoke_produce(
        InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
    EXPECT_EQ(result, InvokeResult::Commit)
        << "on_produce should commit: all 4 channel methods must return "
           "gracefully (None/False/no-op) without a broker";

    engine.finalize();
}

} // anonymous namespace
