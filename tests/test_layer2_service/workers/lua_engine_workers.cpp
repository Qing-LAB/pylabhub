/**
 * @file lua_engine_workers.cpp
 * @brief LuaEngine worker bodies — Chunk 1 (lifecycle + type registration).
 *
 * Layer scope: L2 (scripting). Each body constructs LuaEngine +
 * RoleHostCore + RoleAPIBase and exercises the ScriptEngine contract
 * using raw memory buffers through InvokeTx/InvokeRx — no L3
 * ShmQueue/ZmqQueue/DataBlock/Broker involvement. See
 * docs/HEP/HEP-CORE-0001-hybrid-lifecycle-model.md § "Testing
 * implications" for the L2 vs L3 test boundary.
 *
 * V2 conversion: every body fabricates a RoleAPIBase (which constructs
 * a ThreadManager that registers a dynamic lifecycle module) — so all
 * bodies must run under run_gtest_worker with Logger owned by the
 * subprocess.
 */
#include "lua_engine_workers.h"

#include "lua_engine.hpp"
#include "utils/role_api_base.hpp"
#include "utils/role_host_core.hpp"
#include "utils/schema_utils.hpp"

#include "plh_service.hpp"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"
#include "test_schema_helpers.h"

#include <fmt/core.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace fs = std::filesystem;
using pylabhub::hub::FieldDef;
using pylabhub::hub::SchemaSpec;
using pylabhub::scripting::LuaEngine;
using pylabhub::scripting::RoleAPIBase;
using pylabhub::scripting::RoleHostCore;
using pylabhub::tests::all_types_schema;
using pylabhub::tests::helper::run_gtest_worker;
using pylabhub::tests::simple_schema;
using pylabhub::utils::Logger;

namespace pylabhub::tests::worker
{
namespace lua_engine
{
namespace
{

/// Writes the given Lua source to `<dir>/init.lua`.
void write_script(const fs::path &dir, const std::string &content)
{
    std::ofstream f(dir / "init.lua");
    f << content;
}

/// Produces a fresh RoleAPIBase wired to the given core. Mirrors the
/// original test fixture's helper so test bodies stay 1:1 with the
/// pre-conversion reading.
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

/// Composes initialize + load_script + register_slot_type(simple) +
/// build_api. Returns the owning api so the caller can keep it alive
/// past return (RoleAPIBase must outlive the engine's invoke paths).
///
/// This is a utility, not a macro, so line numbers in failed asserts
/// inside it point here — callers then see `setup_engine returned false`
/// as a separate ASSERT line.
bool setup_engine(LuaEngine &engine, RoleHostCore &core,
                  const fs::path &dir,
                  std::unique_ptr<RoleAPIBase> &test_api_out,
                  const std::string &required_cb = "on_produce",
                  const std::string &tag         = "prod")
{
    if (!engine.initialize("test", &core))
        return false;
    if (!engine.load_script(dir, "init.lua", required_cb.c_str()))
        return false;
    auto spec = simple_schema();
    if (!engine.register_slot_type(spec, "OutSlotFrame", "aligned"))
        return false;
    test_api_out = make_api(core, tag);
    return engine.build_api(*test_api_out);
}

} // namespace

// ── Lifecycle ───────────────────────────────────────────────────────────────

int full_lifecycle(const std::string &dir)
{
    // STRENGTHENED vs. original FullLifecycle: the pre-conversion test had
    // empty on_init/on_stop bodies and only asserted `invoke_on_*` didn't
    // crash — which a no-op implementation would also pass.  The strengthened
    // body has the Lua callbacks report a distinct metric each, so we can
    // verify the callbacks actually executed by inspecting
    // core.custom_metrics_snapshot() afterwards.
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir, R"(
                function on_produce(tx, msgs, api)
                    return true
                end
                function on_init(api)
                    api.report_metric("on_init_called", 1.0)
                end
                function on_stop(api)
                    api.report_metric("on_stop_called", 1.0)
                end
            )");

            RoleHostCore core;
            LuaEngine    engine;
            std::unique_ptr<RoleAPIBase> api;
            ASSERT_TRUE(setup_engine(engine, core, script_dir, api));

            // Before invoking anything: metrics should not contain the keys.
            auto before = core.custom_metrics_snapshot();
            EXPECT_EQ(before.count("on_init_called"), 0u);
            EXPECT_EQ(before.count("on_stop_called"), 0u);

            engine.invoke_on_init();
            auto after_init = core.custom_metrics_snapshot();
            EXPECT_EQ(after_init.count("on_init_called"), 1u)
                << "on_init must have dispatched into the Lua runtime";
            EXPECT_EQ(after_init.count("on_stop_called"), 0u);

            engine.invoke_on_stop();
            auto after_stop = core.custom_metrics_snapshot();
            EXPECT_EQ(after_stop.count("on_init_called"), 1u);
            EXPECT_EQ(after_stop.count("on_stop_called"), 1u)
                << "on_stop must have dispatched into the Lua runtime";

            EXPECT_EQ(engine.script_error_count(), 0u)
                << "no callback should have raised";

            engine.finalize();
        },
        "lua_engine::full_lifecycle", Logger::GetLifecycleModule());
}

int initialize_and_finalize_succeeds(const std::string &dir)
{
    // RENAMED from InitializeFailsGracefully: the original test's name,
    // comment ("double initialize should not crash"), and body (single
    // initialize + finalize) all disagreed.  Body content is preserved;
    // the name now reflects what the body actually tests.
    //
    // A real "initialize_fails_gracefully" test would need a deliberate
    // failure-injection hook (e.g. simulate luaL_newstate() returning
    // nullptr).  No such hook currently exists — adding one is deferred
    // to a later dedicated commit; this test covers the happy path only.
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            (void)script_dir;  // not used — we don't load a script here

            RoleHostCore core;
            LuaEngine    engine;
            ASSERT_TRUE(engine.initialize("test", &core));
            engine.finalize();
        },
        "lua_engine::initialize_and_finalize_succeeds",
        Logger::GetLifecycleModule());
}

