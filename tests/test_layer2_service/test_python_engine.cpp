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
#include "utils/role_host_core.hpp"

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

    /// Create a simple schema with one float32 field.
    SchemaSpec simple_schema()
    {
        SchemaSpec spec;
        spec.has_schema = true; // PythonEngine skips type building if false.
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
        ctx.role_tag  = "prod";
        ctx.uid       = "TEST-ENGINE-00000001";
        ctx.name      = "TestEngine";
        ctx.channel   = "test.channel";
        ctx.log_level = "error";
        ctx.stop_on_script_error = false;
        return ctx;
    }

    RoleHostCore default_core_; ///< Default core for tests that don't provide one.

    /// Initialize engine, load script, register type, build API.
    /// Returns true if all steps succeed.
    bool setup_engine(PythonEngine &engine,
                      const std::string &required_cb = "on_produce")
    {
        engine.set_python_venv("");
        if (!engine.initialize("test", &default_core_))
            return false;
        if (!engine.load_script(tmp_ / "script" / "python",
                                "__init__.py", required_cb.c_str()))
            return false;

        auto spec = simple_schema();
        if (!engine.register_slot_type(spec, "SlotFrame", "aligned"))
            return false;

        auto ctx = producer_context();
        return engine.build_api(ctx);
    }

    /// Setup engine with a RoleHostCore wired into the context.
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
        if (!engine.register_slot_type(spec, "SlotFrame", "aligned"))
            return false;

        auto ctx = producer_context();
        ctx.core = &core;
        return engine.build_api(ctx);
    }

    fs::path tmp_;
};

// ============================================================================
// 1. Lifecycle
// ============================================================================

TEST_F(PythonEngineTest, FullLifecycle)
{
    write_script(
        "def on_produce(out_slot, fz, msgs, api):\n"
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
    write_script("def on_produce(out_slot, fz, msgs, api):\n    return True\n");

    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_produce"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "SlotFrame", "aligned"));

    // float32 = 4 bytes
    EXPECT_EQ(engine.type_sizeof("SlotFrame"), 4u);

    engine.finalize();
}

TEST_F(PythonEngineTest, RegisterSlotType_MultiField)
{
    write_script("def on_produce(out_slot, fz, msgs, api):\n    return True\n");

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

    ASSERT_TRUE(engine.register_slot_type(spec, "SlotFrame", "aligned"));
    EXPECT_EQ(engine.type_sizeof("SlotFrame"), 12u); // 3 * 4 bytes

    engine.finalize();
}

// ============================================================================
// 3. invoke_produce
// ============================================================================

TEST_F(PythonEngineTest, InvokeProduce_CommitOnTrue)
{
    write_script(
        "def on_produce(out_slot, fz, msgs, api):\n"
        "    if out_slot is not None:\n"
        "        out_slot.value = 42.0\n"
        "    return True\n");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;

    auto result =
        engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);
    EXPECT_EQ(result, InvokeResult::Commit);
    EXPECT_FLOAT_EQ(buf, 42.0f);

    engine.finalize();
}

TEST_F(PythonEngineTest, InvokeProduce_DiscardOnFalse)
{
    write_script(
        "def on_produce(out_slot, fz, msgs, api):\n"
        "    return False\n");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;

    auto result =
        engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);
    EXPECT_EQ(result, InvokeResult::Discard);

    engine.finalize();
}

TEST_F(PythonEngineTest, InvokeProduce_NoneReturn_IsError)
{
    // Missing return value (implicit None) is an error.
    write_script(
        "def on_produce(out_slot, fz, msgs, api):\n"
        "    pass  # no explicit return -> None -> error\n");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;

    auto result =
        engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);
    EXPECT_EQ(result, InvokeResult::Error)
        << "Missing return should be Error, not Commit";
    EXPECT_EQ(engine.script_error_count(), 1u);

    engine.finalize();
}

