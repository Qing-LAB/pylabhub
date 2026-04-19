/**
 * @file scriptengine_native_dylib_workers.cpp
 * @brief Pattern 3 worker bodies for the NativeEngine test suite.
 *
 * V2 conversion: each body fabricates a RoleAPIBase, which transitively
 * constructs a ThreadManager that registers a dynamic lifecycle module.
 * Runs only under run_gtest_worker with Logger owned by the subprocess.
 *
 * The plugin .so search directory is provided by the parent as argv[2]
 * (the value of TEST_PLUGIN_DIR at parent build time), so the subprocess
 * can locate the same .so artefacts the build staged.
 */
#include "scriptengine_native_dylib_workers.h"

#include "utils/engine_module_params.hpp"
#include "utils/native_engine.hpp"
#include "utils/role_api_base.hpp"
#include "utils/role_host_core.hpp"
#include "utils/schema_utils.hpp"

#include "plh_service.hpp"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"
#include "test_schema_helpers.h"

#include <fmt/core.h>
#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using pylabhub::hub::FieldDef;
using pylabhub::hub::SchemaSpec;
using pylabhub::scripting::IncomingMessage;
using pylabhub::scripting::InvokeResult;
using pylabhub::scripting::InvokeRx;
using pylabhub::scripting::InvokeStatus;
using pylabhub::scripting::InvokeTx;
using pylabhub::scripting::NativeEngine;
using pylabhub::scripting::RoleAPIBase;
using pylabhub::scripting::RoleHostCore;
using pylabhub::tests::helper::run_gtest_worker;
using pylabhub::utils::Logger;