// ── Type registration ──────────────────────────────────────────────────────

int register_slot_type_sizeof_correct(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir,
                         "function on_produce(tx, msgs, api) return true end");

            RoleHostCore core;
            LuaEngine    engine;
            ASSERT_TRUE(engine.initialize("test", &core));
            ASSERT_TRUE(engine.load_script(script_dir, "init.lua", "on_produce"));

            auto spec = simple_schema();
            ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame",
                                                  "aligned"));
            EXPECT_EQ(engine.type_sizeof("OutSlotFrame"), 4u)
                << "simple_schema is a single float32 field";

            engine.finalize();
        },
        "lua_engine::register_slot_type_sizeof_correct",
        Logger::GetLifecycleModule());
}

int register_slot_type_multi_field(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir,
                         "function on_produce(tx, msgs, api) return true end");

            RoleHostCore core;
            LuaEngine    engine;
            ASSERT_TRUE(engine.initialize("test", &core));
            ASSERT_TRUE(engine.load_script(script_dir, "init.lua", "on_produce"));

            SchemaSpec spec;
            spec.has_schema = true;
            spec.fields.push_back({"x", "float32", 1, 0});
            spec.fields.push_back({"y", "float32", 1, 0});
            spec.fields.push_back({"z", "float32", 1, 0});
            ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame",
                                                  "aligned"));
            EXPECT_EQ(engine.type_sizeof("OutSlotFrame"), 12u)
                << "3 x float32 = 12 bytes";

            engine.finalize();
        },
        "lua_engine::register_slot_type_multi_field",
        Logger::GetLifecycleModule());
}

int register_slot_type_packed_vs_aligned(const std::string &dir)
{
    // STRENGTHENED vs. original RegisterSlotType_PackedPacking: the
    // pre-conversion test only asserted `packed` gave 5 bytes; a bug where
    // the packing argument was silently ignored and aligned were always
    // used would slip by (aligned also gives 8, so forgetting to pack
    // would look fine for this one schema).  The strengthened test
    // registers the SAME fields under both packings and asserts BOTH
    // sizes — aligned(8) vs. packed(5) — so the mode parameter's effect
    // is verified, not merely accepted.
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir,
                         "function on_produce(tx, msgs, api) return true end");

            RoleHostCore core;
            LuaEngine    engine;
            ASSERT_TRUE(engine.initialize("test", &core));
            ASSERT_TRUE(engine.load_script(script_dir, "init.lua", "on_produce"));

            // bool + int32: aligned=8 (3 bytes of padding after bool),
            //               packed=5 (no padding).
            SchemaSpec spec;
            spec.has_schema = true;
            spec.fields.push_back({"flag", "bool",  1, 0});
            spec.fields.push_back({"val",  "int32", 1, 0});

            ASSERT_TRUE(engine.register_slot_type(spec, "AlignedFrame",
                                                  "aligned"));
            EXPECT_EQ(engine.type_sizeof("AlignedFrame"), 8u)
                << "aligned: bool(1) + 3 bytes padding + int32(4) = 8";

            ASSERT_TRUE(engine.register_slot_type(spec, "PackedFrame",
                                                  "packed"));
            EXPECT_EQ(engine.type_sizeof("PackedFrame"), 5u)
                << "packed: bool(1) + int32(4) = 5";

            EXPECT_NE(engine.type_sizeof("AlignedFrame"),
                      engine.type_sizeof("PackedFrame"))
                << "aligned and packed must produce different layouts for "
                   "this schema — if equal, the mode arg was ignored";

            engine.finalize();
        },
        "lua_engine::register_slot_type_packed_vs_aligned",
        Logger::GetLifecycleModule());
}

int register_slot_type_has_schema_false_returns_false(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir,
                         "function on_produce(tx, msgs, api) return true end");

            RoleHostCore core;
            LuaEngine    engine;
            ASSERT_TRUE(engine.initialize("test", &core));
            ASSERT_TRUE(engine.load_script(script_dir, "init.lua", "on_produce"));

            SchemaSpec spec;
            spec.has_schema = false;
            EXPECT_FALSE(engine.register_slot_type(spec, "OutSlotFrame",
                                                   "aligned"));

            engine.finalize();
        },
        "lua_engine::register_slot_type_has_schema_false_returns_false",
        Logger::GetLifecycleModule());
}

int register_slot_type_all_supported_types(const std::string &dir)
{
    // NEW coverage fill: before this test the suite never registered any
    // slot with bool / int8 / int16 / uint64 — leaving the engine's
    // dispatcher branches for those types (lua_engine.cpp:623,624,626,631)
    // untested at L2.  Uses the shared test helper all_types_schema()
    // which is documented to cover every scalar type the engine supports.
    //
    // Asserts: registration succeeds under both aligned and packed, the
    // resulting struct has non-zero sizeof, and aligned >= packed (a
    // schema with sub-word-aligned fields must not shrink under aligned
    // mode — a surprising inequality would indicate a real bug in the
    // layout computation).
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir,
                         "function on_produce(tx, msgs, api) return true end");

            RoleHostCore core;
            LuaEngine    engine;
            ASSERT_TRUE(engine.initialize("test", &core));
            ASSERT_TRUE(engine.load_script(script_dir, "init.lua", "on_produce"));

            auto spec = all_types_schema();
            ASSERT_TRUE(engine.register_slot_type(spec, "AllAligned",
                                                  "aligned"))
                << "every scalar type must register under aligned";
            ASSERT_TRUE(engine.register_slot_type(spec, "AllPacked",
                                                  "packed"))
                << "every scalar type must register under packed";

            const size_t aligned_sz = engine.type_sizeof("AllAligned");
            const size_t packed_sz  = engine.type_sizeof("AllPacked");
            EXPECT_GT(aligned_sz, 0u);
            EXPECT_GT(packed_sz, 0u);
            EXPECT_GE(aligned_sz, packed_sz)
                << "aligned layout can never be smaller than packed for "
                   "the same fields";
            EXPECT_EQ(aligned_sz,
                      pylabhub::hub::compute_schema_size(spec, "aligned"))
                << "engine sizeof must match compute_schema_size (the same "
                   "helper the role host uses to size buffers)";
            EXPECT_EQ(packed_sz,
                      pylabhub::hub::compute_schema_size(spec, "packed"));

            engine.finalize();
        },
        "lua_engine::register_slot_type_all_supported_types",
        Logger::GetLifecycleModule());
}