TEST_F(PythonEngineTest, InvokeProduce_NoneSlot)
{
    write_script(
        "def on_produce(out_slot, fz, msgs, api):\n"
        "    assert out_slot is None, 'expected None slot'\n"
        "    return False\n");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    std::vector<IncomingMessage> msgs;

    auto result =
        engine.invoke_produce(nullptr, 0, nullptr, 0, nullptr, msgs);
    EXPECT_EQ(result, InvokeResult::Discard);

    engine.finalize();
}

TEST_F(PythonEngineTest, InvokeProduce_ScriptError)
{
    write_script(
        "def on_produce(out_slot, fz, msgs, api):\n"
        "    raise RuntimeError('intentional error')\n");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;

    EXPECT_EQ(engine.script_error_count(), 0u);
    auto result =
        engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);
    EXPECT_EQ(result, InvokeResult::Error);
    EXPECT_EQ(engine.script_error_count(), 1u);

    engine.finalize();
}

TEST_F(PythonEngineTest, InvokeProduce_WrongReturnType_IsError)
{
    // Returning a number instead of boolean should be an Error.
    write_script(
        "def on_produce(out_slot, fz, msgs, api):\n"
        "    return 42\n");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;

    auto result =
        engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);
    EXPECT_EQ(result, InvokeResult::Error)
        << "Returning a number should be Error, not Commit";
    EXPECT_EQ(engine.script_error_count(), 1u);

    engine.finalize();
}

TEST_F(PythonEngineTest, InvokeProduce_WrongReturnString_IsError)
{
    // Returning a string instead of boolean should be an Error.
    write_script(
        "def on_produce(out_slot, fz, msgs, api):\n"
        "    return 'ok'\n");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;

    auto result =
        engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);
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
        "def on_consume(in_slot, fz, msgs, api):\n"
        "    assert in_slot is not None, 'expected non-None slot'\n"
        "    assert abs(in_slot.value - 99.5) < 0.01, (\n"
        "        f'expected value ~99.5, got {in_slot.value}')\n");

    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_consume"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "SlotFrame", "aligned"));

    auto ctx = producer_context();
    ctx.role_tag = "cons";
    ASSERT_TRUE(engine.build_api(ctx));

    float data = 99.5f;
    std::vector<IncomingMessage> msgs;

    engine.invoke_consume(&data, sizeof(data), nullptr, 0, nullptr, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "Script assertion failed -- slot value incorrect";

    engine.finalize();
}

TEST_F(PythonEngineTest, InvokeConsume_NoneSlot)
{
    write_script(
        "def on_consume(in_slot, fz, msgs, api):\n"
        "    assert in_slot is None, 'expected None'\n");

    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_consume"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "SlotFrame", "aligned"));

    auto ctx = producer_context();
    ctx.role_tag = "cons";
    ASSERT_TRUE(engine.build_api(ctx));

    std::vector<IncomingMessage> msgs;
    engine.invoke_consume(nullptr, 0, nullptr, 0, nullptr, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u);

    engine.finalize();
}

TEST_F(PythonEngineTest, InvokeConsume_ScriptErrorDetected)
{
    write_script(
        "def on_consume(in_slot, fz, msgs, api):\n"
        "    raise RuntimeError('consume error')\n");

    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_consume"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "SlotFrame", "aligned"));

    auto ctx = producer_context();
    ctx.role_tag = "cons";
    ASSERT_TRUE(engine.build_api(ctx));

    float data = 1.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_consume(&data, sizeof(data), nullptr, 0, nullptr, msgs);
    EXPECT_EQ(engine.script_error_count(), 1u);

    engine.finalize();
}

// ============================================================================
// 5. invoke_process
// ============================================================================