namespace pylabhub::tests::worker
{
namespace native_engine
{
namespace
{

fs::path module_path(const std::string &plugin_dir, const char *base_name)
{
    fs::path dir(plugin_dir);
#if defined(_WIN32) || defined(_WIN64)
    return dir / (std::string(base_name) + ".dll");
#elif defined(__APPLE__)
    return dir / ("lib" + std::string(base_name) + ".dylib");
#else
    return dir / ("lib" + std::string(base_name) + ".so");
#endif
}

fs::path good_plugin_path(const std::string &plugin_dir)
{
    return module_path(plugin_dir, "test_good_producer_plugin");
}
fs::path multifield_module_path(const std::string &plugin_dir)
{
    return module_path(plugin_dir, "test_native_multifield_module");
}

struct MultiFieldSlot
{
    double  ts;
    uint8_t flag;
    int32_t count;
    float   values[3];
    uint8_t tag[8];
};
static_assert(sizeof(MultiFieldSlot) == 40,
              "Must match native module layout");

std::unique_ptr<RoleAPIBase> make_native_api(RoleHostCore &core)
{
    auto api = std::make_unique<RoleAPIBase>(core, "prod",
                                             "PROD-TestNative-00000001");
    api->set_name("TestNative");
    api->set_channel("test.native.channel");
    api->set_log_level("error");
    api->set_stop_on_script_error(false);
    return api;
}

/// Wrapper: worker body + the three standard mods (Logger only — the
/// engine path itself doesn't need FileLock / JsonConfig).
template <typename Fn>
int run_ne_worker(Fn body, const char *name)
{
    return run_gtest_worker(std::move(body), name, Logger::GetLifecycleModule());
}

} // namespace

// ── Core lifecycle ──────────────────────────────────────────────────────────

int full_lifecycle_produce_commits_and_writes_slot(const std::string &plugin_dir)
{
    return run_ne_worker(
        [&]() {
            RoleHostCore core;
            NativeEngine engine;
            ASSERT_TRUE(engine.initialize("test", &core));

            auto plugin = good_plugin_path(plugin_dir);
            ASSERT_TRUE(fs::exists(plugin)) << "Plugin not found: " << plugin;

            ASSERT_TRUE(engine.load_script(plugin.parent_path(),
                                           plugin.filename().string(),
                                           "on_produce"));

            auto spec = pylabhub::tests::simple_schema();
            ASSERT_TRUE(engine.register_slot_type(spec, "SlotFrame", "aligned"));
            EXPECT_EQ(engine.type_sizeof("SlotFrame"), 4u);

            auto test_api = make_native_api(core);
            ASSERT_TRUE(engine.build_api(*test_api));

            engine.invoke_on_init();

            float buf = 0.0f;
            std::vector<IncomingMessage> msgs;
            auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(result, InvokeResult::Commit);
            EXPECT_FLOAT_EQ(buf, 42.0f);

            auto cm = core.custom_metrics_snapshot();
            EXPECT_EQ(cm.count("produce_count"), 1u);
            EXPECT_DOUBLE_EQ(cm.at("produce_count"), 1.0);

            buf = 0.0f;
            result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(result, InvokeResult::Commit);
            EXPECT_FLOAT_EQ(buf, 42.0f);

            cm = core.custom_metrics_snapshot();
            EXPECT_DOUBLE_EQ(cm.at("produce_count"), 2.0);

            engine.invoke_on_stop();
            engine.finalize();
        },
        "native_engine::full_lifecycle_produce_commits_and_writes_slot");
}

int has_callback_reflects_plugin_symbols(const std::string &plugin_dir)
{
    return run_ne_worker(
        [&]() {
            RoleHostCore core;
            NativeEngine engine;
            ASSERT_TRUE(engine.initialize("test", &core));

            auto plugin = good_plugin_path(plugin_dir);
            ASSERT_TRUE(fs::exists(plugin));
            ASSERT_TRUE(engine.load_script(plugin.parent_path(),
                                           plugin.filename().string(),
                                           "on_produce"));

            EXPECT_TRUE(engine.has_callback("on_produce"));
            EXPECT_TRUE(engine.has_callback("on_consume"));
            EXPECT_TRUE(engine.has_callback("on_process"));
            EXPECT_TRUE(engine.has_callback("on_init"));
            EXPECT_TRUE(engine.has_callback("on_stop"));
            EXPECT_TRUE(engine.has_callback("on_heartbeat"));
            EXPECT_FALSE(engine.has_callback("nonexistent_function"));

            engine.finalize();
        },
        "native_engine::has_callback_reflects_plugin_symbols");
}

int schema_validation_matching_schema_succeeds(const std::string &plugin_dir)
{
    return run_ne_worker(
        [&]() {
            RoleHostCore core;
            NativeEngine engine;
            ASSERT_TRUE(engine.initialize("test", &core));

            auto plugin = good_plugin_path(plugin_dir);
            ASSERT_TRUE(engine.load_script(plugin.parent_path(),
                                           plugin.filename().string(),
                                           "on_produce"));
            auto spec = pylabhub::tests::simple_schema();
            EXPECT_TRUE(engine.register_slot_type(spec, "SlotFrame", "aligned"));
            EXPECT_EQ(engine.type_sizeof("SlotFrame"), 4u);
            engine.finalize();
        },
        "native_engine::schema_validation_matching_schema_succeeds");
}

int schema_validation_has_schema_false_returns_false(const std::string &plugin_dir)
{
    return run_ne_worker(
        [&]() {
            RoleHostCore core;
            NativeEngine engine;
            ASSERT_TRUE(engine.initialize("test", &core));

            auto plugin = good_plugin_path(plugin_dir);
            ASSERT_TRUE(engine.load_script(plugin.parent_path(),
                                           plugin.filename().string(),
                                           "on_produce"));
            SchemaSpec spec;
            spec.has_schema = false;
            EXPECT_FALSE(engine.register_slot_type(spec, "SlotFrame", "aligned"));
            engine.finalize();
        },
        "native_engine::schema_validation_has_schema_false_returns_false");
}

int schema_validation_mismatched_schema_fails(const std::string &plugin_dir)
{
    return run_ne_worker(
        [&]() {
            RoleHostCore core;
            NativeEngine engine;
            ASSERT_TRUE(engine.initialize("test", &core));

            auto plugin = good_plugin_path(plugin_dir);
            ASSERT_TRUE(engine.load_script(plugin.parent_path(),
                                           plugin.filename().string(),
                                           "on_produce"));

            SchemaSpec bad_spec;
            bad_spec.has_schema = true;
            FieldDef f;
            f.name     = "temperature";
            f.type_str = "float64";
            f.count    = 1;
            f.length   = 0;
            bad_spec.fields.push_back(f);
            EXPECT_FALSE(engine.register_slot_type(bad_spec, "SlotFrame", "aligned"));
            engine.finalize();
        },
        "native_engine::schema_validation_mismatched_schema_fails");
}

int load_script_missing_file_returns_false()
{
    return run_ne_worker(
        [&]() {
            RoleHostCore core;
            NativeEngine engine;
            ASSERT_TRUE(engine.initialize("test", &core));
            EXPECT_FALSE(engine.load_script("/nonexistent/path",
                                            "libno_such_plugin.so",
                                            "on_produce"));
        },
        "native_engine::load_script_missing_file_returns_false");
}

int load_script_missing_required_callback_returns_false(const std::string &plugin_dir)
{
    return run_ne_worker(
        [&]() {
            RoleHostCore core;
            NativeEngine engine;
            ASSERT_TRUE(engine.initialize("test", &core));

            auto plugin = good_plugin_path(plugin_dir);
            ASSERT_TRUE(fs::exists(plugin));
            EXPECT_FALSE(engine.load_script(plugin.parent_path(),
                                            plugin.filename().string(),
                                            "on_tick"));
        },
        "native_engine::load_script_missing_required_callback_returns_false");
}

int eval_returns_not_found(const std::string &plugin_dir)
{
    return run_ne_worker(
        [&]() {
            RoleHostCore core;
            NativeEngine engine;
            ASSERT_TRUE(engine.initialize("test", &core));

            auto plugin = good_plugin_path(plugin_dir);
            ASSERT_TRUE(engine.load_script(plugin.parent_path(),
                                           plugin.filename().string(),
                                           "on_produce"));
            auto result = engine.eval("some code");
            EXPECT_EQ(result.status, InvokeStatus::NotFound);
            engine.finalize();
        },
        "native_engine::eval_returns_not_found");
}

int generic_invoke_known_callback_returns_true(const std::string &plugin_dir)
{
    return run_ne_worker(
        [&]() {
            RoleHostCore core;
            NativeEngine engine;
            ASSERT_TRUE(engine.initialize("test", &core));

            auto plugin = good_plugin_path(plugin_dir);
            ASSERT_TRUE(engine.load_script(plugin.parent_path(),
                                           plugin.filename().string(),
                                           "on_produce"));

            auto test_api = make_native_api(core);
            ASSERT_TRUE(engine.build_api(*test_api));

            EXPECT_TRUE(engine.invoke("on_heartbeat"));
            EXPECT_FALSE(engine.invoke("nonexistent"));
            engine.finalize();
        },
        "native_engine::generic_invoke_known_callback_returns_true");
}

int supports_multi_state_default_false(const std::string &plugin_dir)
{
    return run_ne_worker(
        [&]() {
            RoleHostCore core;
            NativeEngine engine;
            ASSERT_TRUE(engine.initialize("test", &core));

            auto plugin = good_plugin_path(plugin_dir);
            ASSERT_TRUE(engine.load_script(plugin.parent_path(),
                                           plugin.filename().string(),
                                           "on_produce"));
            EXPECT_FALSE(engine.supports_multi_state());
            engine.finalize();
        },
        "native_engine::supports_multi_state_default_false");
}

int context_fields_passed_to_plugin(const std::string &plugin_dir)
{
    return run_ne_worker(
        [&]() {
            RoleHostCore core;
            NativeEngine engine;
            ASSERT_TRUE(engine.initialize("test", &core));

            auto plugin = good_plugin_path(plugin_dir);
            ASSERT_TRUE(engine.load_script(plugin.parent_path(),
                                           plugin.filename().string(),
                                           "on_produce"));

            auto test_api = make_native_api(core);
            ASSERT_TRUE(engine.build_api(*test_api));

            engine.invoke_on_init();
            EXPECT_EQ(engine.script_error_count(), 0u);

            engine.finalize();
        },
        "native_engine::context_fields_passed_to_plugin");
}

int checksum_wrong_hash_rejects_plugin(const std::string &plugin_dir)
{
    return run_ne_worker(
        [&]() {
            RoleHostCore core;
            NativeEngine engine;
            engine.set_expected_checksum(
                "0000000000000000000000000000000000000000000000000000000000000000");
            ASSERT_TRUE(engine.initialize("test", &core));

            auto plugin = good_plugin_path(plugin_dir);
            ASSERT_TRUE(fs::exists(plugin));
            EXPECT_FALSE(engine.load_script(plugin.parent_path(),
                                            plugin.filename().string(),
                                            "on_produce"));
        },
        "native_engine::checksum_wrong_hash_rejects_plugin");
}

int checksum_empty_hash_skips_verification(const std::string &plugin_dir)
{
    return run_ne_worker(
        [&]() {
            RoleHostCore core;
            NativeEngine engine;
            ASSERT_TRUE(engine.initialize("test", &core));

            auto plugin = good_plugin_path(plugin_dir);
            ASSERT_TRUE(engine.load_script(plugin.parent_path(),
                                           plugin.filename().string(),
                                           "on_produce"));
            engine.finalize();
        },
        "native_engine::checksum_empty_hash_skips_verification");
}

int api_counters_and_schema_size_through_native_module(const std::string &plugin_dir)
{
    return run_ne_worker(
        [&]() {
            RoleHostCore core;
            NativeEngine engine;
            ASSERT_TRUE(engine.initialize("test", &core));

            auto lib = good_plugin_path(plugin_dir);
            ASSERT_TRUE(engine.load_script(lib.parent_path(),
                                           lib.filename().string(),
                                           "on_produce"));
            auto spec = pylabhub::tests::simple_schema();
            ASSERT_TRUE(engine.register_slot_type(spec, "SlotFrame", "aligned"));

            core.inc_out_slots_written();
            core.inc_out_slots_written();
            core.inc_out_slots_written();
            core.inc_in_slots_received();
            core.inc_out_drop_count();
            core.inc_script_error_count();

            core.set_out_slot_spec(SchemaSpec{spec},
                                   pylabhub::hub::compute_schema_size(spec, "aligned"));

            auto test_api = make_native_api(core);
            ASSERT_TRUE(engine.build_api(*test_api));

            float buf = 0.0f;
            std::vector<IncomingMessage> msgs;
            auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(result, InvokeResult::Commit);
            EXPECT_FLOAT_EQ(buf, 42.0f);

            auto metrics = core.custom_metrics_snapshot();
            EXPECT_EQ(static_cast<uint64_t>(metrics["test_out_slots_written"]), 3u);
            EXPECT_EQ(static_cast<size_t>(metrics["test_slot_logical_size"]), 4u);
            EXPECT_EQ(static_cast<uint32_t>(metrics["test_spinlock_count"]), 0u);
            EXPECT_EQ(static_cast<int>(metrics["test_cpp_wrapper_ok"]), 1);

            engine.finalize();
        },
        "native_engine::api_counters_and_schema_size_through_native_module");
}

// ── FullStartup — simple schema ─────────────────────────────────────────────

int full_startup_producer_slot_only(const std::string &plugin_dir)
{
    return run_ne_worker(
        [&]() {
            NativeEngine engine;
            RoleHostCore core;

            auto api = make_native_api(core);
            auto spec = pylabhub::tests::simple_schema();
            core.set_out_slot_spec(SchemaSpec{spec},
                                   pylabhub::hub::compute_schema_size(spec, "aligned"));

            pylabhub::scripting::EngineModuleParams params;
            params.engine            = &engine;
            params.api               = api.get();
            params.tag               = "prod";
            params.script_dir        = good_plugin_path(plugin_dir).parent_path();
            params.entry_point       = good_plugin_path(plugin_dir).filename().string();
            params.required_callback = "on_produce";
            params.out_slot_spec     = spec;
            params.out_packing       = "aligned";

            ASSERT_NO_THROW(pylabhub::scripting::engine_lifecycle_startup(nullptr, &params));
            EXPECT_GT(engine.type_sizeof("OutSlotFrame"), 0u);

            float buf = 0.0f;
            std::vector<IncomingMessage> msgs;
            auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(result, InvokeResult::Commit);
            EXPECT_FLOAT_EQ(buf, 42.0f);

            pylabhub::scripting::engine_lifecycle_shutdown(nullptr, &params);
        },
        "native_engine::full_startup_producer_slot_only");
}

int full_startup_consumer(const std::string &plugin_dir)
{
    return run_ne_worker(
        [&]() {
            NativeEngine engine;
            RoleHostCore core;

            auto api = std::make_unique<RoleAPIBase>(core, "cons",
                                                     "CONS-TestNative-00000001");
            api->set_name("TestNativeConsumer");
            api->set_channel("test.native.channel");
            api->set_log_level("error");

            auto spec = pylabhub::tests::simple_schema();
            core.set_in_slot_spec(SchemaSpec{spec},
                                  pylabhub::hub::compute_schema_size(spec, "aligned"));

            pylabhub::scripting::EngineModuleParams params;
            params.engine            = &engine;
            params.api               = api.get();
            params.tag               = "cons";
            params.script_dir        = good_plugin_path(plugin_dir).parent_path();
            params.entry_point       = good_plugin_path(plugin_dir).filename().string();
            params.required_callback = "on_consume";
            params.in_slot_spec      = spec;
            params.in_packing        = "aligned";

            ASSERT_NO_THROW(pylabhub::scripting::engine_lifecycle_startup(nullptr, &params));
            EXPECT_GT(engine.type_sizeof("InSlotFrame"), 0u);

            float data = 42.0f;
            std::vector<IncomingMessage> msgs;
            auto result = engine.invoke_consume(InvokeRx{&data, sizeof(data)}, msgs);
            EXPECT_EQ(result, InvokeResult::Commit);
            EXPECT_EQ(engine.script_error_count(), 0u);

            pylabhub::scripting::engine_lifecycle_shutdown(nullptr, &params);
        },
        "native_engine::full_startup_consumer");
}

int full_startup_processor(const std::string &plugin_dir)
{
    return run_ne_worker(
        [&]() {
            NativeEngine engine;
            RoleHostCore core;

            auto api = std::make_unique<RoleAPIBase>(core, "proc",
                                                     "PROC-TestNative-00000001");
            api->set_name("TestNativeProcessor");
            api->set_channel("test.native.in");
            api->set_out_channel("test.native.out");
            api->set_log_level("error");

            auto spec = pylabhub::tests::simple_schema();
            core.set_in_slot_spec(SchemaSpec{spec},
                                  pylabhub::hub::compute_schema_size(spec, "aligned"));
            core.set_out_slot_spec(SchemaSpec{spec},
                                   pylabhub::hub::compute_schema_size(spec, "aligned"));

            pylabhub::scripting::EngineModuleParams params;
            params.engine            = &engine;
            params.api               = api.get();
            params.tag               = "proc";
            params.script_dir        = good_plugin_path(plugin_dir).parent_path();
            params.entry_point       = good_plugin_path(plugin_dir).filename().string();
            params.required_callback = "on_process";
            params.in_slot_spec      = spec;
            params.out_slot_spec     = spec;
            params.in_packing        = "aligned";
            params.out_packing       = "aligned";

            ASSERT_NO_THROW(pylabhub::scripting::engine_lifecycle_startup(nullptr, &params));
            EXPECT_GT(engine.type_sizeof("InSlotFrame"), 0u);
            EXPECT_GT(engine.type_sizeof("OutSlotFrame"), 0u);
            EXPECT_EQ(engine.type_sizeof("SlotFrame"), 0u);

            float in_data = 5.0f, out_data = 0.0f;
            std::vector<IncomingMessage> msgs;
            auto result = engine.invoke_process(InvokeRx{&in_data, sizeof(in_data)},
                                                InvokeTx{&out_data, sizeof(out_data)},
                                                msgs);
            EXPECT_EQ(result, InvokeResult::Commit);
            EXPECT_FLOAT_EQ(out_data, 10.0f);

            pylabhub::scripting::engine_lifecycle_shutdown(nullptr, &params);
        },
        "native_engine::full_startup_processor");
}

// ── FullStartup — multifield ────────────────────────────────────────────────

int full_startup_producer_multifield(const std::string &plugin_dir)
{
    return run_ne_worker(
        [&]() {
            NativeEngine engine;
            RoleHostCore core;
            auto api = make_native_api(core);

            auto spec = pylabhub::tests::multifield_schema();
            core.set_out_slot_spec(SchemaSpec{spec},
                                   pylabhub::hub::compute_schema_size(spec, "aligned"));

            pylabhub::scripting::EngineModuleParams params;
            params.engine            = &engine;
            params.api               = api.get();
            params.tag               = "prod";
            params.script_dir        = multifield_module_path(plugin_dir).parent_path();
            params.entry_point       = multifield_module_path(plugin_dir).filename().string();
            params.required_callback = "on_produce";
            params.out_slot_spec     = spec;
            params.out_packing       = "aligned";

            ASSERT_NO_THROW(pylabhub::scripting::engine_lifecycle_startup(nullptr, &params));
            EXPECT_EQ(engine.type_sizeof("OutSlotFrame"), sizeof(MultiFieldSlot));

            MultiFieldSlot slot{};
            std::vector<IncomingMessage> msgs;
            auto result = engine.invoke_produce(InvokeTx{&slot, sizeof(slot)}, msgs);
            EXPECT_EQ(result, InvokeResult::Commit);

            EXPECT_DOUBLE_EQ(slot.ts, 1.23456789);
            EXPECT_EQ(slot.flag, 0xAB);
            EXPECT_EQ(slot.count, -42);
            EXPECT_FLOAT_EQ(slot.values[0], 1.0f);
            EXPECT_FLOAT_EQ(slot.values[1], 2.5f);
            EXPECT_FLOAT_EQ(slot.values[2], -3.75f);
            EXPECT_EQ(std::memcmp(slot.tag, "DEADBEEF", 8), 0);

            pylabhub::scripting::engine_lifecycle_shutdown(nullptr, &params);
        },
        "native_engine::full_startup_producer_multifield");
}

int full_startup_consumer_multifield(const std::string &plugin_dir)
{
    return run_ne_worker(
        [&]() {
            NativeEngine engine;
            RoleHostCore core;

            auto api = std::make_unique<RoleAPIBase>(core, "cons",
                                                     "CONS-TestNative-00000001");
            api->set_name("TestNativeConsumer");
            api->set_channel("test.native.channel");
            api->set_log_level("error");

            auto spec = pylabhub::tests::multifield_schema();
            core.set_in_slot_spec(SchemaSpec{spec},
                                  pylabhub::hub::compute_schema_size(spec, "aligned"));

            pylabhub::scripting::EngineModuleParams params;
            params.engine            = &engine;
            params.api               = api.get();
            params.tag               = "cons";
            params.script_dir        = multifield_module_path(plugin_dir).parent_path();
            params.entry_point       = multifield_module_path(plugin_dir).filename().string();
            params.required_callback = "on_consume";
            params.in_slot_spec      = spec;
            params.in_packing        = "aligned";

            ASSERT_NO_THROW(pylabhub::scripting::engine_lifecycle_startup(nullptr, &params));
            EXPECT_EQ(engine.type_sizeof("InSlotFrame"), sizeof(MultiFieldSlot));

            MultiFieldSlot slot{};
            slot.ts = 9.87; slot.flag = 0xCD; slot.count = 100;
            slot.values[0] = 10.0f; slot.values[1] = 20.0f; slot.values[2] = 30.0f;
            std::memcpy(slot.tag, "ABCDEFGH", 8);

            std::vector<IncomingMessage> msgs;
            auto result = engine.invoke_consume(InvokeRx{&slot, sizeof(slot)}, msgs);
            EXPECT_EQ(result, InvokeResult::Commit);
            EXPECT_EQ(engine.script_error_count(), 0u);

            pylabhub::scripting::engine_lifecycle_shutdown(nullptr, &params);
        },
        "native_engine::full_startup_consumer_multifield");
}

int full_startup_processor_multifield(const std::string &plugin_dir)
{
    return run_ne_worker(
        [&]() {
            NativeEngine engine;
            RoleHostCore core;

            auto api = std::make_unique<RoleAPIBase>(core, "proc",
                                                     "PROC-TestNative-00000001");
            api->set_name("TestNativeProcessor");
            api->set_channel("test.native.in");
            api->set_out_channel("test.native.out");
            api->set_log_level("error");

            auto spec = pylabhub::tests::multifield_schema();
            size_t sz = pylabhub::hub::compute_schema_size(spec, "aligned");
            core.set_in_slot_spec(SchemaSpec{spec}, sz);
            core.set_out_slot_spec(SchemaSpec{spec}, sz);

            pylabhub::scripting::EngineModuleParams params;
            params.engine            = &engine;
            params.api               = api.get();
            params.tag               = "proc";
            params.script_dir        = multifield_module_path(plugin_dir).parent_path();
            params.entry_point       = multifield_module_path(plugin_dir).filename().string();
            params.required_callback = "on_process";
            params.in_slot_spec      = spec;
            params.out_slot_spec     = spec;
            params.in_packing        = "aligned";
            params.out_packing       = "aligned";

            ASSERT_NO_THROW(pylabhub::scripting::engine_lifecycle_startup(nullptr, &params));
            EXPECT_GT(engine.type_sizeof("InSlotFrame"), 0u);
            EXPECT_GT(engine.type_sizeof("OutSlotFrame"), 0u);

            MultiFieldSlot in_slot{};
            in_slot.ts = 1.23456789; in_slot.flag = 0xAB; in_slot.count = -42;
            in_slot.values[0] = 1.0f; in_slot.values[1] = 2.5f; in_slot.values[2] = -3.75f;
            std::memcpy(in_slot.tag, "DEADBEEF", 8);

            MultiFieldSlot out_slot{};
            std::vector<IncomingMessage> msgs;
            auto result = engine.invoke_process(InvokeRx{&in_slot, sizeof(in_slot)},
                                                InvokeTx{&out_slot, sizeof(out_slot)},
                                                msgs);
            EXPECT_EQ(result, InvokeResult::Commit);
            EXPECT_DOUBLE_EQ(out_slot.ts, 1.23456789);
            EXPECT_EQ(out_slot.flag, 0xAB);
            EXPECT_EQ(out_slot.count, -84);
            EXPECT_FLOAT_EQ(out_slot.values[0], 1.0f);
            EXPECT_FLOAT_EQ(out_slot.values[1], 2.5f);
            EXPECT_FLOAT_EQ(out_slot.values[2], -3.75f);
            EXPECT_EQ(std::memcmp(out_slot.tag, "DEADBEEF", 8), 0);

            pylabhub::scripting::engine_lifecycle_shutdown(nullptr, &params);
        },
        "native_engine::full_startup_processor_multifield");
}

int full_startup_producer_slot_and_flexzone(const std::string &plugin_dir)
{
    return run_ne_worker(
        [&]() {
            NativeEngine engine;
            RoleHostCore core;
            auto api = make_native_api(core);

            auto spec = pylabhub::tests::simple_schema();
            core.set_out_slot_spec(SchemaSpec{spec},
                                   pylabhub::hub::compute_schema_size(spec, "aligned"));
            core.set_out_fz_spec(SchemaSpec{spec},
                                 pylabhub::hub::align_to_physical_page(
                                     pylabhub::hub::compute_schema_size(spec, "aligned")));

            pylabhub::scripting::EngineModuleParams params;
            params.engine            = &engine;
            params.api               = api.get();
            params.tag               = "prod";
            params.script_dir        = good_plugin_path(plugin_dir).parent_path();
            params.entry_point       = good_plugin_path(plugin_dir).filename().string();
            params.required_callback = "on_produce";
            params.out_slot_spec     = spec;
            params.out_fz_spec       = spec;
            params.out_packing       = "aligned";

            ASSERT_NO_THROW(pylabhub::scripting::engine_lifecycle_startup(nullptr, &params));
            EXPECT_GT(engine.type_sizeof("OutSlotFrame"), 0u);
            EXPECT_GT(engine.type_sizeof("OutFlexFrame"), 0u);
            EXPECT_TRUE(core.has_tx_fz());

            float slot_buf = 0.0f;
            std::vector<IncomingMessage> msgs;
            auto result = engine.invoke_produce(InvokeTx{&slot_buf, sizeof(slot_buf)},
                                                msgs);
            EXPECT_EQ(result, InvokeResult::Commit);
            EXPECT_FLOAT_EQ(slot_buf, 42.0f);

            pylabhub::scripting::engine_lifecycle_shutdown(nullptr, &params);
        },
        "native_engine::full_startup_producer_slot_and_flexzone");
}

int invoke_on_inbox_typed_data(const std::string &plugin_dir)
{
    return run_ne_worker(
        [&]() {
            RoleHostCore core;
            NativeEngine engine;
            ASSERT_TRUE(engine.initialize("test", &core));

            auto lib = good_plugin_path(plugin_dir);
            ASSERT_TRUE(engine.load_script(lib.parent_path(),
                                           lib.filename().string(),
                                           "on_produce"));

            auto spec = pylabhub::tests::simple_schema();
            ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame", "aligned"));
            ASSERT_TRUE(engine.register_slot_type(spec, "InboxFrame", "aligned"));

            auto test_api = make_native_api(core);
            ASSERT_TRUE(engine.build_api(*test_api));

            float inbox_data = 77.0f;
            engine.invoke_on_inbox(
                {&inbox_data, sizeof(inbox_data), "PROD-SENDER-00000001", 1});
            EXPECT_EQ(engine.script_error_count(), 0u);

            engine.finalize();
        },
        "native_engine::invoke_on_inbox_typed_data");
}

int api_band_pub_sub_no_broker_graceful_return(const std::string &plugin_dir)
{
    return run_ne_worker(
        [&]() {
            RoleHostCore core;
            NativeEngine engine;
            ASSERT_TRUE(engine.initialize("test", &core));

            auto lib = good_plugin_path(plugin_dir);
            ASSERT_TRUE(engine.load_script(lib.parent_path(),
                                           lib.filename().string(),
                                           "on_produce"));
            auto spec = pylabhub::tests::simple_schema();
            ASSERT_TRUE(engine.register_slot_type(spec, "SlotFrame", "aligned"));
            core.set_out_slot_spec(SchemaSpec{spec},
                                   pylabhub::hub::compute_schema_size(spec, "aligned"));

            auto test_api = make_native_api(core);
            ASSERT_TRUE(engine.build_api(*test_api));

            float buf = 0.0f;
            std::vector<IncomingMessage> msgs;
            auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(result, InvokeResult::Commit);

            auto metrics = core.custom_metrics_snapshot();
            EXPECT_EQ(static_cast<int>(metrics["test_band_join_null"]), 1);
            EXPECT_EQ(static_cast<int>(metrics["test_band_leave_zero"]), 1);
            EXPECT_EQ(static_cast<int>(metrics["test_band_send_ok"]), 1);
            EXPECT_EQ(static_cast<int>(metrics["test_band_members_null"]), 1);

            engine.finalize();
        },
        "native_engine::api_band_pub_sub_no_broker_graceful_return");
}

} // namespace native_engine
} // namespace pylabhub::tests::worker