// ── Alias creation ─────────────────────────────────────────────────────────

int alias_slot_frame_producer(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir,
                         "function on_produce(tx, msgs, api) return true end");

            RoleHostCore core;
            LuaEngine    engine;
            ASSERT_TRUE(engine.initialize("test", &core));
            ASSERT_TRUE(engine.load_script(script_dir, "init.lua", "on_produce"));

            auto spec = simple_schema();
            ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame",
                                                  "aligned"));

            auto api = make_api(core);
            ASSERT_TRUE(engine.build_api(*api));

            EXPECT_EQ(engine.type_sizeof("SlotFrame"),
                      engine.type_sizeof("OutSlotFrame"))
                << "producer build_api must expose OutSlotFrame under the "
                   "role-agnostic 'SlotFrame' alias";
            EXPECT_GT(engine.type_sizeof("SlotFrame"), 0u);

            engine.finalize();
        },
        "lua_engine::alias_slot_frame_producer",
        Logger::GetLifecycleModule());
}

int alias_slot_frame_consumer(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir,
                         "function on_consume(rx, msgs, api) return true end");

            RoleHostCore core;
            LuaEngine    engine;
            ASSERT_TRUE(engine.initialize("test", &core));
            ASSERT_TRUE(engine.load_script(script_dir, "init.lua", "on_consume"));

            auto spec = simple_schema();
            ASSERT_TRUE(engine.register_slot_type(spec, "InSlotFrame",
                                                  "aligned"));

            auto api = make_api(core, "cons");
            ASSERT_TRUE(engine.build_api(*api));

            EXPECT_EQ(engine.type_sizeof("SlotFrame"),
                      engine.type_sizeof("InSlotFrame"))
                << "consumer build_api must expose InSlotFrame under the "
                   "role-agnostic 'SlotFrame' alias";
            EXPECT_GT(engine.type_sizeof("SlotFrame"), 0u);

            engine.finalize();
        },
        "lua_engine::alias_slot_frame_consumer",
        Logger::GetLifecycleModule());
}

int alias_no_alias_processor(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir,
                         "function on_process(rx, tx, msgs, api) return true end");

            RoleHostCore core;
            LuaEngine    engine;
            ASSERT_TRUE(engine.initialize("test", &core));
            ASSERT_TRUE(engine.load_script(script_dir, "init.lua", "on_process"));

            auto spec = simple_schema();
            ASSERT_TRUE(engine.register_slot_type(spec, "InSlotFrame",
                                                  "aligned"));
            ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame",
                                                  "aligned"));

            auto api = make_api(core, "proc");
            ASSERT_TRUE(engine.build_api(*api));

            // Processor: explicit — no "SlotFrame" alias because the role has
            // both an input and an output; the alias would be ambiguous.
            EXPECT_EQ(engine.type_sizeof("SlotFrame"), 0u)
                << "processor must not have a bare 'SlotFrame' alias";
            EXPECT_GT(engine.type_sizeof("InSlotFrame"), 0u);
            EXPECT_GT(engine.type_sizeof("OutSlotFrame"), 0u);

            engine.finalize();
        },
        "lua_engine::alias_no_alias_processor",
        Logger::GetLifecycleModule());
}

int alias_flex_frame_producer(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir,
                         R"(function on_produce(tx, msgs, api) return true end)");

            RoleHostCore core;
            LuaEngine    engine;
            ASSERT_TRUE(engine.initialize("test", &core));
            ASSERT_TRUE(engine.load_script(script_dir, "init.lua", "on_produce"));

            auto spec = simple_schema();
            ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame",
                                                  "aligned"));
            ASSERT_TRUE(engine.register_slot_type(spec, "OutFlexFrame",
                                                  "aligned"));

            auto api = make_api(core);
            ASSERT_TRUE(engine.build_api(*api));

            EXPECT_EQ(engine.type_sizeof("FlexFrame"),
                      engine.type_sizeof("OutFlexFrame"))
                << "producer with registered OutFlexFrame must expose "
                   "'FlexFrame' alias";
            EXPECT_GT(engine.type_sizeof("FlexFrame"), 0u);

            engine.finalize();
        },
        "lua_engine::alias_flex_frame_producer",
        Logger::GetLifecycleModule());
}

int alias_producer_no_fz_no_flex_frame_alias(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir,
                         "function on_produce(tx, msgs, api) return true end");

            RoleHostCore core;
            LuaEngine    engine;
            ASSERT_TRUE(engine.initialize("test", &core));
            ASSERT_TRUE(engine.load_script(script_dir, "init.lua", "on_produce"));

            auto spec = simple_schema();
            ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame",
                                                  "aligned"));
            // Deliberately NO OutFlexFrame registered.

            auto api = make_api(core);
            ASSERT_TRUE(engine.build_api(*api));

            EXPECT_GT(engine.type_sizeof("SlotFrame"), 0u)
                << "SlotFrame alias exists since OutSlotFrame was registered";
            EXPECT_EQ(engine.type_sizeof("FlexFrame"), 0u)
                << "FlexFrame alias must not exist when no OutFlexFrame "
                   "was registered";

            engine.finalize();
        },
        "lua_engine::alias_producer_no_fz_no_flex_frame_alias",
        Logger::GetLifecycleModule());
}