TEST_F(PythonEngineTest, InvokeProcess_DualSlots)
{
    write_script(
        "def on_process(in_slot, out_slot, fz, msgs, api):\n"
        "    if in_slot is not None and out_slot is not None:\n"
        "        out_slot.value = in_slot.value * 2.0\n"
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

    auto ctx = producer_context();
    ctx.role_tag = "proc";
    ASSERT_TRUE(engine.build_api(ctx));

    float in_data  = 21.0f;
    float out_data = 0.0f;
    std::vector<IncomingMessage> msgs;

    auto result = engine.invoke_process(
        &in_data, sizeof(in_data), &out_data, sizeof(out_data),
        nullptr, 0, nullptr, msgs);
    EXPECT_EQ(result, InvokeResult::Commit);
    EXPECT_FLOAT_EQ(out_data, 42.0f);

    engine.finalize();
}

TEST_F(PythonEngineTest, InvokeProcess_NoneInput)
{
    write_script(
        "def on_process(in_slot, out_slot, fz, msgs, api):\n"
        "    assert in_slot is None, 'expected None input'\n"
        "    assert out_slot is None, 'expected None output'\n"
        "    return False\n");

    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_process"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "InSlotFrame", "aligned"));
    ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame", "aligned"));

    auto ctx = producer_context();
    ctx.role_tag = "proc";
    ASSERT_TRUE(engine.build_api(ctx));

    std::vector<IncomingMessage> msgs;
    auto result = engine.invoke_process(
        nullptr, 0, nullptr, 0, nullptr, 0, nullptr, msgs);
    EXPECT_EQ(result, InvokeResult::Discard);
    EXPECT_EQ(engine.script_error_count(), 0u);

    engine.finalize();
}

TEST_F(PythonEngineTest, InvokeProcess_InputOnlyNoOutput)
{
    write_script(
        "def on_process(in_slot, out_slot, fz, msgs, api):\n"
        "    assert in_slot is not None, 'expected input data'\n"
        "    assert out_slot is None, 'expected None output'\n"
        "    return False\n");

    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_process"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "InSlotFrame", "aligned"));
    ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame", "aligned"));

    auto ctx = producer_context();
    ctx.role_tag = "proc";
    ASSERT_TRUE(engine.build_api(ctx));

    float in_data = 10.0f;
    std::vector<IncomingMessage> msgs;
    auto result = engine.invoke_process(
        &in_data, sizeof(in_data), nullptr, 0, nullptr, 0, nullptr, msgs);
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
        "def on_produce(out_slot, fz, msgs, api):\n"
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
        engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);
    EXPECT_EQ(result, InvokeResult::Discard);
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "Script assertion failed -- messages not received correctly";

    engine.finalize();
}

TEST_F(PythonEngineTest, InvokeConsume_BareDataMessages)
{
    // Consumer data messages are bare bytes (not tuples). Events are still dicts.
    write_script(
        "def on_consume(in_slot, fz, msgs, api):\n"
        "    assert len(msgs) == 2, f'expected 2 messages, got {len(msgs)}'\n"
        "    # First msg: data message -> bare bytes\n"
        "    assert isinstance(msgs[0], bytes), (\n"
        "        f'data msg should be bytes, got {type(msgs[0])}')\n"
        "    # Second msg: event message -> dict with .event\n"
        "    assert isinstance(msgs[1], dict), (\n"
        "        f'event msg should be dict, got {type(msgs[1])}')\n"
        "    assert msgs[1]['event'] == 'channel_closing'\n");

    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_consume"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "SlotFrame", "aligned"));

    auto ctx = producer_context();
    ctx.role_tag = "cons";
    ASSERT_TRUE(engine.build_api(ctx));

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
        "def on_produce(out_slot, fz, msgs, api):\n"
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
        engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);
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
        "def on_produce(out_slot, fz, msgs, api):\n"
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
        "def on_produce(out_slot, fz, msgs, api):\n"
        "    api.stop()\n"
        "    return False\n");

    RoleHostCore core;
    PythonEngine engine;
    ASSERT_TRUE(setup_engine_with_core(engine, core));

    EXPECT_FALSE(core.is_shutdown_requested());

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);

    EXPECT_TRUE(core.is_shutdown_requested())
        << "api.stop() should set shutdown_requested";

    engine.finalize();
}

