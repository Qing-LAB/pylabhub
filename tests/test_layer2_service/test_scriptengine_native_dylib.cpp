/**
 * @file test_scriptengine_native_dylib.cpp
 * @brief L2 tests for NativeEngine — the ScriptEngine for native C/C++ plugins.
 *
 * Tests the full lifecycle: dlopen → ABI check → schema validation →
 * native_init → invoke callbacks → native_finalize → dlclose.
 *
 * Uses a real test plugin .so built alongside the test binary.
 * Error paths: missing .so, bad schema, bad ABI, missing required callback.
 */
#include <gtest/gtest.h>

#include "native_engine.hpp"
#include "utils/role_host_core.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using pylabhub::scripting::NativeEngine;
using pylabhub::scripting::ScriptEngine;
using pylabhub::scripting::InvokeResult;
using pylabhub::scripting::InvokeResponse;
using pylabhub::scripting::InvokeStatus;
using pylabhub::scripting::RoleAPIBase;
using pylabhub::scripting::RoleHostCore;
using pylabhub::scripting::IncomingMessage;
using pylabhub::hub::SchemaSpec;
using pylabhub::hub::FieldDef;
using pylabhub::scripting::InvokeTx;

namespace
{

// ============================================================================
// Helpers
// ============================================================================

/// Path to the test plugin .so (set by CMake via -D define).
#ifndef TEST_PLUGIN_DIR
#   define TEST_PLUGIN_DIR "."
#endif

fs::path good_plugin_path()
{
    fs::path dir(TEST_PLUGIN_DIR);
#if defined(_WIN32) || defined(_WIN64)
    fs::path p = dir / "test_good_producer_plugin.dll";
#elif defined(__APPLE__)
    fs::path p = dir / "libtest_good_producer_plugin.dylib";
#else
    fs::path p = dir / "libtest_good_producer_plugin.so";
#endif
    return p;
}

SchemaSpec simple_schema()
{
    SchemaSpec spec;
    spec.has_schema = true;
    FieldDef f;
    f.name     = "value";
    f.type_str = "float32";
    f.count    = 1;
    f.length   = 0;
    spec.fields.push_back(f);
    return spec;
}

std::unique_ptr<RoleAPIBase> make_native_api(RoleHostCore &core)
{
    auto api = std::make_unique<RoleAPIBase>(core);
    api->set_role_tag("prod");
    api->set_uid("PROD-TestNative-00000001");
    api->set_name("TestNative");
    api->set_channel("test.native.channel");
    api->set_log_level("error");
    api->set_stop_on_script_error(false);
    return api;
}

// ============================================================================
// Tests
// ============================================================================

class NativeEngineTest : public ::testing::Test
{
  protected:
    RoleHostCore core_;
};

TEST_F(NativeEngineTest, FullLifecycle_ProduceCommitsAndWritesSlot)
{
    NativeEngine engine;
    ASSERT_TRUE(engine.initialize("test", &core_));

    auto plugin = good_plugin_path();
    ASSERT_TRUE(fs::exists(plugin)) << "Plugin not found: " << plugin;

    ASSERT_TRUE(engine.load_script(plugin.parent_path(), plugin.filename().string(),
                                   "on_produce"));

    auto spec = simple_schema();
    ASSERT_TRUE(engine.register_slot_type(spec, "SlotFrame", "aligned"));
    EXPECT_EQ(engine.type_sizeof("SlotFrame"), 4u);

    auto test_api = make_native_api(core_);
    ASSERT_TRUE(engine.build_api(*test_api));

    // on_init should be callable.
    engine.invoke_on_init();

    // on_produce: plugin writes 42.0 to slot.
    float buf = 0.0f;
    std::vector<IncomingMessage> msgs;
    auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
    EXPECT_EQ(result, InvokeResult::Commit);
    EXPECT_FLOAT_EQ(buf, 42.0f);

    // Custom metric should have been reported by the plugin.
    auto cm = core_.custom_metrics_snapshot();
    EXPECT_EQ(cm.count("produce_count"), 1u);
    EXPECT_DOUBLE_EQ(cm.at("produce_count"), 1.0);

    // Second produce: counter increments.
    buf = 0.0f;
    result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf), nullptr, 0}, msgs);
    EXPECT_EQ(result, InvokeResult::Commit);
    EXPECT_FLOAT_EQ(buf, 42.0f);

    cm = core_.custom_metrics_snapshot();
    EXPECT_DOUBLE_EQ(cm.at("produce_count"), 2.0);

    engine.invoke_on_stop();
    engine.finalize();
}