// ── invoke_produce (chunk 2) ────────────────────────────────────────────────
//
// These bodies exercise the callback-return-value contract:
//   true   → InvokeResult::Commit (caller publishes the slot)
//   false  → InvokeResult::Discard (caller drops the slot)
//   nil    → InvokeResult::Error (missing explicit return is an error)
//   error() → InvokeResult::Error + script_error_count++
//
// The engine does NOT roll back Lua-side writes to tx.slot on Discard —
// the slot buffer is owned by the caller (the role host / test
// harness), and the return value only tells the caller whether to
// publish it. The DiscardOnFalse_ButLuaWroteSlot test pins this
// contract explicitly.

namespace
{

/// Standard boilerplate: write the given Lua source, set up an engine
/// with a simple schema registered, run run_gtest_worker. Inner body
/// is passed as a functor that receives the live engine + a buffer
/// pointer + an empty IncomingMessage vector.
///
/// Kept as a helper function (not a macro) so compile errors report
/// accurate source lines.
template <typename F>
int produce_worker_with_script(const std::string &dir,
                               const char *scenario_name,
                               const std::string &lua_source,
                               F &&body)
{
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir, lua_source);

            RoleHostCore core;
            LuaEngine    engine;
            std::unique_ptr<RoleAPIBase> api;
            ASSERT_TRUE(setup_engine(engine, core, script_dir, api));

            body(engine, core);

            engine.finalize();
        },
        scenario_name, Logger::GetLifecycleModule());
}

} // namespace

int invoke_produce_commit_on_true(const std::string &dir)
{
    // Strengthened over the pre-conversion test by also asserting
    // script_error_count == 0 post-invoke (catches a regression where
    // the engine reports Commit but silently logged a script error).
    return produce_worker_with_script(
        dir, "lua_engine::invoke_produce_commit_on_true",
        R"(
            function on_produce(tx, msgs, api)
                if tx.slot then
                    tx.slot.value = 42.0
                end
                return true
            end
        )",
        [](pylabhub::scripting::LuaEngine &engine,
           pylabhub::scripting::RoleHostCore & /*core*/) {
            float buf = 0.0f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            auto result = engine.invoke_produce(
                pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(result, pylabhub::scripting::InvokeResult::Commit);
            EXPECT_FLOAT_EQ(buf, 42.0f);
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "commit path must not emit script errors";
        });
}