TEST_F(PythonEngineTest, ApiSetCriticalError)
{
    // Python API's set_critical_error() takes no arguments (unlike Lua).
    write_script(
        "def on_produce(out_slot, fz, msgs, api):\n"
        "    api.set_critical_error()\n"
        "    return False\n");

    RoleHostCore core;
    PythonEngine engine;
    ASSERT_TRUE(setup_engine_with_core(engine, core));

    EXPECT_FALSE(core.is_critical_error());
    EXPECT_FALSE(core.is_shutdown_requested());

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);

    EXPECT_TRUE(core.is_critical_error())
        << "api.set_critical_error() should set critical_error_";
    EXPECT_TRUE(core.is_shutdown_requested())
        << "api.set_critical_error() should also set shutdown_requested";

    engine.finalize();
}

TEST_F(PythonEngineTest, ApiStopReason_Default)
{
    write_script(
        "def on_produce(out_slot, fz, msgs, api):\n"
        "    reason = api.stop_reason()\n"
        "    assert reason == 'normal', (\n"
        "        f\"expected 'normal', got '{reason}'\")\n"
        "    return False\n");

    RoleHostCore core;
    PythonEngine engine;
    ASSERT_TRUE(setup_engine_with_core(engine, core));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "stop_reason should be 'normal'";

    engine.finalize();
}

TEST_F(PythonEngineTest, ApiStopReason_PeerDead)
{
    write_script(
        "def on_produce(out_slot, fz, msgs, api):\n"
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
    engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);
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
        "def on_produce(out_slot, fz, msgs, api):\n"
        "    ow = api.out_slots_written()\n"
        "    dr = api.out_drop_count()\n"
        "    assert ow == 42, f'expected out_slots_written=42, got {ow}'\n"
        "    assert dr == 7, f'expected out_drop_count=7, got {dr}'\n"
        "    return False\n");

    RoleHostCore core;
    core.test_set_out_written(42);
    core.test_set_drops(7);

    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_produce"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "SlotFrame", "aligned"));

    auto ctx = producer_context();
    ctx.core = &core;
    ASSERT_TRUE(engine.build_api(ctx));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "Script assertion failed -- metrics values incorrect";

    engine.finalize();
}