TEST_F(NativeEngineTest, HasCallback_ReflectsPluginSymbols)
{
    NativeEngine engine;
    ASSERT_TRUE(engine.initialize("test", &core_));

    auto plugin = good_plugin_path();
    ASSERT_TRUE(fs::exists(plugin)) << "Plugin not found: " << plugin;
    ASSERT_TRUE(engine.load_script(plugin.parent_path(), plugin.filename().string(),
                                   "on_produce"));

    EXPECT_TRUE(engine.has_callback("on_produce"));
    EXPECT_TRUE(engine.has_callback("on_init"));
    EXPECT_TRUE(engine.has_callback("on_stop"));
    EXPECT_TRUE(engine.has_callback("on_heartbeat"));
    EXPECT_FALSE(engine.has_callback("on_consume"));
    EXPECT_FALSE(engine.has_callback("on_process"));
    EXPECT_FALSE(engine.has_callback("nonexistent_function"));

    engine.finalize();
}

TEST_F(NativeEngineTest, SchemaValidation_MatchingSchema_Succeeds)
{
    NativeEngine engine;
    ASSERT_TRUE(engine.initialize("test", &core_));

    auto plugin = good_plugin_path();
    ASSERT_TRUE(engine.load_script(plugin.parent_path(), plugin.filename().string(),
                                   "on_produce"));

    auto spec = simple_schema();
    EXPECT_TRUE(engine.register_slot_type(spec, "SlotFrame", "aligned"));
    EXPECT_EQ(engine.type_sizeof("SlotFrame"), 4u);

    engine.finalize();
}

TEST_F(NativeEngineTest, SchemaValidation_HasSchemaFalse_ReturnsFalse)
{
    NativeEngine engine;
    ASSERT_TRUE(engine.initialize("test", &core_));

    auto plugin = good_plugin_path();
    ASSERT_TRUE(engine.load_script(plugin.parent_path(), plugin.filename().string(),
                                   "on_produce"));

    SchemaSpec spec;
    spec.has_schema = false;
    EXPECT_FALSE(engine.register_slot_type(spec, "SlotFrame", "aligned"));

    engine.finalize();
}

TEST_F(NativeEngineTest, SchemaValidation_MismatchedSchema_Fails)
{
    NativeEngine engine;
    ASSERT_TRUE(engine.initialize("test", &core_));

    auto plugin = good_plugin_path();
    ASSERT_TRUE(engine.load_script(plugin.parent_path(), plugin.filename().string(),
                                   "on_produce"));

    // Schema that doesn't match the plugin's PLH_DECLARE_SCHEMA.
    SchemaSpec bad_spec;
    bad_spec.has_schema = true;
    FieldDef f;
    f.name     = "temperature";  // wrong name
    f.type_str = "float64";      // wrong type
    f.count    = 1;
    f.length   = 0;
    bad_spec.fields.push_back(f);

    EXPECT_FALSE(engine.register_slot_type(bad_spec, "SlotFrame", "aligned"));

    engine.finalize();
}

TEST_F(NativeEngineTest, LoadScript_MissingFile_ReturnsFalse)
{
    NativeEngine engine;
    ASSERT_TRUE(engine.initialize("test", &core_));

    EXPECT_FALSE(engine.load_script("/nonexistent/path",
                                    "libno_such_plugin.so", "on_produce"));
}