int invoke_produce_discard_on_false(const std::string &dir)
{
    // Strengthened over the pre-conversion test. The original declared
    // `float buf = 0.0f` and only asserted `result == Discard` — if
    // the engine secretly wrote 0.0 to buf on the Discard path, the
    // test would still pass (buf == 0.0 is indistinguishable from
    // uninitialized sentinel of 0.0).
    //
    // This body inits buf to a non-default sentinel (777.0f) that the
    // Lua script does NOT write, then asserts buf is STILL 777.0f
    // post-invoke. That catches any engine-internal write on the
    // Discard path.
    return produce_worker_with_script(
        dir, "lua_engine::invoke_produce_discard_on_false",
        R"(
            function on_produce(tx, msgs, api)
                -- deliberately do NOT touch tx.slot; return false
                return false
            end
        )",
        [](pylabhub::scripting::LuaEngine &engine,
           pylabhub::scripting::RoleHostCore & /*core*/) {
            float buf = 777.0f;  // sentinel — Lua script doesn't write
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            auto result = engine.invoke_produce(
                pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(result, pylabhub::scripting::InvokeResult::Discard);
            EXPECT_FLOAT_EQ(buf, 777.0f)
                << "engine must not write buf on Discard when Lua didn't write";
            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

int invoke_produce_nil_return_is_error(const std::string &dir)
{
    return produce_worker_with_script(
        dir, "lua_engine::invoke_produce_nil_return_is_error",
        R"(
            function on_produce(tx, msgs, api)
                -- no explicit return → nil → error (must be explicit)
            end
        )",
        [](pylabhub::scripting::LuaEngine &engine,
           pylabhub::scripting::RoleHostCore & /*core*/) {
            float buf = 0.0f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            auto result = engine.invoke_produce(
                pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(result, pylabhub::scripting::InvokeResult::Error)
                << "missing return is an error — must be explicit true/false";
            EXPECT_EQ(engine.script_error_count(), 1u);
        });
}

int invoke_produce_nil_slot(const std::string &dir)
{
    // Strengthened over the pre-conversion test. The original relied on
    // a `assert(tx.slot == nil, ...)` inside the Lua body but never
    // verified that assertion held from the C++ side — if the engine
    // silently dispatched a non-nil slot, the Lua assert would fail,
    // bump script_error_count, and the test would still see result=
    // Discard (because assert() aborts, Lua returns nil...error... but
    // the test asserts Discard).  Added an explicit
    // EXPECT_EQ(script_error_count, 0) to confirm the Lua assert passed,
    // i.e. the engine really did pass nil for InvokeTx{nullptr, 0}.
    return produce_worker_with_script(
        dir, "lua_engine::invoke_produce_nil_slot",
        R"(
            function on_produce(tx, msgs, api)
                assert(tx.slot == nil, "expected nil slot")
                return false
            end
        )",
        [](pylabhub::scripting::LuaEngine &engine,
           pylabhub::scripting::RoleHostCore & /*core*/) {
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            auto result = engine.invoke_produce(
                pylabhub::scripting::InvokeTx{nullptr, 0}, msgs);
            EXPECT_EQ(result, pylabhub::scripting::InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "the Lua assert(tx.slot == nil) must have passed; "
                   "a non-zero count means the engine dispatched a non-nil "
                   "slot when given InvokeTx{nullptr, 0}";
        });
}

int invoke_produce_script_error(const std::string &dir)
{
    return produce_worker_with_script(
        dir, "lua_engine::invoke_produce_script_error",
        R"(
            function on_produce(tx, msgs, api)
                error("intentional error")
            end
        )",
        [](pylabhub::scripting::LuaEngine &engine,
           pylabhub::scripting::RoleHostCore & /*core*/) {
            float buf = 0.0f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            EXPECT_EQ(engine.script_error_count(), 0u);
            auto result = engine.invoke_produce(
                pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(result, pylabhub::scripting::InvokeResult::Error);
            EXPECT_EQ(engine.script_error_count(), 1u);
        });
}

int invoke_produce_discard_on_false_but_lua_wrote_slot(const std::string &dir)
{
    // NEW test — pins the production contract that the engine does NOT
    // roll back Lua-side writes to tx.slot when the callback returns
    // false.  The slot buffer is caller-owned; the return value only
    // tells the caller whether to publish the slot downstream.  A user
    // who writes `tx.slot.value = 42 then return false` gets:
    //   result == Discard (caller drops the slot)
    //   buf    == 42.0     (caller's buffer was mutated in place)
    //
    // This subtlety matters because a role host implementing
    // "conditionally publish" may reuse the same buffer for the next
    // iteration; if they expected the engine to clear it on Discard
    // they'd get stale data.
    return produce_worker_with_script(
        dir,
        "lua_engine::invoke_produce_discard_on_false_but_lua_wrote_slot",
        R"(
            function on_produce(tx, msgs, api)
                if tx.slot then
                    tx.slot.value = 42.0
                end
                return false
            end
        )",
        [](pylabhub::scripting::LuaEngine &engine,
           pylabhub::scripting::RoleHostCore & /*core*/) {
            float buf = 0.0f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            auto result = engine.invoke_produce(
                pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(result, pylabhub::scripting::InvokeResult::Discard)
                << "explicit `return false` must yield Discard";
            EXPECT_FLOAT_EQ(buf, 42.0f)
                << "Lua write to tx.slot.value must propagate to the caller's "
                   "buffer regardless of return value — the engine does NOT "
                   "roll back on Discard.";
            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

// ── invoke_consume (chunk 3) ────────────────────────────────────────────────
//
// These bodies exercise the consumer-side callback contract. `rx` is
// documented as "Input direction (read-only slot + flexzone)" — the
// RxSlot_IsReadOnly test pins the "read-only" part, which the original
// ReceivesReadOnlySlot test named but did not actually verify.
//
// invoke_consume returns InvokeResult (Commit/Discard/Error); per the
// header comment the consumer data loop currently ignores it
// ("Currently ignored by the consumer data loop — reserved for future
// flow control"), but the engine still computes it and the tests assert
// on it so a regression in the dispatch path does not silently pass.

namespace
{

/// Consumer variant of setup_engine: loads a script with `on_consume`
/// as the required callback, registers InSlotFrame (not OutSlotFrame),
/// builds the API with tag "cons" so the SlotFrame alias points at
/// InSlotFrame.
bool setup_consumer_engine(LuaEngine &engine, RoleHostCore &core,
                           const fs::path &dir,
                           std::unique_ptr<RoleAPIBase> &api_out)
{
    if (!engine.initialize("test", &core))
        return false;
    if (!engine.load_script(dir, "init.lua", "on_consume"))
        return false;
    auto spec = simple_schema();
    if (!engine.register_slot_type(spec, "InSlotFrame", "aligned"))
        return false;
    api_out = make_api(core, "cons");
    return engine.build_api(*api_out);
}

/// Consumer-side equivalent of produce_worker_with_script.
template <typename F>
int consume_worker_with_script(const std::string &dir,
                               const char *scenario_name,
                               const std::string &lua_source,
                               F &&body)
{
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir, lua_source);

            RoleHostCore core;
            LuaEngine    engine;
            std::unique_ptr<RoleAPIBase> api;
            ASSERT_TRUE(setup_consumer_engine(engine, core, script_dir, api));

            body(engine, core);

            engine.finalize();
        },
        scenario_name, Logger::GetLifecycleModule());
}

} // namespace

int invoke_consume_receives_slot(const std::string &dir)
{
    // Strengthened over the pre-conversion test (which was called
    // InvokeConsume_ReceivesReadOnlySlot but did not actually exercise
    // the "read-only" part — see the dedicated rx_slot_is_read_only
    // worker below for that coverage).  The original body only checked
    // script_error_count == 0 (i.e. the Lua-side `assert(rx.slot.value
    // ~= nil)` passed).  This body additionally asserts result ==
    // Commit, so a regression in the invoke_consume return-value
    // dispatch is caught even though the production consumer loop
    // currently ignores the return value.
    return consume_worker_with_script(
        dir, "lua_engine::invoke_consume_receives_slot",
        R"(
            function on_consume(rx, msgs, api)
                assert(rx.slot ~= nil, "expected non-nil slot")
                assert(math.abs(rx.slot.value - 99.5) < 0.01,
                       "expected value ~99.5, got " .. tostring(rx.slot.value))
                return true
            end
        )",
        [](pylabhub::scripting::LuaEngine &engine,
           pylabhub::scripting::RoleHostCore & /*core*/) {
            float data = 99.5f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            auto result = engine.invoke_consume(
                pylabhub::scripting::InvokeRx{&data, sizeof(data)}, msgs);
            EXPECT_EQ(result, pylabhub::scripting::InvokeResult::Commit)
                << "on_consume returning true must map to Commit even though "
                   "the data loop currently ignores the return value";
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "Lua-side assert must have passed — if this fails, the "
                   "engine passed the wrong value in rx.slot";
        });
}

int invoke_consume_nil_slot(const std::string &dir)
{
    // Strengthened: also asserts result == Commit (Lua returns true).
    // The pre-conversion test only checked script_error_count == 0,
    // which confirms the Lua-side `assert(rx.slot == nil)` passed but
    // does not catch a dispatch regression that e.g. maps "true" to
    // Discard.
    return consume_worker_with_script(
        dir, "lua_engine::invoke_consume_nil_slot",
        R"(
            function on_consume(rx, msgs, api)
                assert(rx.slot == nil, "expected nil")
                return true
            end
        )",
        [](pylabhub::scripting::LuaEngine &engine,
           pylabhub::scripting::RoleHostCore & /*core*/) {
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            auto result = engine.invoke_consume(
                pylabhub::scripting::InvokeRx{nullptr, 0}, msgs);
            EXPECT_EQ(result, pylabhub::scripting::InvokeResult::Commit);
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "Lua assert(rx.slot == nil) must have passed";
        });
}

int invoke_consume_script_error_detected(const std::string &dir)
{
    // Strengthened: additionally asserts result == Error.  The
    // pre-conversion test only checked script_error_count == 1, which
    // confirms the engine recognised the error — but not that it
    // translated it to the Error result code.
    return consume_worker_with_script(
        dir, "lua_engine::invoke_consume_script_error_detected",
        R"(
            function on_consume(rx, msgs, api)
                error("consume error")
            end
        )",
        [](pylabhub::scripting::LuaEngine &engine,
           pylabhub::scripting::RoleHostCore & /*core*/) {
            float data = 1.0f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            auto result = engine.invoke_consume(
                pylabhub::scripting::InvokeRx{&data, sizeof(data)}, msgs);
            EXPECT_EQ(result, pylabhub::scripting::InvokeResult::Error);
            EXPECT_EQ(engine.script_error_count(), 1u);
        });
}