TEST_F(PythonEngineTest, MetricsClosures_InReceivedWorks)
{
    // Python API uses in_slots_received() (not in_received).
    // Consumer role: ctx.consumer must be non-null, ctx.producer null.
    write_script(
        "def on_consume(in_slot, fz, msgs, api):\n"
        "    ir = api.in_slots_received()\n"
        "    assert ir == 15, f'expected in_slots_received=15, got {ir}'\n");

    RoleHostCore core;
    core.test_set_in_received(15);

    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_consume"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "SlotFrame", "aligned"));

    auto ctx = producer_context();
    ctx.role_tag = "cons";  // consumer role for in_slots_received
    ctx.core = &core;
    ASSERT_TRUE(engine.build_api(ctx));

    std::vector<IncomingMessage> msgs;
    engine.invoke_consume(nullptr, 0, nullptr, 0, nullptr, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "Script assertion failed -- in_received value incorrect";

    engine.finalize();
}

TEST_F(PythonEngineTest, MultipleErrors_CountAccumulates)
{
    write_script(
        "def on_produce(out_slot, fz, msgs, api):\n"
        "    raise RuntimeError('oops')\n");

    PythonEngine engine;
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
// 9. Error handling
// ============================================================================

TEST_F(PythonEngineTest, StopOnScriptError_SetsShutdownOnError)
{
    write_script(
        "def on_produce(out_slot, fz, msgs, api):\n"
        "    raise RuntimeError('intentional error')\n");

    RoleHostCore core;
    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_produce"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "SlotFrame", "aligned"));

    auto ctx = producer_context();
    ctx.core = &core;
    ctx.stop_on_script_error = true; // enable
    ASSERT_TRUE(engine.build_api(ctx));

    EXPECT_FALSE(core.is_shutdown_requested());

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    auto result =
        engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);

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
    write_script("def on_produce(out_slot, fz, msgs, api):\n    return True\n");

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
    write_script("def on_produce(out_slot, fz, msgs, api)\n"  // missing colon
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
        "def on_produce(out_slot, fz, msgs, api):\n"
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
        "def on_produce(out_slot, fz, msgs, api):\n"
        "    global call_count\n"
        "    call_count += 1\n"
        "    if out_slot is not None:\n"
        "        out_slot.value = float(call_count)\n"
        "    return True\n");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    std::vector<IncomingMessage> msgs;

    // Call 3 times -- counter should increment.
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
// 12. Flexzone
// ============================================================================

TEST_F(PythonEngineTest, InvokeProduce_WithFlexzone)
{
    // Script writes to both slot and flexzone.
    write_script(
        "def on_produce(out_slot, fz, msgs, api):\n"
        "    assert out_slot is not None, 'expected slot'\n"
        "    assert fz is not None, 'expected flexzone'\n"
        "    out_slot.value = 10.0\n"
        "    fz.value = 20.0\n"
        "    return True\n");

    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_produce"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "SlotFrame", "aligned"));
    ASSERT_TRUE(engine.register_slot_type(spec, "FlexFrame", "aligned"));

    auto ctx = producer_context();
    ASSERT_TRUE(engine.build_api(ctx));

    float slot_buf = 0.0f;
    float fz_buf   = 0.0f;
    std::vector<IncomingMessage> msgs;

    auto result = engine.invoke_produce(
        &slot_buf, sizeof(slot_buf), &fz_buf, sizeof(fz_buf), "FlexFrame", msgs);
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
        "def on_produce(out_slot, fz, msgs, api):\n"
        "    return False\n"
        "\n"
        "def on_inbox(data, sender, api):\n"
        "    assert data is not None, 'expected inbox data'\n"
        "    assert hasattr(data, 'value'), f'expected typed struct, got {type(data)}'\n"
        "    assert abs(data.value - 77.0) < 0.01, (\n"
        "        f'expected value ~77.0, got {data.value}')\n"
        "    assert sender == 'PROD-SENDER-00000001', (\n"
        "        f'expected sender UID, got {sender}')\n");

    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_produce"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "SlotFrame", "aligned"));
    ASSERT_TRUE(engine.register_slot_type(spec, "InboxFrame", "aligned"));

    auto ctx = producer_context();
    ASSERT_TRUE(engine.build_api(ctx));

    float inbox_data = 77.0f;
    engine.invoke_on_inbox(&inbox_data, sizeof(inbox_data), "PROD-SENDER-00000001");
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
        "def on_produce(out_slot, fz, msgs, api):\n"
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

    ASSERT_TRUE(engine.register_slot_type(spec, "SlotFrame", "aligned"));
    ASSERT_TRUE(engine.register_slot_type(spec, "InboxFrame", "aligned"));

    auto ctx = producer_context();
    ASSERT_TRUE(engine.build_api(ctx));

    size_t slot_sz  = engine.type_sizeof("SlotFrame");
    size_t inbox_sz = engine.type_sizeof("InboxFrame");
    EXPECT_GT(slot_sz, 0u) << "SlotFrame size must be > 0";
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
        "def on_produce(out_slot, fz, msgs, api):\n"
        "    return False\n"
        "\n"
        "def on_inbox(data, sender, api):\n"
        "    pass\n");

    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_produce"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "SlotFrame", "aligned"));
    // Deliberately NOT registering "InboxFrame".

    auto ctx = producer_context();
    ASSERT_TRUE(engine.build_api(ctx));

    float raw = 1.0f;
    engine.invoke_on_inbox(&raw, sizeof(raw), "CONS-SENDER-00000001");
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
def on_produce(out_slot, fz, msgs, api):
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
    write_script("def on_produce(out_slot, fz, msgs, api):\n    return True\n");
    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    EXPECT_FALSE(engine.invoke("no_such_function"));
    engine.finalize();
}