// ── Dispatcher ──────────────────────────────────────────────────────────────

namespace
{

struct NativeEngineWorkerRegistrar
{
    NativeEngineWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "native_engine")
                    return -1;
                std::string sc(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::native_engine;

                // load_script_missing_file takes no plugin_dir arg.
                if (sc == "load_script_missing_file_returns_false")
                    return load_script_missing_file_returns_false();

                // All other scenarios require plugin_dir as argv[2].
                if (argc <= 2) {
                    fmt::print(stderr,
                               "native_engine.{}: missing plugin_dir arg\n", sc);
                    return 1;
                }
                const std::string pdir = argv[2];

                if (sc == "full_lifecycle_produce_commits_and_writes_slot")
                    return full_lifecycle_produce_commits_and_writes_slot(pdir);
                if (sc == "has_callback_reflects_plugin_symbols")
                    return has_callback_reflects_plugin_symbols(pdir);
                if (sc == "schema_validation_matching_schema_succeeds")
                    return schema_validation_matching_schema_succeeds(pdir);
                if (sc == "schema_validation_has_schema_false_returns_false")
                    return schema_validation_has_schema_false_returns_false(pdir);
                if (sc == "schema_validation_mismatched_schema_fails")
                    return schema_validation_mismatched_schema_fails(pdir);
                if (sc == "load_script_missing_required_callback_returns_false")
                    return load_script_missing_required_callback_returns_false(pdir);
                if (sc == "eval_returns_not_found")
                    return eval_returns_not_found(pdir);
                if (sc == "generic_invoke_known_callback_returns_true")
                    return generic_invoke_known_callback_returns_true(pdir);
                if (sc == "supports_multi_state_default_false")
                    return supports_multi_state_default_false(pdir);
                if (sc == "context_fields_passed_to_plugin")
                    return context_fields_passed_to_plugin(pdir);
                if (sc == "checksum_wrong_hash_rejects_plugin")
                    return checksum_wrong_hash_rejects_plugin(pdir);
                if (sc == "checksum_empty_hash_skips_verification")
                    return checksum_empty_hash_skips_verification(pdir);
                if (sc == "api_counters_and_schema_size_through_native_module")
                    return api_counters_and_schema_size_through_native_module(pdir);
                if (sc == "full_startup_producer_slot_only")
                    return full_startup_producer_slot_only(pdir);
                if (sc == "full_startup_consumer")
                    return full_startup_consumer(pdir);
                if (sc == "full_startup_processor")
                    return full_startup_processor(pdir);
                if (sc == "full_startup_producer_multifield")
                    return full_startup_producer_multifield(pdir);
                if (sc == "full_startup_consumer_multifield")
                    return full_startup_consumer_multifield(pdir);
                if (sc == "full_startup_processor_multifield")
                    return full_startup_processor_multifield(pdir);
                if (sc == "full_startup_producer_slot_and_flexzone")
                    return full_startup_producer_slot_and_flexzone(pdir);
                if (sc == "invoke_on_inbox_typed_data")
                    return invoke_on_inbox_typed_data(pdir);
                if (sc == "api_band_pub_sub_no_broker_graceful_return")
                    return api_band_pub_sub_no_broker_graceful_return(pdir);

                fmt::print(stderr,
                           "[native_engine] ERROR: unknown scenario '{}'\n", sc);
                return 1;
            });
    }
};
static NativeEngineWorkerRegistrar g_native_engine_registrar;

} // namespace