int invoke_consume_rx_slot_is_read_only(const std::string &dir)
{
    // NEW test — pins the "read-only" part of the consumer contract
    // (see src/include/utils/script_engine.hpp:341:
    //   "Input direction (read-only slot + flexzone)").
    //
    // The pre-conversion InvokeConsume_ReceivesReadOnlySlot test name
    // promised this coverage but the body never attempted a write to
    // rx.slot — so whether the "read-only" claim holds at the ffi
    // boundary was unverified.
    //
    // This worker pre-fills the underlying C buffer with a sentinel
    // (99.5), attempts a Lua-side write of a different value (777.0)
    // to rx.slot.value, and asserts the underlying buffer is still 99.5
    // after invoke_consume. Either of these outcomes is a valid
    // "read-only" implementation:
    //
    //   A) Engine's ffi cast produces a `const T*`; Lua write raises a
    //      runtime error → script_error_count increments and result ==
    //      InvokeResult::Error.
    //   B) LuaJIT's ffi silently no-ops const writes (accepted per
    //      LuaJIT docs); Lua returns normally → result == Commit.
    //
    // What is NOT valid is the C buffer getting the value 777.0.  The
    // test pins: (buf == 99.5) AND result != Discard — both A and B
    // satisfy it; a buffer-overwrite bug fails it.
    //
    // The body does not assert which of A or B actually happens so
    // this test does not over-fit to the current implementation — if
    // LuaJIT const-write semantics change, the test still passes as
    // long as the read-only invariant is upheld.
    return consume_worker_with_script(
        dir, "lua_engine::invoke_consume_rx_slot_is_read_only",
        R"(
            function on_consume(rx, msgs, api)
                -- Attempt to mutate rx.slot. Per the engine contract this
                -- must not propagate to the underlying C buffer.
                rx.slot.value = 777.0
                return true
            end
        )",
        [](pylabhub::scripting::LuaEngine &engine,
           pylabhub::scripting::RoleHostCore & /*core*/) {
            float data = 99.5f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            auto result = engine.invoke_consume(
                pylabhub::scripting::InvokeRx{&data, sizeof(data)}, msgs);

            // Core invariant: the buffer is owned by the caller and is
            // read-only from Lua's perspective. Lua's write attempt
            // must not propagate.
            EXPECT_FLOAT_EQ(data, 99.5f)
                << "rx.slot is documented read-only; Lua's `rx.slot.value = "
                   "777.0` must NOT propagate to the underlying C buffer. "
                   "If data == 777.0 here, that's a real contract violation "
                   "(Lua has write access to the caller's input buffer).";

            // Either valid outcome:
            //   - Discard should NOT happen (the Lua code did not return
            //     false anywhere); anything else means the contract holds.
            EXPECT_NE(result, pylabhub::scripting::InvokeResult::Discard)
                << "unexpected Discard — Lua explicitly `return true`";
        });
}

// ── invoke_process (chunk 4) ────────────────────────────────────────────────
//
// Processor role: always has an input channel by definition (a role
// without an input channel is a consumer, not a processor).  Nil rx.slot
// inside on_process represents a runtime condition (input queue timed
// out / no data) not a missing channel; nil tx.slot similarly represents
// "output queue has no slot right now" (backpressure).
//
// The scenarios below are named by slot-state combination to keep the
// contract under test explicit: DualSlots = rx+tx present, BothSlotsNil
// = both timed out, RxPresent_TxNil = input available but output
// backpressured.  All three cases are real production states the
// processor's main loop feeds to on_process.

namespace
{

/// Processor variant of setup_engine.  Registers both InSlotFrame and
/// OutSlotFrame (processor uses separate in/out types; no SlotFrame
/// alias — see alias_no_alias_processor in chunk 1), builds API with
/// tag "proc".
bool setup_processor_engine(LuaEngine &engine, RoleHostCore &core,
                            const fs::path &dir,
                            std::unique_ptr<RoleAPIBase> &api_out)
{
    if (!engine.initialize("test", &core))
        return false;
    if (!engine.load_script(dir, "init.lua", "on_process"))
        return false;
    auto spec = simple_schema();
    if (!engine.register_slot_type(spec, "InSlotFrame", "aligned"))
        return false;
    if (!engine.register_slot_type(spec, "OutSlotFrame", "aligned"))
        return false;
    api_out = make_api(core, "proc");
    return engine.build_api(*api_out);
}

/// Processor-side equivalent of produce/consume_worker_with_script.
template <typename F>
int process_worker_with_script(const std::string &dir,
                               const char *scenario_name,
                               const std::string &lua_source,
                               F &&body)
{
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir, lua_source);