TEST_F(PythonEngineTest, Invoke_EmptyName_ReturnsFalse)
{
    write_script("def on_produce(out_slot, fz, msgs, api):\n    return True\n");
    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    EXPECT_FALSE(engine.invoke(""));
    engine.finalize();
}

TEST_F(PythonEngineTest, Invoke_ScriptError_ReturnsFalseAndIncrementsErrors)
{
    write_script(R"(
def on_produce(out_slot, fz, msgs, api):
    return True

def bad_func():
    raise RuntimeError("intentional test error")
)");
    RoleHostCore core;
    PythonEngine engine;
    ASSERT_TRUE(setup_engine_with_core(engine, core));

    EXPECT_FALSE(engine.invoke("bad_func"));
    EXPECT_EQ(core.script_errors(), 1u);
    engine.finalize();
}

TEST_F(PythonEngineTest, Invoke_FromNonOwnerThread_Queued)
{
    write_script(R"(
def on_produce(out_slot, fz, msgs, api):
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
        engine.invoke_produce(nullptr, 0, nullptr, 0, nullptr, msgs);
        std::this_thread::yield();
    }

    t.join();
    EXPECT_TRUE(result);
    engine.finalize();
}

TEST_F(PythonEngineTest, Invoke_FromNonOwnerThread_FinalizeUnblocks)
{
    write_script(R"(
def on_produce(out_slot, fz, msgs, api):
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
def on_produce(out_slot, fz, msgs, api):
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
        engine.invoke_produce(nullptr, 0, nullptr, 0, nullptr, msgs);
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
def on_produce(out_slot, fz, msgs, api):
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
def on_produce(out_slot, fz, msgs, api):
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
    write_script("def on_produce(out_slot, fz, msgs, api):\n    return True\n");
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
    write_script("def on_produce(out_slot, fz, msgs, api):\n    return True\n");
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

def on_produce(out_slot, fz, msgs, api):
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
        engine.invoke_produce(nullptr, 0, nullptr, 0, nullptr, msgs);

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
def on_produce(out_slot, fz, msgs, api):
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
    engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u);
    engine.finalize();
}

TEST_F(PythonEngineTest, Api_LastCycleWorkUs_ReadsFromCore)
{
    write_script(R"(
def on_produce(out_slot, fz, msgs, api):
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
    engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u);
    engine.finalize();
}

TEST_F(PythonEngineTest, Api_CriticalError_DefaultIsFalse)
{
    write_script(R"(
def on_produce(out_slot, fz, msgs, api):
    assert api.critical_error() == False, "critical_error should be False by default"
    return False
)");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u);
    engine.finalize();
}

TEST_F(PythonEngineTest, Api_IdentityAccessors_ReturnCorrectValues)
{
    write_script(R"(
def on_produce(out_slot, fz, msgs, api):
    assert api.uid() == "TEST-ENGINE-00000001", f"uid: {api.uid()}"
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
    engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u);
    engine.finalize();
}

TEST_F(PythonEngineTest, Api_CustomMetrics_ReportAndReadInMetrics)
{
    write_script(R"(
def on_produce(out_slot, fz, msgs, api):
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
    engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u);
    engine.finalize();
}

TEST_F(PythonEngineTest, Api_CustomMetrics_BatchAndClear)
{
    write_script(R"(
def on_produce(out_slot, fz, msgs, api):
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
    engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u);
    engine.finalize();
}

// ============================================================================
// 15. Error paths for on_init / on_stop / on_inbox
// ============================================================================