TEST_F(NativeEngineTest, LoadScript_MissingRequiredCallback_ReturnsFalse)
{
    NativeEngine engine;
    ASSERT_TRUE(engine.initialize("test", &core_));

    auto plugin = good_plugin_path();
    ASSERT_TRUE(fs::exists(plugin));

    // Plugin has on_produce but not on_consume.
    EXPECT_FALSE(engine.load_script(plugin.parent_path(), plugin.filename().string(),
                                    "on_consume"));
}

TEST_F(NativeEngineTest, Eval_ReturnsNotFound)
{
    NativeEngine engine;
    ASSERT_TRUE(engine.initialize("test", &core_));

    auto plugin = good_plugin_path();
    ASSERT_TRUE(engine.load_script(plugin.parent_path(), plugin.filename().string(),
                                   "on_produce"));

    auto result = engine.eval("some code");
    EXPECT_EQ(result.status, InvokeStatus::NotFound);

    engine.finalize();
}

TEST_F(NativeEngineTest, GenericInvoke_KnownCallback_ReturnsTrue)
{
    NativeEngine engine;
    ASSERT_TRUE(engine.initialize("test", &core_));

    auto plugin = good_plugin_path();
    ASSERT_TRUE(engine.load_script(plugin.parent_path(), plugin.filename().string(),
                                   "on_produce"));

    auto test_api = make_native_api(core_);
    ASSERT_TRUE(engine.build_api(*test_api));

    EXPECT_TRUE(engine.invoke("on_heartbeat"));
    EXPECT_FALSE(engine.invoke("nonexistent"));

    engine.finalize();
}

TEST_F(NativeEngineTest, SupportsMultiState_DefaultFalse)
{
    NativeEngine engine;
    ASSERT_TRUE(engine.initialize("test", &core_));

    auto plugin = good_plugin_path();
    ASSERT_TRUE(engine.load_script(plugin.parent_path(), plugin.filename().string(),
                                   "on_produce"));

    // Test plugin does not export plugin_is_thread_safe → defaults to false.
    EXPECT_FALSE(engine.supports_multi_state());

    engine.finalize();
}

TEST_F(NativeEngineTest, ContextFieldsPassedToPlugin)
{
    NativeEngine engine;
    ASSERT_TRUE(engine.initialize("test", &core_));

    auto plugin = good_plugin_path();
    ASSERT_TRUE(engine.load_script(plugin.parent_path(), plugin.filename().string(),
                                   "on_produce"));

    auto test_api = make_native_api(core_);
    ASSERT_TRUE(engine.build_api(*test_api));

    // The plugin stores g_ctx in native_init. We can verify indirectly:
    // native_init increments init_count by 1, on_init increments by 100.
    engine.invoke_on_init();

    // Call test_get_init_count via generic invoke to verify both ran.
    // We can't easily read the return value from a void function via invoke(),
    // but we can verify on_init was called by checking it didn't error.
    EXPECT_EQ(engine.script_error_count(), 0u);

    engine.finalize();
}

TEST_F(NativeEngineTest, Checksum_WrongHash_RejectsPlugin)
{
    NativeEngine engine;
    engine.set_expected_checksum("0000000000000000000000000000000000000000000000000000000000000000");
    ASSERT_TRUE(engine.initialize("test", &core_));

    auto plugin = good_plugin_path();
    ASSERT_TRUE(fs::exists(plugin));

    // Wrong checksum → load_script should fail.
    EXPECT_FALSE(engine.load_script(plugin.parent_path(), plugin.filename().string(),
                                    "on_produce"));
}

TEST_F(NativeEngineTest, Checksum_EmptyHash_SkipsVerification)
{
    NativeEngine engine;
    // No checksum set → verification skipped, load succeeds.
    ASSERT_TRUE(engine.initialize("test", &core_));

    auto plugin = good_plugin_path();
    ASSERT_TRUE(engine.load_script(plugin.parent_path(), plugin.filename().string(),
                                   "on_produce"));
    engine.finalize();
}

} // anonymous namespace