            RoleHostCore core;
            LuaEngine    engine;
            std::unique_ptr<RoleAPIBase> api;
            ASSERT_TRUE(setup_processor_engine(engine, core, script_dir, api));

            body(engine, core);

            engine.finalize();
        },
        scenario_name, Logger::GetLifecycleModule());
}

} // namespace

int invoke_process_dual_slots(const std::string &dir)
{
    // Strengthened over the pre-conversion test:
    //
    //   a) Asserts script_error_count == 0 (catches a regression where
    //      the engine silently logged a script error alongside returning
    //      Commit).
    //
    //   b) Asserts the rx input buffer was NOT mutated even though Lua
    //      read rx.slot.value.  rx is documented read-only
    //      (script_engine.hpp:356) and the processor dual-slot code
    //      path must preserve that contract the same way the consumer
    //      path does.  The pre-conversion test only checked out_data,
    //      leaving a regression where the engine wrote back to rx.slot
    //      (aliasing bug, say) undetectable.
    return process_worker_with_script(
        dir, "lua_engine::invoke_process_dual_slots",
        R"(
            function on_process(rx, tx, msgs, api)
                if rx.slot and tx.slot then
                    tx.slot.value = rx.slot.value * 2.0
                    return true
                end
                return false
            end
        )",
        [](pylabhub::scripting::LuaEngine &engine,
           pylabhub::scripting::RoleHostCore & /*core*/) {
            float in_data  = 21.0f;
            float out_data = 0.0f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            auto result = engine.invoke_process(
                pylabhub::scripting::InvokeRx{&in_data, sizeof(in_data)},
                pylabhub::scripting::InvokeTx{&out_data, sizeof(out_data)},
                msgs);
            EXPECT_EQ(result, pylabhub::scripting::InvokeResult::Commit);
            EXPECT_FLOAT_EQ(out_data, 42.0f)
                << "Lua computed tx.slot.value = rx.slot.value * 2.0 = 42.0";
            EXPECT_FLOAT_EQ(in_data, 21.0f)
                << "rx.slot is read-only — input buffer must not be mutated "
                   "by the engine's dual-slot dispatch path";
            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

int invoke_process_both_slots_nil(const std::string &dir)
{
    // Renamed from InvokeProcess_NilInput.  The original name could be
    // misread as "processor with no input channel", which is semantically
    // wrong (such a role is a consumer).  The actual scenario is runtime
    // state: both rx and tx paths produced nil — input queue timed out
    // AND output queue has no slot.  Processor-shaped code paths must
    // still pass rx/tx to Lua as nil and not attempt to dereference the
    // null pointer buffers.
    return process_worker_with_script(
        dir, "lua_engine::invoke_process_both_slots_nil",
        // NOTE: custom raw-string delimiter R"LUA( ... )LUA" is REQUIRED
        // whenever the Lua source contains parenthesised-substring-then-
        // quote sequences like `"... (...)"` — the default R"( ... )"
        // delimiter terminates at the first `)"`, which inside a Lua
        // string literal closes the raw string prematurely.
        R"LUA(
            function on_process(rx, tx, msgs, api)
                assert(rx.slot == nil, "expected nil input (timeout / no data)")
                assert(tx.slot == nil, "expected nil output (backpressure)")
                return false
            end
        )LUA",
        [](pylabhub::scripting::LuaEngine &engine,
           pylabhub::scripting::RoleHostCore & /*core*/) {
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            auto result = engine.invoke_process(
                pylabhub::scripting::InvokeRx{nullptr, 0},
                pylabhub::scripting::InvokeTx{nullptr, 0}, msgs);
            EXPECT_EQ(result, pylabhub::scripting::InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "Lua-side assert(both nil) must have passed; failure means "
                   "the engine dispatched a non-nil slot for a null/zero-size "
                   "InvokeRx or InvokeTx";
        });
}

int invoke_process_rx_present_tx_nil(const std::string &dir)
{
    // Renamed from InvokeProcess_InputOnlyNoOutput.  Original name could
    // be misread as "processor without output channel"; actual scenario
    // is "input has data, but the output queue is backpressured / no
    // slot available for this iteration".  Lua's typical response is
    // to drop (return false) or queue work internally; this test only
    // exercises the drop path.
    //
    // Strengthened: asserts script_error_count == 0 (the original left
    // this unchecked, so a Lua-side assert failure would bump the count
    // while the engine still returned Discard for other reasons).
    return process_worker_with_script(
        dir, "lua_engine::invoke_process_rx_present_tx_nil",
        // Custom delimiter — see process_both_slots_nil note above.
        R"LUA(
            function on_process(rx, tx, msgs, api)
                assert(rx.slot ~= nil, "expected input data")
                assert(tx.slot == nil, "expected nil output (backpressure)")
                return false
            end
        )LUA",
        [](pylabhub::scripting::LuaEngine &engine,
           pylabhub::scripting::RoleHostCore & /*core*/) {
            float in_data = 10.0f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            auto result = engine.invoke_process(
                pylabhub::scripting::InvokeRx{&in_data, sizeof(in_data)},
                pylabhub::scripting::InvokeTx{nullptr, 0}, msgs);
            EXPECT_EQ(result, pylabhub::scripting::InvokeResult::Discard);
            EXPECT_FLOAT_EQ(in_data, 10.0f)
                << "rx is still read-only in the rx-only-no-tx path";
            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

int invoke_process_rx_slot_is_read_only(const std::string &dir)
{
    // NEW test — the processor's dual-slot code path goes through
    // invoke_process (a separate engine entry from invoke_consume), so
    // the "rx.slot is read-only" contract needs its own coverage here.
    // Without this test the read-only invariant is unverified in the
    // processor path.
    //
    // Lua attempts `rx.slot.value = 777.0` then writes tx (which should
    // succeed).  C++ pre-fills the rx buffer with 99.5 and asserts it's
    // STILL 99.5 after invoke_process.  The tx-write succeeds normally
    // (tx is writable), so out_data ends up reflecting what Lua wrote
    // to tx — not whatever the rx-write would have produced.
    //
    // Either read-only outcome is accepted (see the consumer variant's
    // doc block for the full rationale):
    //   - Engine raises a Lua error on rx-write → Error result
    //   - LuaJIT silently no-ops → Commit result (Lua also wrote tx)
    return process_worker_with_script(
        dir, "lua_engine::invoke_process_rx_slot_is_read_only",
        R"(
            function on_process(rx, tx, msgs, api)
                rx.slot.value = 777.0   -- must NOT propagate
                tx.slot.value = 3.25    -- must propagate normally
                return true
            end
        )",
        [](pylabhub::scripting::LuaEngine &engine,
           pylabhub::scripting::RoleHostCore & /*core*/) {
            float in_data  = 99.5f;
            float out_data = 0.0f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            auto result = engine.invoke_process(
                pylabhub::scripting::InvokeRx{&in_data, sizeof(in_data)},
                pylabhub::scripting::InvokeTx{&out_data, sizeof(out_data)},
                msgs);
            EXPECT_FLOAT_EQ(in_data, 99.5f)
                << "rx.slot is read-only in invoke_process too; Lua's write "
                   "to rx.slot.value must NOT reach the caller's input buffer";
            // tx write should propagate regardless of the rx-write outcome,
            // EXCEPT if Lua raised an error on the rx-write before reaching
            // the tx-write line — in that case Lua aborted early and
            // out_data stays 0.0f.  Accept either "out_data = 3.25 (silent
            // no-op, Lua continued to tx write)" or "out_data = 0.0 (Lua
            // raised on rx write, never reached tx write)".
            EXPECT_TRUE(out_data == 3.25f || out_data == 0.0f)
                << "tx wrote iff Lua didn't raise; got out_data="
                << out_data;
            // Whichever path was taken, result must not be Discard — Lua
            // explicitly `return true`.
            EXPECT_NE(result, pylabhub::scripting::InvokeResult::Discard);
        });
}

} // namespace lua_engine
} // namespace pylabhub::tests::worker

// ── Dispatcher ──────────────────────────────────────────────────────────────

namespace
{

struct LuaEngineWorkerRegistrar
{
    LuaEngineWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "lua_engine")
                    return -1;
                std::string sc(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::lua_engine;

                if (argc <= 2) {
                    fmt::print(stderr,
                               "lua_engine.{}: missing <dir> arg\n", sc);
                    return 1;
                }
                const std::string dir = argv[2];

                if (sc == "full_lifecycle")
                    return full_lifecycle(dir);
                if (sc == "initialize_and_finalize_succeeds")
                    return initialize_and_finalize_succeeds(dir);
                if (sc == "register_slot_type_sizeof_correct")
                    return register_slot_type_sizeof_correct(dir);
                if (sc == "register_slot_type_multi_field")
                    return register_slot_type_multi_field(dir);
                if (sc == "register_slot_type_packed_vs_aligned")
                    return register_slot_type_packed_vs_aligned(dir);
                if (sc == "register_slot_type_has_schema_false_returns_false")
                    return register_slot_type_has_schema_false_returns_false(dir);
                if (sc == "register_slot_type_all_supported_types")
                    return register_slot_type_all_supported_types(dir);
                if (sc == "alias_slot_frame_producer")
                    return alias_slot_frame_producer(dir);
                if (sc == "alias_slot_frame_consumer")
                    return alias_slot_frame_consumer(dir);
                if (sc == "alias_no_alias_processor")
                    return alias_no_alias_processor(dir);
                if (sc == "alias_flex_frame_producer")
                    return alias_flex_frame_producer(dir);
                if (sc == "alias_producer_no_fz_no_flex_frame_alias")
                    return alias_producer_no_fz_no_flex_frame_alias(dir);

                // Chunk 2: invoke_produce
                if (sc == "invoke_produce_commit_on_true")
                    return invoke_produce_commit_on_true(dir);
                if (sc == "invoke_produce_discard_on_false")
                    return invoke_produce_discard_on_false(dir);
                if (sc == "invoke_produce_nil_return_is_error")
                    return invoke_produce_nil_return_is_error(dir);
                if (sc == "invoke_produce_nil_slot")
                    return invoke_produce_nil_slot(dir);
                if (sc == "invoke_produce_script_error")
                    return invoke_produce_script_error(dir);
                if (sc == "invoke_produce_discard_on_false_but_lua_wrote_slot")
                    return invoke_produce_discard_on_false_but_lua_wrote_slot(dir);

                // Chunk 3: invoke_consume
                if (sc == "invoke_consume_receives_slot")
                    return invoke_consume_receives_slot(dir);
                if (sc == "invoke_consume_nil_slot")
                    return invoke_consume_nil_slot(dir);
                if (sc == "invoke_consume_script_error_detected")
                    return invoke_consume_script_error_detected(dir);
                if (sc == "invoke_consume_rx_slot_is_read_only")
                    return invoke_consume_rx_slot_is_read_only(dir);

                // Chunk 4: invoke_process
                if (sc == "invoke_process_dual_slots")
                    return invoke_process_dual_slots(dir);
                if (sc == "invoke_process_both_slots_nil")
                    return invoke_process_both_slots_nil(dir);
                if (sc == "invoke_process_rx_present_tx_nil")
                    return invoke_process_rx_present_tx_nil(dir);
                if (sc == "invoke_process_rx_slot_is_read_only")
                    return invoke_process_rx_slot_is_read_only(dir);

                fmt::print(stderr,
                           "[lua_engine] ERROR: unknown scenario '{}'\n", sc);
                return 1;
            });
    }
};
static LuaEngineWorkerRegistrar g_lua_engine_registrar;

} // namespace