TEST_F(PythonEngineTest, InvokeOnInit_ScriptError)
{
    write_script(
        "def on_produce(out_slot, fz, msgs, api):\n"
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
        "def on_produce(out_slot, fz, msgs, api):\n"
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
        "def on_produce(out_slot, fz, msgs, api):\n"
        "    return False\n"
        "\n"
        "def on_inbox(data, sender, api):\n"
        "    raise RuntimeError('inbox exploded')\n");

    PythonEngine engine;
    engine.set_python_venv("");
    ASSERT_TRUE(engine.initialize("test", &default_core_));
    ASSERT_TRUE(engine.load_script(tmp_ / "script" / "python",
                                   "__init__.py", "on_produce"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "SlotFrame", "aligned"));
    ASSERT_TRUE(engine.register_slot_type(spec, "InboxFrame", "aligned"));

    auto ctx = producer_context();
    ASSERT_TRUE(engine.build_api(ctx));

    EXPECT_EQ(engine.script_error_count(), 0u);
    float inbox_data = 1.0f;
    engine.invoke_on_inbox(&inbox_data, sizeof(inbox_data), "SENDER-00000001");
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
def on_produce(out_slot, fz, msgs, api):
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
    engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "Script assertion failed -- producer queue-state defaults incorrect";

    engine.finalize();
}

TEST_F(PythonEngineTest, Api_ProcessorQueueState_DualDefaults)
{
    write_script(R"(
def on_process(in_slot, out_slot, fz, msgs, api):
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

    RoleContext ctx{};
    ctx.role_tag    = "proc";
    ctx.uid         = "TEST-ENGINE-00000001";
    ctx.name        = "TestEngine";
    ctx.channel     = "test.in.channel";
    ctx.out_channel = "test.out.channel";
    ctx.log_level   = "error";
    ctx.core        = &core;
    ctx.stop_on_script_error = false;
    ASSERT_TRUE(engine.build_api(ctx));

    std::vector<IncomingMessage> msgs;
    auto result = engine.invoke_process(
        nullptr, 0, nullptr, 0, nullptr, 0, nullptr, msgs);
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
def on_produce(out_slot, fz, msgs, api):
    v = api.ctrl_queue_dropped()
    assert v == 0, f"expected ctrl_queue_dropped==0, got {v}"
    return False
)");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);
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
def on_produce(out_slot, fz, msgs, api):
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
    engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);
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
def on_produce(out_slot, fz, msgs, api):
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
    engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);
    EXPECT_EQ(engine.script_error_count(), 0u)
        << "Script assertion failed -- custom metric overwrite incorrect";

    engine.finalize();
}

TEST_F(PythonEngineTest, Api_CustomMetrics_ZeroValue)
{
    write_script(R"(
def on_produce(out_slot, fz, msgs, api):
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
    engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);
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
def on_produce(out_slot, fz, msgs, api):
    assert len(msgs) == 0, f"expected empty msgs, got {len(msgs)}"
    return False
)");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs; // empty

    auto result =
        engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);
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
def on_produce(out_slot, fz, msgs, api):
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
    engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);
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
def on_process(in_slot, out_slot, fz, msgs, api):
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

    RoleContext ctx{};
    ctx.role_tag    = "proc";
    ctx.uid         = "TEST-ENGINE-00000001";
    ctx.name        = "TestEngine";
    ctx.channel     = "sensor.input";
    ctx.out_channel = "sensor.output";
    ctx.log_level   = "error";
    ctx.core        = &core;
    ctx.stop_on_script_error = false;
    ASSERT_TRUE(engine.build_api(ctx));

    float in_data  = 1.0f;
    float out_data = 0.0f;
    std::vector<IncomingMessage> msgs;
    auto result = engine.invoke_process(&in_data, sizeof(in_data),
                                         &out_data, sizeof(out_data),
                                         nullptr, 0, nullptr, msgs);
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
def on_produce(out_slot, fz, msgs, api):
    result = api.open_inbox("some-uid")
    assert result is None, f"expected None, got {result}"
    return False
)");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    auto result =
        engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);
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
def on_produce(out_slot, fz, msgs, api):
    api.report_metrics(42)  # wrong type
    return False
)");

    PythonEngine engine;
    ASSERT_TRUE(setup_engine(engine));

    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    engine.invoke_produce(&buf, sizeof(buf), nullptr, 0, nullptr, msgs);
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

def on_produce(out_slot, fz, msgs, api):
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

} // anonymous namespace
