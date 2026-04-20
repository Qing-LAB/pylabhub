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

/// Which role the LuaEngine is being set up for. Encodes the per-role
/// defaults for required callback, API tag, and default slot
/// registrations — see setup_role_engine below.
enum class RoleKind
{
    Producer,   ///< on_produce, tag "prod", register OutSlotFrame
    Consumer,   ///< on_consume, tag "cons", register InSlotFrame
    Processor,  ///< on_process, tag "proc", register InSlotFrame + OutSlotFrame
};

/// Composes initialize + load_script + per-role register_slot_type +
/// build_api. Owning api pointer returned via api_out so the caller
/// keeps it alive past this function (RoleAPIBase must outlive the
/// engine's invoke paths).
///
/// This is a utility, not a macro, so line numbers in failed asserts
/// inside it point here — callers then see `setup_role_engine returned
/// false` as a separate ASSERT line.
///
/// Tests that need custom slot registrations (e.g. chunk 1's alias
/// tests that register OutFlexFrame, or the all_types_schema test)
/// drive initialize/load_script/register_slot_type/build_api inline
/// themselves and skip this helper.
bool setup_role_engine(LuaEngine &engine, RoleHostCore &core,
                       const fs::path &dir,
                       std::unique_ptr<RoleAPIBase> &api_out,
                       RoleKind kind)
{
    const char *required_cb  = nullptr;
    const char *tag          = nullptr;
    bool        register_in  = false;
    bool        register_out = false;
    switch (kind)
    {
        case RoleKind::Producer:
            required_cb = "on_produce"; tag = "prod";
            register_out = true;                       break;
        case RoleKind::Consumer:
            required_cb = "on_consume"; tag = "cons";
            register_in = true;                        break;
        case RoleKind::Processor:
            required_cb = "on_process"; tag = "proc";
            register_in = true;  register_out = true;  break;
    }

    if (!engine.initialize("test", &core))
        return false;
    if (!engine.load_script(dir, "init.lua", required_cb))
        return false;
    auto spec = simple_schema();
    if (register_in && !engine.register_slot_type(spec, "InSlotFrame", "aligned"))
        return false;
    if (register_out && !engine.register_slot_type(spec, "OutSlotFrame", "aligned"))
        return false;
    api_out = make_api(core, tag);
    return engine.build_api(*api_out);
}

/// Unified script-worker template: writes `init.lua` with `lua_source`,
/// sets up a LuaEngine for the given role, calls `body(engine, core)`,
/// finalizes. Replaces the earlier chunk-specific produce/consume/
/// process_worker_with_script trio whose logic was identical modulo
/// the RoleKind.
template <typename F>
int script_worker(const std::string &dir, const char *scenario_name,
                  RoleKind kind, const std::string &lua_source, F &&body)
{
    return run_gtest_worker(
        [&]()
        {
            const fs::path script_dir(dir);
            write_script(script_dir, lua_source);

            RoleHostCore core;
            LuaEngine    engine;
            std::unique_ptr<RoleAPIBase> api;
            ASSERT_TRUE(setup_role_engine(engine, core, script_dir, api, kind));

            body(engine, core);

            engine.finalize();
        },
        scenario_name, Logger::GetLifecycleModule());
}

// Thin per-role delegates — no logic, just fill in the RoleKind so the
// 14 existing call sites in chunks 2-5 remain unchanged through this
// refactor.  All three wrappers forward identically; the per-role
// behaviour lives in setup_role_engine above.
template <typename F>
int produce_worker_with_script(const std::string &dir, const char *name,
                               const std::string &lua_source, F &&body)
{
    return script_worker(dir, name, RoleKind::Producer,
                         lua_source, std::forward<F>(body));
}
template <typename F>
int consume_worker_with_script(const std::string &dir, const char *name,
                               const std::string &lua_source, F &&body)
{
    return script_worker(dir, name, RoleKind::Consumer,
                         lua_source, std::forward<F>(body));
}
template <typename F>
int process_worker_with_script(const std::string &dir, const char *name,
                               const std::string &lua_source, F &&body)
{
    return script_worker(dir, name, RoleKind::Processor,
                         lua_source, std::forward<F>(body));
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
            ASSERT_TRUE(setup_role_engine(engine, core, script_dir, api,
                                          RoleKind::Producer));

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
    // Design enforcement test — verifies that the library implements rx
    // read-only as a LOUD failure, not a silent no-op. Source-confirmed:
    //
    //   src/scripting/lua_engine.cpp:697-699
    //     register_slot_type("InSlotFrame", ...) →
    //     safe_cache(ref_in_slot_readonly_, "InSlotFrame", readonly=true)
    //
    //   src/scripting/lua_state.cpp:285,291
    //     cache_ffi_typeof(name, readonly=true) →
    //     ffi.typeof("InSlotFrame const*")
    //
    //   src/scripting/lua_engine.cpp:843
    //     push_slot_view_cached(rx.slot, ref_in_slot_readonly_)
    //
    // LuaJIT's ffi raises a runtime Lua error when Lua code tries to
    // write through a `const*` cdata.  The engine's pcall path catches
    // it, increments script_error_count, and returns InvokeResult::Error.
    //
    // Design rationale for LOUD failure (why silent no-op would be
    // wrong): a user whose buggy script does `rx.slot.value = X` wants
    // to know immediately — silent no-op would let the broken script
    // run, appear healthy, and corrupt downstream logic that depended
    // on the "modified" value.  The library is correct to reject the
    // write explicitly; this test enforces that design choice so a
    // regression (engine stops using `const*` cast, or ffi stops
    // raising, or catch path suppresses the error) fails the test.
    return consume_worker_with_script(
        dir, "lua_engine::invoke_consume_rx_slot_is_read_only",
        R"(
            function on_consume(rx, msgs, api)
                -- Attempt to mutate rx.slot. The library's contract is
                -- that this MUST raise a Lua error, which propagates to
                -- InvokeResult::Error via the engine's pcall path.
                rx.slot.value = 777.0
                return true  -- unreachable if the write raises
            end
        )",
        [](pylabhub::scripting::LuaEngine &engine,
           pylabhub::scripting::RoleHostCore & /*core*/) {
            float data = 99.5f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            auto result = engine.invoke_consume(
                pylabhub::scripting::InvokeRx{&data, sizeof(data)}, msgs);

            // Four coupled assertions that together pin the design:
            //
            //  (a) buf unchanged — the read-only invariant
            EXPECT_FLOAT_EQ(data, 99.5f)
                << "rx.slot write must NOT propagate to the underlying "
                   "C buffer. data==777.0 here would indicate the engine "
                   "dropped the `const*` cast in register_slot_type.";

            //  (b) the library signals Error — loud, observable result
            EXPECT_EQ(result, pylabhub::scripting::InvokeResult::Error)
                << "rx write must surface as InvokeResult::Error, not "
                   "Discard or Commit. A silent no-op (Commit here) "
                   "would let broken scripts appear healthy while silently "
                   "producing incorrect data downstream.";

            //  (c) the script-error counter tracks the raise — so
            //      production metrics can observe the bug rate
            EXPECT_EQ(engine.script_error_count(), 1u)
                << "the raised Lua error must be counted so role hosts' "
                   "metrics reflect buggy scripts.";

            //  (d) the ERROR log is the third observable channel; the
            //      parent's expected_error_substrings enforces its
            //      substring, so no C++-side assertion is needed here.
        });
}

// ── Messages (chunk 5) ──────────────────────────────────────────────────────
//
// Note on the two projection paths (see src/scripting/lua_engine.cpp:1084
// and :1124 for canonical reference):
//   - push_messages_table_       — used by invoke_produce + invoke_process
//                                   data messages keep sender + data fields
//   - push_messages_table_bare_  — used by invoke_consume
//                                   data messages become plain byte strings
//
// Event messages use the same format in both paths (event + flattened
// details fields).

int invoke_produce_receives_messages_event_with_details(const std::string &dir)
{
    // Strengthened over the pre-conversion test.  The original set
    // `m1.details["identity"] = "abc123"` in C++ but the Lua body never
    // verified the details map reached Lua.  That left a gap: the
    // engine could be silently dropping the details map (or inserting
    // it under a different shape) and the test would pass.
    //
    // The strengthened body asserts the details key is PROMOTED to a
    // sibling field on the message table — `msgs[1].identity == "abc123"`
    // — which is the documented projection (see push_messages_table_
    // at lua_engine.cpp:1101).  This pins the "promoted, not nested"
    // contract explicitly.
    //
    // Also adds a multi-key details check on msgs[2] to cover the loop
    // over details.items() in the engine (not just the single-key
    // case).
    return produce_worker_with_script(
        dir, "lua_engine::invoke_produce_receives_messages_event_with_details",
        R"LUA(
            function on_produce(tx, msgs, api)
                -- Event count and names (covered by original test).
                assert(#msgs == 2, "expected 2 messages, got " .. tostring(#msgs))
                assert(msgs[1].event == "consumer_joined",
                       "msgs[1].event: " .. tostring(msgs[1].event))
                assert(msgs[2].event == "channel_closing",
                       "msgs[2].event: " .. tostring(msgs[2].event))

                -- NEW: verify details map promotion (m1).
                assert(msgs[1].identity == "abc123",
                       "msgs[1].identity expected 'abc123', got " ..
                       tostring(msgs[1].identity))

                -- NEW: verify details map promotion (m2) with multiple keys.
                assert(msgs[2].reason == "voluntary",
                       "msgs[2].reason: " .. tostring(msgs[2].reason))
                assert(msgs[2].code == 0,
                       "msgs[2].code: " .. tostring(msgs[2].code))
                return false
            end
        )LUA",
        [](pylabhub::scripting::LuaEngine &engine,
           pylabhub::scripting::RoleHostCore & /*core*/) {
            std::vector<pylabhub::scripting::IncomingMessage> msgs;

            pylabhub::scripting::IncomingMessage m1;
            m1.event = "consumer_joined";
            m1.details["identity"] = "abc123";
            msgs.push_back(std::move(m1));

            pylabhub::scripting::IncomingMessage m2;
            m2.event = "channel_closing";
            m2.details["reason"] = "voluntary";
            m2.details["code"]   = 0;
            msgs.push_back(std::move(m2));

            float buf = 0.0f;
            auto result = engine.invoke_produce(
                pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(result, pylabhub::scripting::InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "Lua asserts must have passed — a non-zero count means "
                   "either #msgs was wrong, the event fields didn't match, "
                   "or details-map promotion is broken";
        });
}

int invoke_produce_receives_messages_empty_vector(const std::string &dir)
{
    // NEW test — edge case not covered by the pre-conversion suite.
    // When C++ passes an empty msgs vector, Lua should see an empty
    // table (not nil, and not a table with stale contents).  This is
    // the trivial zero-length path through push_messages_table_; worth
    // a dedicated test because it's the simplest break-point for a
    // range-iteration bug.
    return produce_worker_with_script(
        dir, "lua_engine::invoke_produce_receives_messages_empty_vector",
        R"LUA(
            function on_produce(tx, msgs, api)
                assert(type(msgs) == "table",
                       "msgs should be a table, got " .. type(msgs))
                assert(#msgs == 0,
                       "msgs should be empty, got #msgs=" .. tostring(#msgs))
                -- Also verify msgs is iterable without error (ipairs on
                -- empty table is valid but has produced NPE-style bugs
                -- in past ffi-table implementations).
                local any = false
                for _, _m in ipairs(msgs) do any = true end
                assert(any == false, "empty msgs should have no entries")
                return false
            end
        )LUA",
        [](pylabhub::scripting::LuaEngine &engine,
           pylabhub::scripting::RoleHostCore & /*core*/) {
            std::vector<pylabhub::scripting::IncomingMessage> msgs;  // empty
            float buf = 0.0f;
            auto result = engine.invoke_produce(
                pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(result, pylabhub::scripting::InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

int invoke_produce_receives_messages_data_message(const std::string &dir)
{
    // NEW test — data-message shape for the producer/processor path
    // (push_messages_table_).  A data message has empty event and
    // populated data/sender fields; Lua receives {sender="<hex>",
    // data="<bytes>"}.  The pre-conversion test never exercised this
    // shape at all, leaving the sender-hex-formatting and
    // data-bytes-lua_pushlstring code paths untested.
    return produce_worker_with_script(
        dir, "lua_engine::invoke_produce_receives_messages_data_message",
        R"LUA(
            function on_produce(tx, msgs, api)
                assert(#msgs == 1, "expected 1 message")
                local m = msgs[1]
                -- Data message: event field must be absent/nil, sender
                -- present as hex, data present as byte string.
                assert(m.event == nil,
                       "data message should have no event, got " ..
                       tostring(m.event))
                assert(type(m.sender) == "string",
                       "sender should be string, got " .. type(m.sender))
                -- The two sender bytes 0xCA 0xFE project to "cafe" (hex).
                assert(m.sender == "cafe",
                       "sender hex expected 'cafe', got " .. tostring(m.sender))
                -- Data payload is the 4 ASCII bytes "pong".
                assert(type(m.data) == "string",
                       "data should be string, got " .. type(m.data))
                assert(m.data == "pong",
                       "data expected 'pong', got " .. tostring(m.data))
                return false
            end
        )LUA",
        [](pylabhub::scripting::LuaEngine &engine,
           pylabhub::scripting::RoleHostCore & /*core*/) {
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            pylabhub::scripting::IncomingMessage m;
            m.event = "";  // data message — empty event triggers bare projection
            // IncomingMessage::sender is std::string (per role_host_core.hpp
            // :48) — treated as an arbitrary byte sequence and rendered to
            // Lua via format_tools::bytes_to_hex. Two bytes 0xCA 0xFE
            // project to hex "cafe".
            m.sender = std::string("\xCA\xFE", 2);
            for (char c : std::string{"pong"})
                m.data.push_back(static_cast<std::byte>(c));
            msgs.push_back(std::move(m));

            float buf = 0.0f;
            auto result = engine.invoke_produce(
                pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(result, pylabhub::scripting::InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "a failing assert here indicates the data-message "
                   "projection differs from the documented shape";
        });
}

int invoke_consume_receives_messages_data_bare_format(const std::string &dir)
{
    // NEW test — consumer-side data-message shape
    // (push_messages_table_bare_). The consumer variant drops the
    // sender field and exposes data as a plain byte string directly at
    // msgs[i] (not a nested table).  That divergence from the producer
    // path is a real production contract documented only in the
    // in-source comment at lua_engine.cpp:1146; this test pins it
    // explicitly.
    return consume_worker_with_script(
        dir, "lua_engine::invoke_consume_receives_messages_data_bare_format",
        R"LUA(
            function on_consume(rx, msgs, api)
                assert(#msgs == 1, "expected 1 message")
                -- Consumer bare format: msgs[1] is the raw byte string,
                -- NOT a table.  This is the key divergence from the
                -- producer/processor path.
                assert(type(msgs[1]) == "string",
                       "consumer data msg should be bare string, got " ..
                       type(msgs[1]))
                assert(msgs[1] == "hello",
                       "data expected 'hello', got " .. tostring(msgs[1]))
                return true
            end
        )LUA",
        [](pylabhub::scripting::LuaEngine &engine,
           pylabhub::scripting::RoleHostCore & /*core*/) {
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            pylabhub::scripting::IncomingMessage m;
            m.event = "";
            for (char c : std::string{"hello"})
                m.data.push_back(static_cast<std::byte>(c));
            msgs.push_back(std::move(m));

            float data = 0.0f;
            auto result = engine.invoke_consume(
                pylabhub::scripting::InvokeRx{&data, sizeof(data)}, msgs);
            EXPECT_EQ(result, pylabhub::scripting::InvokeResult::Commit);
            EXPECT_EQ(engine.script_error_count(), 0u);
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
    // Design enforcement test — the processor's dual-slot code path
    // must enforce the same loud-failure rx read-only contract as the
    // consumer path (see invoke_consume_rx_slot_is_read_only doc block
    // for full source-trace rationale).  invoke_process is a separate
    // engine entry (lua_engine.cpp:899-900 pushes rx via the same
    // ref_in_slot_readonly_ cached ctype as the consumer path uses),
    // so the contract should hold the same way.
    //
    // Because the Lua error is raised on the rx-write line, the body
    // never reaches the subsequent tx-write — so out_data stays at its
    // caller-initialised 0.0f. We assert that explicitly to pin the
    // "raise aborts the callback" sequencing.
    return process_worker_with_script(
        dir, "lua_engine::invoke_process_rx_slot_is_read_only",
        R"(
            function on_process(rx, tx, msgs, api)
                rx.slot.value = 777.0   -- raises; aborts the callback
                tx.slot.value = 3.25    -- unreachable
                return true             -- unreachable
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

            // Four coupled assertions pinning the design contract:
            EXPECT_FLOAT_EQ(in_data, 99.5f)
                << "rx.slot write must NOT propagate to the underlying "
                   "rx buffer.";
            EXPECT_EQ(result, pylabhub::scripting::InvokeResult::Error)
                << "rx write in invoke_process must surface as "
                   "InvokeResult::Error, same loud contract as "
                   "invoke_consume.";
            EXPECT_EQ(engine.script_error_count(), 1u);
            // tx never written because the Lua error aborted the
            // callback on the preceding line. This pins the
            // raise-aborts-callback sequencing.
            EXPECT_FLOAT_EQ(out_data, 0.0f)
                << "the Lua error on the rx write must abort the rest of "
                   "on_process — the tx write on the next line was "
                   "unreachable, so out_data stays at its initial 0.0f.";
        });
}

// ── API closures: introspection + control (chunk 6a) ──────────────────────
//
// L2-scoped api.* closures. Queue-state, band pub/sub, and inbox
// closures are deferred to L3 (see chunk-6a header in the .h file).
//
// Each body runs as a producer worker (script_worker with
// RoleKind::Producer) because the closures under test are role-agnostic
// — uid/name/channel values come from make_api which is producer-tagged
// by default; version_info is role-independent; log/stop/critical_error
// operate on the core shared by all roles.

int api_version_info_returns_json_string(const std::string &dir)
{
    // Strengthened over the pre-conversion test. Original asserted:
    //   type(info) == "string", #info > 10, info contains "{"
    // The last assertion is weak ("{" appears in any JSON including
    // "{{")—it doesn't guarantee the string is valid, complete, or
    // contains useful fields.
    //
    // Strengthened body additionally asserts the string contains
    // closing bracket AND three real key substrings this version of
    // the library definitely emits (verified against
    // src/utils/core/version_registry.cpp:77-84 at the time of
    // writing):
    //
    //   "release"     — the release tag (e.g. "0.1.0a0")
    //   "library"     — library version triple
    //   "script_api_major" — ABI indicator scripts depend on
    //
    // If the library removes any of these keys, the test fails —
    // which is what we want, since scripts using version_info() in
    // the wild key off these exact names.
    return script_worker(
        dir, "lua_engine::api_version_info_returns_json_string",
        RoleKind::Producer,
        R"LUA(
            function on_produce(tx, msgs, api)
                local info = api.version_info()
                assert(type(info) == "string",
                       "version_info should return string, got " .. type(info))
                assert(#info > 10,
                       "version_info unexpectedly short: " .. info)
                assert(info:find("{") ~= nil,
                       "version_info missing '{': " .. info)
                assert(info:find("}") ~= nil,
                       "version_info missing '}': " .. info)
                assert(info:find("release") ~= nil,
                       "version_info missing 'release' key: " .. info)
                assert(info:find("library") ~= nil,
                       "version_info missing 'library' key: " .. info)
                assert(info:find("script_api_major") ~= nil,
                       "version_info missing 'script_api_major' key: " .. info)
                return false
            end
        )LUA",
        [](LuaEngine &engine, RoleHostCore & /*core*/) {
            float buf = 0.0f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            auto result = engine.invoke_produce(
                pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(result, pylabhub::scripting::InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

int api_identity_uid_name_channel(const std::string &dir)
{
    // NEW test. Pins that api.uid(), api.name(), api.channel() read
    // straight through to the RoleAPIBase getters — a simple
    // mis-wiring (e.g. uid() returning name) would silently break
    // role scripts that use these for logging, registration, or
    // channel-scoped work. Values are the ones make_api installs
    // (see setup_role_engine → make_api in this file).
    return script_worker(
        dir, "lua_engine::api_identity_uid_name_channel",
        RoleKind::Producer,
        R"LUA(
            function on_produce(tx, msgs, api)
                assert(api.uid() == "TEST-ENGINE-00000001",
                       "uid: " .. tostring(api.uid()))
                assert(api.name() == "TestEngine",
                       "name: " .. tostring(api.name()))
                assert(api.channel() == "test.channel",
                       "channel: " .. tostring(api.channel()))
                return false
            end
        )LUA",
        [](LuaEngine &engine, RoleHostCore & /*core*/) {
            float buf = 0.0f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            auto result = engine.invoke_produce(
                pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(result, pylabhub::scripting::InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

int api_log_dispatches_levels(const std::string &dir)
{
    // NEW test. Pins the level dispatch in lua_api_log
    // (lua_engine.cpp:1311-1328): "error" → LOGGER_ERROR,
    // "warn"/"warning" → LOGGER_WARN, "debug" → LOGGER_DEBUG,
    // anything else → LOGGER_INFO. The log tag format is
    // "[<log_tag>-lua]"; for this suite log_tag == "test".
    //
    // C++ verifies the three distinct log lines land in stderr with
    // the right levels. "debug" is omitted here because
    // LOGGER_COMPILE_LEVEL=0 in the cmake sets all levels compiled
    // in but runtime may still filter DEBUG; testing the compiled
    // presence is a separate concern from level-dispatch routing.
    return script_worker(
        dir, "lua_engine::api_log_dispatches_levels",
        RoleKind::Producer,
        R"LUA(
            function on_produce(tx, msgs, api)
                api.log("error",   "lua_error_msg")
                api.log("warn",    "lua_warn_msg")
                api.log("warning", "lua_warning_msg")
                api.log("info",    "lua_info_msg")
                api.log("unknown", "lua_unknown_msg")
                return false
            end
        )LUA",
        [](LuaEngine &engine, RoleHostCore & /*core*/) {
            float buf = 0.0f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            auto result = engine.invoke_produce(
                pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(result, pylabhub::scripting::InvokeResult::Discard);
            // The worker doesn't EXPECT on stderr directly — the parent
            // test's expected_error_substrings pins the one ERROR line
            // the engine must emit. Absence of other ERRORs is also
            // verified there by the framework's "every ERROR must be
            // accounted for" clause.
            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

int api_stop_sets_shutdown_requested(const std::string &dir)
{
    // NEW test. api.stop() → core->request_stop() → sets
    // shutdown_requested_ (role_host_core.hpp:206-209). The script
    // host's main loop polls is_shutdown_requested() to exit
    // cleanly; this test pins that the Lua API path correctly
    // reaches the same flag.
    return script_worker(
        dir, "lua_engine::api_stop_sets_shutdown_requested",
        RoleKind::Producer,
        R"LUA(
            function on_produce(tx, msgs, api)
                api.stop()
                return false
            end
        )LUA",
        [](LuaEngine &engine, RoleHostCore &core) {
            // Pre-condition: nothing has set the flag yet.
            EXPECT_FALSE(core.is_shutdown_requested());
            EXPECT_FALSE(core.is_critical_error())
                << "stop() alone must NOT flag a critical error — that's "
                   "only set by set_critical_error()";

            float buf = 0.0f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            auto result = engine.invoke_produce(
                pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(result, pylabhub::scripting::InvokeResult::Discard);

            // Post-condition: Lua api.stop() must have reached the core.
            EXPECT_TRUE(core.is_shutdown_requested());
            // And must NOT have touched the critical-error path.
            EXPECT_FALSE(core.is_critical_error());
            EXPECT_EQ(core.stop_reason_string(), std::string("normal"))
                << "stop_reason stays 'normal' after a plain api.stop() — "
                   "a non-normal reason indicates the wrong path was taken.";
            EXPECT_EQ(engine.script_error_count(), 0u);

            // Engine also logs one INFO line documenting the api.stop()
            // call (lua_engine.cpp:1333). Not asserted here — INFO lines
            // are not in the framework's expected-error channel.
        });
}

int api_critical_error_set_and_read_and_stop_reason(const std::string &dir)
{
    // NEW test. Probes three related closures in one scenario,
    // because their contracts only make sense together:
    //
    //   api.critical_error()      — reads the flag (bool)
    //   api.set_critical_error()  — sets the flag + logs ERROR +
    //                               sets stop_reason=CriticalError +
    //                               sets shutdown_requested=true
    //   api.stop_reason()         — reads stop_reason as string
    //
    // The Lua body does READ-before, SET, READ-after, read-stop-reason,
    // returning true only if the transitions were correct. A regression
    // in any of the three closures (or the core's set_critical_error
    // side-effects) fails this test.
    return script_worker(
        dir, "lua_engine::api_critical_error_set_and_read_and_stop_reason",
        RoleKind::Producer,
        R"LUA(
            function on_produce(tx, msgs, api)
                -- Initial state: flag is false
                assert(api.critical_error() == false,
                       "initial critical_error should be false")
                assert(api.stop_reason() == "normal",
                       "initial stop_reason should be 'normal', got " ..
                       tostring(api.stop_reason()))

                -- Set via Lua API
                api.set_critical_error("deliberate test error")

                -- Read-after: all three post-conditions
                assert(api.critical_error() == true,
                       "after set_critical_error, flag should be true")
                assert(api.stop_reason() == "critical_error",
                       "after set_critical_error, stop_reason should be "
                       .. "'critical_error', got " ..
                       tostring(api.stop_reason()))
                return false
            end
        )LUA",
        [](LuaEngine &engine, RoleHostCore &core) {
            // Pre-condition: clean state
            EXPECT_FALSE(core.is_critical_error());
            EXPECT_FALSE(core.is_shutdown_requested());

            float buf = 0.0f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            auto result = engine.invoke_produce(
                pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(result, pylabhub::scripting::InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "Lua asserts must have all passed; a non-zero count "
                   "here means one of the post-set reads was wrong";

            // set_critical_error's documented three side effects
            // (role_host_core.hpp:216-222):
            EXPECT_TRUE(core.is_critical_error());
            EXPECT_TRUE(core.is_shutdown_requested())
                << "set_critical_error must ALSO set shutdown_requested "
                   "so the main loop exits — see role_host_core.hpp:221.";
            EXPECT_EQ(core.stop_reason_string(),
                      std::string("critical_error"));
        });
}

int api_stop_reason_reflects_all_enum_values(const std::string &dir)
{
    // NEW test — closes a real coverage gap and subsumes two V2 tests.
    //
    // Chunk 6a's ApiCriticalError_SetAndReadAndStopReason covers only the
    // Lua-set-Lua-read path for Normal + CriticalError.  Two V2 tests
    // covered PeerDead and CriticalError injected from C++ (via
    // core.set_stop_reason(...)) but HubDead was never tested at all.
    // This worker runs one engine and loops over all four StopReason
    // enum values, verifying the string mapping that
    // stop_reason_string() implements (role_host_core.hpp:270-279):
    //
    //   Normal        → "normal"
    //   PeerDead      → "peer_dead"
    //   HubDead       → "hub_dead"
    //   CriticalError → "critical_error"
    //
    // Communication Lua→C++ is via set_shared_data: the Lua script
    // writes api.stop_reason() into core.shared_data under a well-known
    // key, the C++ driver reads it back and compares. That avoids
    // needing 4 separate Lua scripts or a Lua-side table of expected
    // strings.
    return script_worker(
        dir, "lua_engine::api_stop_reason_reflects_all_enum_values",
        RoleKind::Producer,
        R"LUA(
            function on_produce(tx, msgs, api)
                api.set_shared_data("observed_reason", api.stop_reason())
                return false
            end
        )LUA",
        [](LuaEngine &engine, RoleHostCore &core) {
            struct Case { RoleHostCore::StopReason r; const char *expected; };
            const Case cases[] = {
                {RoleHostCore::StopReason::Normal,        "normal"},
                {RoleHostCore::StopReason::PeerDead,      "peer_dead"},
                {RoleHostCore::StopReason::HubDead,       "hub_dead"},
                {RoleHostCore::StopReason::CriticalError, "critical_error"},
            };

            for (const auto &c : cases)
            {
                core.set_stop_reason(c.r);
                float buf = 0.0f;
                std::vector<pylabhub::scripting::IncomingMessage> msgs;
                auto result = engine.invoke_produce(
                    pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
                EXPECT_EQ(result, pylabhub::scripting::InvokeResult::Discard);
                EXPECT_EQ(engine.script_error_count(), 0u)
                    << "iter expected=" << c.expected;

                auto v = core.get_shared_data("observed_reason");
                ASSERT_TRUE(v.has_value())
                    << "Lua must have written the key; missing value means "
                       "set_shared_data did not reach the core";
                ASSERT_TRUE(std::holds_alternative<std::string>(*v))
                    << "Lua must have written a string (the return of "
                       "api.stop_reason()); wrong variant means the closure "
                       "returned a non-string type";
                EXPECT_EQ(std::get<std::string>(*v), std::string(c.expected))
                    << "api.stop_reason() must reflect set_stop_reason("
                    << static_cast<int>(c.r)
                    << ") — wrong string means stop_reason_string()'s switch "
                       "lost a case or Lua closure read a stale cached value";
            }
        });
}

// ── API closures: custom metrics (chunk 6b) ────────────────────────────────
//
// All bodies below run as Producer workers. custom metrics route
// through RoleHostCore's custom_metrics_ map independent of role.
// Each worker uses the `script_worker(RoleKind::Producer, ...)`
// helper and asserts from inside the Lua callback (via
// api.metrics()) — so the tests pin the full report→read round-trip
// through the Lua closure surface.

int api_report_metric_appears_under_custom(const std::string &dir)
{
    // Strengthened over the V2 test.  V2 asserted m.custom.k ==
    // expected; this version additionally pins that the key lands
    // ONLY under m.custom (not at the top level of m and not under
    // any other sub-group).  A regression where report_metric
    // accidentally promoted keys to the top of m (shadowing the
    // loop/role sub-groups) would slip past the weaker V2 check.
    return script_worker(
        dir, "lua_engine::api_report_metric_appears_under_custom",
        RoleKind::Producer,
        R"LUA(
            function on_produce(tx, msgs, api)
                api.report_metric("latency_ms", 42.5)
                api.report_metric("throughput", 100)

                local m = api.metrics()
                assert(type(m.custom) == "table",
                       "custom group must exist after report_metric")
                assert(m.custom.latency_ms == 42.5,
                       "m.custom.latency_ms expected 42.5, got "
                       .. tostring(m.custom.latency_ms))
                assert(m.custom.throughput == 100,
                       "m.custom.throughput expected 100, got "
                       .. tostring(m.custom.throughput))

                -- NEW: strengthening — key must NOT appear at top level.
                assert(m.latency_ms == nil,
                       "report_metric must NOT promote keys to top "
                       .. "level of m; got m.latency_ms="
                       .. tostring(m.latency_ms))
                assert(m.throughput == nil,
                       "report_metric must NOT promote keys to top "
                       .. "level of m; got m.throughput="
                       .. tostring(m.throughput))
                return false
            end
        )LUA",
        [](LuaEngine &engine, RoleHostCore & /*core*/) {
            float buf = 0.0f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            auto result = engine.invoke_produce(
                pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(result, pylabhub::scripting::InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

int api_report_metric_overwrite_same_key(const std::string &dir)
{
    return script_worker(
        dir, "lua_engine::api_report_metric_overwrite_same_key",
        RoleKind::Producer,
        R"LUA(
            function on_produce(tx, msgs, api)
                api.report_metric("x", 1)
                api.report_metric("x", 2)
                local m = api.metrics()
                assert(type(m.custom) == "table",
                       "custom group must exist")
                assert(m.custom.x == 2,
                       "second report_metric must overwrite; "
                       .. "expected 2, got " .. tostring(m.custom.x))
                return false
            end
        )LUA",
        [](LuaEngine &engine, RoleHostCore & /*core*/) {
            float buf = 0.0f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            auto result = engine.invoke_produce(
                pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(result, pylabhub::scripting::InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

int api_report_metric_zero_value_preserved(const std::string &dir)
{
    // 0.0 is the default value of a missing table lookup in some
    // implementations; this test pins that a reported 0 is visibly
    // PRESENT (not nil-confused) in m.custom.
    return script_worker(
        dir, "lua_engine::api_report_metric_zero_value_preserved",
        RoleKind::Producer,
        R"LUA(
            function on_produce(tx, msgs, api)
                api.report_metric("x", 0.0)
                local m = api.metrics()
                assert(type(m.custom) == "table",
                       "custom group must exist even for 0.0")
                assert(m.custom.x ~= nil,
                       "0.0 must be present, not nil-confused; "
                       .. "m.custom.x was nil")
                assert(m.custom.x == 0,
                       "expected 0, got " .. tostring(m.custom.x))
                return false
            end
        )LUA",
        [](LuaEngine &engine, RoleHostCore & /*core*/) {
            float buf = 0.0f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            auto result = engine.invoke_produce(
                pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(result, pylabhub::scripting::InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

int api_report_metrics_batch_accepts_table(const std::string &dir)
{
    // Strengthened over V2.  V2 used all-positive integer-like values
    // (a=1.0, b=2.0, c=3.0).  This body mixes small int, negative
    // int, and fractional double to exercise the luaL_checknumber
    // coercion path and pin that sign is preserved.
    return script_worker(
        dir, "lua_engine::api_report_metrics_batch_accepts_table",
        RoleKind::Producer,
        R"LUA(
            function on_produce(tx, msgs, api)
                api.report_metrics({a = 1, b = -7, c = 3.5})

                local m = api.metrics()
                assert(type(m.custom) == "table", "custom must exist")
                assert(m.custom.a == 1,
                       "a expected 1, got " .. tostring(m.custom.a))
                assert(m.custom.b == -7,
                       "b expected -7 (negative preserved), got "
                       .. tostring(m.custom.b))
                assert(m.custom.c == 3.5,
                       "c expected 3.5, got " .. tostring(m.custom.c))
                return false
            end
        )LUA",
        [](LuaEngine &engine, RoleHostCore & /*core*/) {
            float buf = 0.0f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            auto result = engine.invoke_produce(
                pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(result, pylabhub::scripting::InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

int api_report_metrics_non_table_arg_is_error(const std::string &dir)
{
    // Strengthened: V2 only asserted script_error_count >= 1.
    // Source-confirmed: lua_api_report_metrics
    // (src/scripting/lua_engine.cpp:1634) calls
    // luaL_checktype(L, 1, LUA_TTABLE) which raises a Lua error on
    // wrong type; the engine's pcall path translates that to
    // InvokeResult::Error and increments script_error_count.  This
    // body pins BOTH observable effects (result == Error AND
    // script_error_count == 1) so a regression where the engine
    // silently accepts a non-table (e.g. luaL_checktype replaced by
    // a permissive cast) would fail the result-code assertion.
    return script_worker(
        dir, "lua_engine::api_report_metrics_non_table_arg_is_error",
        RoleKind::Producer,
        R"LUA(
            function on_produce(tx, msgs, api)
                api.report_metrics(42)  -- wrong type, must raise
                return false            -- unreachable if raise fires
            end
        )LUA",
        [](LuaEngine &engine, RoleHostCore & /*core*/) {
            float buf = 0.0f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            auto result = engine.invoke_produce(
                pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(result, pylabhub::scripting::InvokeResult::Error)
                << "non-table arg must surface as InvokeResult::Error, "
                   "not Discard/Commit — loud-failure contract";
            EXPECT_EQ(engine.script_error_count(), 1u);
        });
}

int api_clear_custom_metrics_empties_and_allows_rewrite(const std::string &dir)
{
    // Strengthened over V2.  V2 only verified that clear empties
    // m.custom.  This body additionally verifies that AFTER clear,
    // a subsequent report_metric re-populates the custom table from
    // scratch — closing a real regression class where clear might
    // leave the table in an unusable state (e.g. null pointer, or
    // stale "cleared" flag blocking future reports).
    return script_worker(
        dir, "lua_engine::api_clear_custom_metrics_empties_and_allows_rewrite",
        RoleKind::Producer,
        R"LUA(
            function on_produce(tx, msgs, api)
                api.report_metric("a", 1)
                api.report_metric("b", 2)
                local before = api.metrics()
                assert(before.custom.a == 1, "pre-clear a==1")
                assert(before.custom.b == 2, "pre-clear b==2")

                api.clear_custom_metrics()
                local cleared = api.metrics()
                -- Source-confirmed (lua_engine.cpp:1790-1803): the
                -- engine only emits "custom" when cm.empty() is
                -- false.  After clear the snapshot is empty → the
                -- custom FIELD is absent (not an empty table).  Pin
                -- that exact shape — a regression that emits an
                -- empty custom table instead is a legitimate
                -- behaviour change worth catching.
                assert(cleared.custom == nil,
                       "post-clear: m.custom must be absent (nil), "
                       .. "NOT an empty table.  got "
                       .. type(cleared.custom))

                -- NEW strengthening: subsequent report must work.
                api.report_metric("c", 99)
                local after = api.metrics()
                assert(type(after.custom) == "table",
                       "custom must be re-creatable post-clear")
                assert(after.custom.c == 99,
                       "post-clear report_metric must work; "
                       .. "c expected 99, got "
                       .. tostring(after.custom.c))
                assert(after.custom.a == nil,
                       "cleared key 'a' must not reappear")
                return false
            end
        )LUA",
        [](LuaEngine &engine, RoleHostCore & /*core*/) {
            float buf = 0.0f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            auto result = engine.invoke_produce(
                pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(result, pylabhub::scripting::InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

// ── API closures: shared data (chunk 6c) ───────────────────────────────────
//
// set_shared_data dispatch (source-confirmed at
// src/scripting/lua_engine.cpp:1397-1417):
//   bool Lua value     → bool variant
//   whole number       → int64_t variant
//   fractional number  → double variant
//   string             → std::string variant
//   nil or other type  → remove_shared_data(key)  (NOT an error)
//
// get_shared_data returns the value under the variant's native Lua
// type, or nil if the key is missing.

int api_shared_data_round_trip_all_variant_types(const std::string &dir)
{
    // Strengthened over V2 SharedData_SetAndGetFromScript.  V2 set
    // four values from Lua and read them back via C++
    // core.get_shared_data only.  This body ALSO reads each value
    // back via Lua api.get_shared_data in the same callback — so
    // the Lua→C++ set path AND the C++→Lua get path for all four
    // variant types are both exercised in one round-trip.  A
    // regression on either direction (e.g. get_shared_data variant
    // visitor drops a branch) fails here.
    return script_worker(
        dir, "lua_engine::api_shared_data_round_trip_all_variant_types",
        RoleKind::Producer,
        R"LUA(
            function on_produce(tx, msgs, api)
                api.set_shared_data("counter", 42)      -- int64 branch
                api.set_shared_data("label",   "hello") -- string branch
                api.set_shared_data("flag",    true)    -- bool branch
                api.set_shared_data("ratio",   3.14)    -- double branch

                -- NEW: read back via Lua api.get_shared_data on the
                -- SAME invocation — pins the Lua read path, not just
                -- the C++-side check V2 relied on.
                local c = api.get_shared_data("counter")
                assert(c == 42,
                       "counter roundtrip: expected 42, got "
                       .. tostring(c) .. " (type=" .. type(c) .. ")")
                assert(type(c) == "number",
                       "counter should be number, got " .. type(c))

                local l = api.get_shared_data("label")
                assert(l == "hello",
                       "label: expected 'hello', got " .. tostring(l))
                assert(type(l) == "string",
                       "label should be string, got " .. type(l))

                local f = api.get_shared_data("flag")
                assert(f == true,
                       "flag: expected true, got " .. tostring(f))
                assert(type(f) == "boolean",
                       "flag should be boolean, got " .. type(f))

                local r = api.get_shared_data("ratio")
                assert(math.abs(r - 3.14) < 1e-9,
                       "ratio: expected 3.14, got " .. tostring(r))
                assert(type(r) == "number",
                       "ratio should be number, got " .. type(r))

                return false
            end
        )LUA",
        [](LuaEngine &engine, RoleHostCore &core) {
            float buf = 0.0f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            auto result = engine.invoke_produce(
                pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(result, pylabhub::scripting::InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 0u);

            // Additionally verify the underlying core variant types
            // are what the Lua→C++ set path promised
            // (lua_engine.cpp:1401-1413):
            //   whole number → int64_t
            //   string       → std::string
            //   bool         → bool
            //   fractional   → double
            auto c = core.get_shared_data("counter");
            ASSERT_TRUE(c.has_value());
            EXPECT_TRUE(std::holds_alternative<int64_t>(*c))
                << "42 must be stored as int64 (whole-number branch)";
            EXPECT_EQ(std::get<int64_t>(*c), 42);

            auto l = core.get_shared_data("label");
            ASSERT_TRUE(l.has_value());
            EXPECT_TRUE(std::holds_alternative<std::string>(*l));
            EXPECT_EQ(std::get<std::string>(*l), "hello");

            auto f = core.get_shared_data("flag");
            ASSERT_TRUE(f.has_value());
            EXPECT_TRUE(std::holds_alternative<bool>(*f));
            EXPECT_TRUE(std::get<bool>(*f));

            auto r = core.get_shared_data("ratio");
            ASSERT_TRUE(r.has_value());
            EXPECT_TRUE(std::holds_alternative<double>(*r))
                << "3.14 must be stored as double (fractional branch)";
            EXPECT_DOUBLE_EQ(std::get<double>(*r), 3.14);
        });
}

int api_shared_data_get_missing_key_returns_nil(const std::string &dir)
{
    return script_worker(
        dir, "lua_engine::api_shared_data_get_missing_key_returns_nil",
        RoleKind::Producer,
        R"LUA(
            function on_produce(tx, msgs, api)
                local v = api.get_shared_data("nonexistent")
                assert(v == nil,
                       "missing key must return nil, got " .. tostring(v))
                return false
            end
        )LUA",
        [](LuaEngine &engine, RoleHostCore & /*core*/) {
            float buf = 0.0f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            auto result = engine.invoke_produce(
                pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(result, pylabhub::scripting::InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

int api_shared_data_nil_removes_key(const std::string &dir)
{
    // Strengthened: reads back via Lua api.get_shared_data after
    // removal (V2 only checked via C++ core).  Also adds a pre-set
    // assertion so the test distinguishes "removal worked" from
    // "key was never there".
    return script_worker(
        dir, "lua_engine::api_shared_data_nil_removes_key",
        RoleKind::Producer,
        R"LUA(
            function on_produce(tx, msgs, api)
                api.set_shared_data("temp", 99)
                -- pre-condition: set worked
                assert(api.get_shared_data("temp") == 99,
                       "pre-condition: temp must be 99 after set")

                api.set_shared_data("temp", nil)
                -- post-condition: key removed (Lua side)
                assert(api.get_shared_data("temp") == nil,
                       "post: temp must be nil after set nil; got "
                       .. tostring(api.get_shared_data("temp")))
                return false
            end
        )LUA",
        [](LuaEngine &engine, RoleHostCore &core) {
            float buf = 0.0f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            auto result = engine.invoke_produce(
                pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(result, pylabhub::scripting::InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 0u);
            // C++-side: key truly removed, not just set to a "nil
            // sentinel".  has_value()==false is the contract.
            EXPECT_FALSE(core.get_shared_data("temp").has_value())
                << "set_shared_data(key, nil) must remove the key "
                   "(calls core.remove_shared_data at "
                   "lua_engine.cpp:1415) — not store a nil/sentinel";
        });
}

int api_shared_data_overwrite_changes_type(const std::string &dir)
{
    // NEW gap-fill.  V2 covered type-preserving overwrite (bool to
    // bool, string to string, etc.) implicitly via the round-trip
    // test but never CROSS-type overwrite.  This body sets an int64,
    // then sets the same key to a string, and asserts the stored
    // variant flipped to the new type (a regression where the engine
    // refused to change type or silently coerced would fail here).
    return script_worker(
        dir, "lua_engine::api_shared_data_overwrite_changes_type",
        RoleKind::Producer,
        R"LUA(
            function on_produce(tx, msgs, api)
                api.set_shared_data("k", 42)           -- int64
                assert(api.get_shared_data("k") == 42,
                       "pre: k must be 42")

                api.set_shared_data("k", "now a string")  -- string
                local v = api.get_shared_data("k")
                assert(v == "now a string",
                       "post: k must be 'now a string', got "
                       .. tostring(v))
                assert(type(v) == "string",
                       "post: type must be string, got " .. type(v))
                return false
            end
        )LUA",
        [](LuaEngine &engine, RoleHostCore &core) {
            float buf = 0.0f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            auto result = engine.invoke_produce(
                pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(result, pylabhub::scripting::InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 0u);

            // The underlying variant must now hold std::string —
            // not still int64_t.
            auto v = core.get_shared_data("k");
            ASSERT_TRUE(v.has_value());
            EXPECT_TRUE(std::holds_alternative<std::string>(*v))
                << "cross-type overwrite must update the variant "
                   "alternative; still holding int64_t means the "
                   "engine refused the overwrite or kept the old type";
            EXPECT_EQ(std::get<std::string>(*v), "now a string");
        });
}

int api_shared_data_overwrite_changes_value_same_type(const std::string &dir)
{
    // NEW gap-fill.  V2 tested set-then-get (one value) and
    // set-then-nil (remove).  It never tested in-place overwrite
    // with the same type — a bug class where the second set is
    // silently a no-op (e.g. "key already exists" guard).
    return script_worker(
        dir,
        "lua_engine::api_shared_data_overwrite_changes_value_same_type",
        RoleKind::Producer,
        R"LUA(
            function on_produce(tx, msgs, api)
                api.set_shared_data("n", 1)
                api.set_shared_data("n", 2)
                assert(api.get_shared_data("n") == 2,
                       "same-type overwrite: expected 2, got "
                       .. tostring(api.get_shared_data("n")))
                return false
            end
        )LUA",
        [](LuaEngine &engine, RoleHostCore &core) {
            float buf = 0.0f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            auto result = engine.invoke_produce(
                pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(result, pylabhub::scripting::InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 0u);

            auto v = core.get_shared_data("n");
            ASSERT_TRUE(v.has_value());
            EXPECT_TRUE(std::holds_alternative<int64_t>(*v));
            EXPECT_EQ(std::get<int64_t>(*v), 2)
                << "second set must overwrite the first — 1 still "
                   "present means set became a no-op";
        });
}

// ── Error handling: runtime error surfacing (chunk 7a) ─────────────────────

int invoke_multiple_errors_count_accumulates(const std::string &dir)
{
    // Strengthened over V2 MultipleErrors_CountAccumulates.  V2 only
    // asserted the final count after 5 invocations.  This body
    // additionally asserts each invoke_produce returns Error (not
    // Commit / Discard), and asserts the count increments
    // MONOTONICALLY across the loop (catches a regression where
    // script_error_count is only updated at finalize, or increments
    // by more than one per error).
    return produce_worker_with_script(
        dir, "lua_engine::invoke_multiple_errors_count_accumulates",
        R"LUA(
            function on_produce(tx, msgs, api)
                error("oops")
            end
        )LUA",
        [](LuaEngine &engine, RoleHostCore & /*core*/) {
            float buf = 0.0f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            EXPECT_EQ(engine.script_error_count(), 0u);

            for (size_t i = 0; i < 5; ++i)
            {
                auto r = engine.invoke_produce(
                    pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
                EXPECT_EQ(r, pylabhub::scripting::InvokeResult::Error)
                    << "iteration " << i
                    << ": raised error must surface as Error, not Discard";
                EXPECT_EQ(engine.script_error_count(), i + 1)
                    << "iteration " << i << ": count must increment by 1 "
                    << "per invocation, monotonically";
            }
        });
}

int invoke_produce_wrong_return_type_is_error(const std::string &dir)
{
    // Strengthened over V2.  V2 asserted result==Error + count==1.
    // This body additionally invokes a SECOND time to prove the
    // engine doesn't enter a broken state after the first error —
    // the 2nd invocation should surface Error the same way (not
    // crash, not hang, not silently succeed).  This pins the "one
    // bad script iteration does not brick the engine" contract.
    // Note: we cannot use eval() to redefine on_produce because the
    // engine caches the Lua ref at load_script time
    // (lua_engine.cpp: ref_on_produce_) — a subsequent eval defines
    // a new global but the cached ref still points at the original.
    return produce_worker_with_script(
        dir, "lua_engine::invoke_produce_wrong_return_type_is_error",
        R"LUA(
            function on_produce(tx, msgs, api)
                return 42  -- number instead of true/false
            end
        )LUA",
        [](LuaEngine &engine, RoleHostCore & /*core*/) {
            float buf = 0.0f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            auto r1 = engine.invoke_produce(
                pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(r1, pylabhub::scripting::InvokeResult::Error)
                << "returning a number must surface as Error";
            EXPECT_EQ(engine.script_error_count(), 1u);

            auto r2 = engine.invoke_produce(
                pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(r2, pylabhub::scripting::InvokeResult::Error)
                << "second invocation must also cleanly surface Error — "
                   "engine not entering a broken state after the first";
            EXPECT_EQ(engine.script_error_count(), 2u);
        });
}

int invoke_produce_wrong_return_string_is_error(const std::string &dir)
{
    // Same strengthening as wrong_return_type.  Covers the string
    // branch of the return-value type dispatch which is handled
    // separately from the number branch in the engine (different
    // lua_type case).
    return produce_worker_with_script(
        dir, "lua_engine::invoke_produce_wrong_return_string_is_error",
        R"LUA(
            function on_produce(tx, msgs, api)
                return "ok"  -- string instead of true/false
            end
        )LUA",
        [](LuaEngine &engine, RoleHostCore & /*core*/) {
            float buf = 0.0f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            auto r1 = engine.invoke_produce(
                pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(r1, pylabhub::scripting::InvokeResult::Error);
            EXPECT_EQ(engine.script_error_count(), 1u);

            auto r2 = engine.invoke_produce(
                pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(r2, pylabhub::scripting::InvokeResult::Error)
                << "second invocation must also surface Error cleanly";
            EXPECT_EQ(engine.script_error_count(), 2u);
        });
}

int invoke_produce_stop_on_script_error_sets_shutdown(const std::string &dir)
{
    // Strengthened over V2 StopOnScriptError_SetsShutdownOnError.
    // V2 asserted post-error is_shutdown_requested()==true.  This
    // body additionally pins:
    //   (a) is_critical_error() stays FALSE — stop_on_script_error is
    //       a controlled shutdown, NOT a critical error.  Distinct
    //       semantics: critical implies restart, controlled does not.
    //   (b) stop_reason stays "normal" (not "critical_error").
    //   (c) result is Error (V2 already pinned this).
    //
    // Custom setup inline (doesn't use setup_role_engine) because
    // make_api sets set_stop_on_script_error(false) by default and
    // we need true for this test.
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir, R"LUA(
                function on_produce(tx, msgs, api)
                    error("intentional error")
                end
            )LUA");

            RoleHostCore core;
            LuaEngine engine;
            ASSERT_TRUE(engine.initialize("test", &core));
            ASSERT_TRUE(engine.load_script(script_dir, "init.lua", "on_produce"));

            auto spec = simple_schema();
            ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame",
                                                   "aligned"));

            auto api = make_api(core);
            api->set_stop_on_script_error(true);  // ← key difference from default
            ASSERT_TRUE(engine.build_api(*api));

            EXPECT_FALSE(core.is_shutdown_requested());
            EXPECT_FALSE(core.is_critical_error());
            EXPECT_EQ(core.stop_reason_string(), "normal");

            float buf = 0.0f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            auto r = engine.invoke_produce(
                pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);

            EXPECT_EQ(r, pylabhub::scripting::InvokeResult::Error);
            EXPECT_TRUE(core.is_shutdown_requested())
                << "stop_on_script_error=true must flip shutdown_requested";
            EXPECT_FALSE(core.is_critical_error())
                << "stop_on_script_error is a controlled stop, NOT critical "
                   "— critical_error must remain false so the parent role "
                   "host doesn't treat this as a restart-worthy failure";
            EXPECT_EQ(core.stop_reason_string(), "normal")
                << "stop_reason stays 'normal' — the shutdown path here is "
                   "explicit user policy, not the StopReason::CriticalError "
                   "enum value";

            engine.finalize();
        },
        "lua_engine::invoke_produce_stop_on_script_error_sets_shutdown",
        Logger::GetLifecycleModule());
}

int invoke_on_init_or_stop_script_error_accumulates(const std::string &dir)
{
    // MERGED from V2 InvokeOnInit_ScriptError + InvokeOnStop_ScriptError.
    // The two V2 tests were nearly identical structurally — same
    // mechanism exercised via two different engine entry points.
    // Merged body additionally asserts the count accumulates across
    // BOTH lifecycle stages (0 → 1 after on_init error → 2 after
    // on_stop error), pinning the "script_error_count is engine-wide,
    // not per-callback" contract that neither V2 test covered.
    return produce_worker_with_script(
        dir, "lua_engine::invoke_on_init_or_stop_script_error_accumulates",
        R"LUA(
            function on_produce(tx, msgs, api) return false end
            function on_init(api)
                error("init failed")
            end
            function on_stop(api)
                error("stop failed")
            end
        )LUA",
        [](LuaEngine &engine, RoleHostCore & /*core*/) {
            EXPECT_EQ(engine.script_error_count(), 0u);
            engine.invoke_on_init();
            EXPECT_EQ(engine.script_error_count(), 1u)
                << "on_init error must bump count";
            engine.invoke_on_stop();
            EXPECT_EQ(engine.script_error_count(), 2u)
                << "on_stop error must bump count again — script_error_count "
                   "is engine-wide, not per-callback";
        });
}

int invoke_on_inbox_script_error_increments_count(const std::string &dir)
{
    // Strengthened over V2.  V2 asserted count >= 1 (loose); this
    // body pins count == 1 and also pins that the inbox typed data
    // REACHED Lua before the error was raised — by having the
    // callback briefly touch msg.slot before raising, then observing
    // a side effect via custom_metrics.
    //
    // Inline setup because InboxFrame must be registered in addition
    // to OutSlotFrame and setup_role_engine doesn't handle inbox.
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir, R"LUA(
                function on_produce(tx, msgs, api) return false end
                function on_inbox(msg, api)
                    -- Pin that msg.{data,sender_uid,seq} reached Lua
                    -- by reporting a metric before raising.  msg
                    -- fields per lua_engine.cpp:974-986: data (cdata
                    -- or nil), sender_uid (string), seq (integer).
                    if msg.data ~= nil
                       and msg.sender_uid == "SENDER-00000001"
                       and msg.seq == 1 then
                        api.report_metric("inbox_reached", 1)
                    end
                    error("inbox failed")
                end
            )LUA");

            RoleHostCore core;
            LuaEngine engine;
            ASSERT_TRUE(engine.initialize("test", &core));
            ASSERT_TRUE(engine.load_script(script_dir, "init.lua",
                                           "on_produce"));

            auto spec = simple_schema();
            ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame",
                                                   "aligned"));
            ASSERT_TRUE(engine.register_slot_type(spec, "InboxFrame",
                                                   "aligned"));

            auto api = make_api(core);
            ASSERT_TRUE(engine.build_api(*api));

            EXPECT_EQ(engine.script_error_count(), 0u);

            float inbox_data = 1.0f;
            engine.invoke_on_inbox({&inbox_data, sizeof(inbox_data),
                                    "SENDER-00000001", 1});

            EXPECT_EQ(engine.script_error_count(), 1u)
                << "on_inbox error must increment count by exactly 1";

            auto cm = core.custom_metrics_snapshot();
            ASSERT_EQ(cm.count("inbox_reached"), 1u)
                << "msg.slot must have reached Lua before the error — "
                   "missing metric means the inbox frame projection "
                   "raised before reaching user code";
            EXPECT_DOUBLE_EQ(cm["inbox_reached"], 1.0);

            engine.finalize();
        },
        "lua_engine::invoke_on_inbox_script_error_increments_count",
        Logger::GetLifecycleModule());
}

int eval_syntax_error_returns_script_error(const std::string &dir)
{
    // Strengthened over V2 Eval_SyntaxError_ReturnsScriptError.
    // V2 asserted only status == ScriptError.  This body also
    // asserts (a) script_error_count increments, (b) engine is
    // still usable after (can eval a valid expression).  The eval
    // error path is a distinct code path from invoke errors —
    // different LuaJIT entry — so pinning both is useful.
    return produce_worker_with_script(
        dir, "lua_engine::eval_syntax_error_returns_script_error",
        R"LUA(
            function on_produce(tx, msgs, api) return false end
        )LUA",
        [](LuaEngine &engine, RoleHostCore & /*core*/) {
            EXPECT_EQ(engine.script_error_count(), 0u);

            auto bad = engine.eval("invalid syntax {{{");
            EXPECT_EQ(bad.status,
                      pylabhub::scripting::InvokeStatus::ScriptError);
            EXPECT_EQ(engine.script_error_count(), 1u)
                << "eval syntax error must increment script_error_count "
                   "the same way invoke errors do";

            // Engine still usable: eval a valid scalar expression.
            auto good = engine.eval("return 7 * 6");
            EXPECT_EQ(good.status, pylabhub::scripting::InvokeStatus::Ok)
                << "engine must be usable after an eval syntax error";
            // NB: don't pin good.value here — that belongs in eval's
            // own test (Eval_ReturnsScalarResult, V2 line ~1428).
        });
}

// ── Error handling: setup-phase error paths (chunk 7b) ─────────────────────

int load_script_missing_file_returns_false(const std::string &dir)
{
    // Strengthened over V2.  V2 only asserted false return.  This
    // body additionally pins:
    //   (a) script_error_count stays 0 — setup errors must NOT
    //       increment the runtime script-error counter, which is
    //       reserved for invoke-time Lua errors (HEP-CORE-0019).
    //   (b) engine stays usable — a subsequent load_script with a
    //       valid file succeeds.  A regression that leaves the
    //       engine's ref table in a broken state after a missed
    //       file would fail the retry.
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            RoleHostCore core;
            LuaEngine    engine;
            ASSERT_TRUE(engine.initialize("test", &core));

            EXPECT_FALSE(engine.load_script(script_dir, "nonexistent.lua",
                                             "on_produce"))
                << "load_script must return false when file is missing";
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "missing-file error is a setup failure, NOT a script "
                   "error — script_error_count must stay 0";

            // Now write a valid script and retry — must succeed.
            write_script(script_dir,
                         "function on_produce(tx, msgs, api) return false end");
            EXPECT_TRUE(engine.load_script(script_dir, "init.lua",
                                            "on_produce"))
                << "engine must be re-usable after a load_script failure";

            engine.finalize();
        },
        "lua_engine::load_script_missing_file_returns_false",
        Logger::GetLifecycleModule());
}

int load_script_missing_required_callback_returns_false(const std::string &dir)
{
    // Strengthened: verifies has_callback("on_produce") is false
    // after the failed load (pins that the extracted-ref table is
    // clean, not holding a stale ref from a partial load), and
    // that a retry with a script containing on_produce succeeds.
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            RoleHostCore core;
            LuaEngine    engine;
            ASSERT_TRUE(engine.initialize("test", &core));

            // Script defines on_init but NOT on_produce.
            write_script(script_dir, R"LUA(
                function on_init(api) end
                -- on_produce intentionally absent
            )LUA");
            EXPECT_FALSE(engine.load_script(script_dir, "init.lua",
                                             "on_produce"))
                << "load_script must return false when required callback "
                   "is missing";
            EXPECT_EQ(engine.script_error_count(), 0u);

            // Retry with a script that HAS on_produce — must succeed.
            write_script(script_dir, R"LUA(
                function on_produce(tx, msgs, api) return false end
            )LUA");
            EXPECT_TRUE(engine.load_script(script_dir, "init.lua",
                                            "on_produce"));
            EXPECT_TRUE(engine.has_callback("on_produce"));

            engine.finalize();
        },
        "lua_engine::load_script_missing_required_callback_returns_false",
        Logger::GetLifecycleModule());
}

int load_script_syntax_error_returns_false(const std::string &dir)
{
    // Strengthened: verifies retry with valid syntax succeeds.
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            RoleHostCore core;
            LuaEngine    engine;
            ASSERT_TRUE(engine.initialize("test", &core));

            // Syntactically invalid Lua.
            write_script(script_dir,
                         "function on_produce(tx, msgs, api)  -- unterminated");
            EXPECT_FALSE(engine.load_script(script_dir, "init.lua",
                                             "on_produce"))
                << "load_script must return false on syntax error";
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "syntax error is a load-time failure, NOT a runtime "
                   "script error";

            // Overwrite with valid syntax and retry.
            write_script(script_dir,
                         "function on_produce(tx, msgs, api) return false end");
            EXPECT_TRUE(engine.load_script(script_dir, "init.lua",
                                            "on_produce"));

            engine.finalize();
        },
        "lua_engine::load_script_syntax_error_returns_false",
        Logger::GetLifecycleModule());
}

int register_slot_type_bad_field_type_returns_false(const std::string &dir)
{
    // Strengthened over V2.  V2 only asserted false on ONE bad
    // type (complex128).  This body additionally pins:
    //   (a) type_sizeof("BadFrame") returns 0 — the failed
    //       registration did NOT leak a partial type into the ffi
    //       table.
    //   (b) registering a DIFFERENT good schema under the same name
    //       subsequently succeeds — the failure doesn't poison the
    //       name slot.
    //   (c) a second unsupported type name also fails — pins the
    //       rejection isn't specific to one value.
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir,
                         "function on_produce(tx, msgs, api) return true end");

            RoleHostCore core;
            LuaEngine    engine;
            ASSERT_TRUE(engine.initialize("test", &core));
            ASSERT_TRUE(engine.load_script(script_dir, "init.lua",
                                            "on_produce"));

            // complex128 is unsupported.
            SchemaSpec bad;
            bad.has_schema = true;
            bad.fields.push_back({"x", "complex128", 1, 0});
            EXPECT_FALSE(engine.register_slot_type(bad, "BadFrame",
                                                    "aligned"))
                << "unsupported type 'complex128' must fail registration";

            EXPECT_EQ(engine.type_sizeof("BadFrame"), 0u)
                << "failed registration must NOT leak a partial type — "
                   "type_sizeof for an unregistered name is 0";

            // A different bogus type name should also fail.
            SchemaSpec bad2;
            bad2.has_schema = true;
            bad2.fields.push_back({"y", "not_a_type", 1, 0});
            EXPECT_FALSE(engine.register_slot_type(bad2, "BadFrame2",
                                                     "aligned"));

            // After both failures, registering a VALID schema under
            // the previously-failed name must succeed.
            auto good = simple_schema();
            EXPECT_TRUE(engine.register_slot_type(good, "BadFrame",
                                                    "aligned"))
                << "failed registration must not poison the name slot — "
                   "a subsequent valid schema under the same name must "
                   "register successfully";
            EXPECT_GT(engine.type_sizeof("BadFrame"), 0u);

            engine.finalize();
        },
        "lua_engine::register_slot_type_bad_field_type_returns_false",
        Logger::GetLifecycleModule());
}

int finalize_double_call_is_safe(const std::string &dir)
{
    // NEW gap-fill (from docs/todo/TESTING_TODO.md "finalize()
    // idempotence").  The engine's header does not promise
    // double-finalize is safe, but the role host's shutdown path
    // can reach finalize through multiple routes (normal stop,
    // critical error, signal handler) — confirming double-finalize
    // is a no-op rules out a real class of production bugs.
    //
    // Test sequence:
    //   1. Init, load_script, register_slot_type, build_api.
    //   2. First finalize() — primary cleanup.
    //   3. Second finalize() — must NOT crash, must NOT throw.
    //   4. After double-finalize, is_accepting() must be false
    //      (engine clearly rejects new work).
    //   5. A post-finalize invoke_produce must return Error (not
    //      crash) — demonstrates the engine's "dead state" is
    //      observable and not fatal.
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir,
                         "function on_produce(tx, msgs, api) return true end");

            RoleHostCore core;
            LuaEngine    engine;
            std::unique_ptr<RoleAPIBase> api;
            ASSERT_TRUE(setup_role_engine(engine, core, script_dir, api,
                                           RoleKind::Producer));

            // First finalize — primary cleanup.
            engine.finalize();

            // Second finalize — must be a safe no-op.
            engine.finalize();
            // (Reaching this line without SIGSEGV or exception is
            // itself the test — no EXPECT needed for "did not crash".)

            // Post-finalize state observability.
            EXPECT_FALSE(engine.is_accepting())
                << "post-finalize is_accepting must return false — "
                   "engine is in dead state, must not pretend otherwise";

            // Post-finalize invocations must fail gracefully, not crash.
            float buf = 0.0f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            auto r = engine.invoke_produce(
                pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(r, pylabhub::scripting::InvokeResult::Error)
                << "invoke_produce after finalize must surface as Error, "
                   "not attempt to run against torn-down Lua state";
        },
        "lua_engine::finalize_double_call_is_safe",
        Logger::GetLifecycleModule());
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

                // Chunk 5: Messages
                if (sc == "invoke_produce_receives_messages_event_with_details")
                    return invoke_produce_receives_messages_event_with_details(dir);
                if (sc == "invoke_produce_receives_messages_empty_vector")
                    return invoke_produce_receives_messages_empty_vector(dir);
                if (sc == "invoke_produce_receives_messages_data_message")
                    return invoke_produce_receives_messages_data_message(dir);
                if (sc == "invoke_consume_receives_messages_data_bare_format")
                    return invoke_consume_receives_messages_data_bare_format(dir);

                // Chunk 6a: api.* closures (introspection + control)
                if (sc == "api_version_info_returns_json_string")
                    return api_version_info_returns_json_string(dir);
                if (sc == "api_identity_uid_name_channel")
                    return api_identity_uid_name_channel(dir);
                if (sc == "api_log_dispatches_levels")
                    return api_log_dispatches_levels(dir);
                if (sc == "api_stop_sets_shutdown_requested")
                    return api_stop_sets_shutdown_requested(dir);
                if (sc == "api_critical_error_set_and_read_and_stop_reason")
                    return api_critical_error_set_and_read_and_stop_reason(dir);
                if (sc == "api_stop_reason_reflects_all_enum_values")
                    return api_stop_reason_reflects_all_enum_values(dir);

                // Chunk 6b: api.* custom-metrics closures
                if (sc == "api_report_metric_appears_under_custom")
                    return api_report_metric_appears_under_custom(dir);
                if (sc == "api_report_metric_overwrite_same_key")
                    return api_report_metric_overwrite_same_key(dir);
                if (sc == "api_report_metric_zero_value_preserved")
                    return api_report_metric_zero_value_preserved(dir);
                if (sc == "api_report_metrics_batch_accepts_table")
                    return api_report_metrics_batch_accepts_table(dir);
                if (sc == "api_report_metrics_non_table_arg_is_error")
                    return api_report_metrics_non_table_arg_is_error(dir);
                if (sc == "api_clear_custom_metrics_empties_and_allows_rewrite")
                    return api_clear_custom_metrics_empties_and_allows_rewrite(dir);

                // Chunk 6c: api.* shared-data closures
                if (sc == "api_shared_data_round_trip_all_variant_types")
                    return api_shared_data_round_trip_all_variant_types(dir);
                if (sc == "api_shared_data_get_missing_key_returns_nil")
                    return api_shared_data_get_missing_key_returns_nil(dir);
                if (sc == "api_shared_data_nil_removes_key")
                    return api_shared_data_nil_removes_key(dir);
                if (sc == "api_shared_data_overwrite_changes_type")
                    return api_shared_data_overwrite_changes_type(dir);
                if (sc == "api_shared_data_overwrite_changes_value_same_type")
                    return api_shared_data_overwrite_changes_value_same_type(dir);

                // Chunk 7a: runtime error surfacing
                if (sc == "invoke_multiple_errors_count_accumulates")
                    return invoke_multiple_errors_count_accumulates(dir);
                if (sc == "invoke_produce_wrong_return_type_is_error")
                    return invoke_produce_wrong_return_type_is_error(dir);
                if (sc == "invoke_produce_wrong_return_string_is_error")
                    return invoke_produce_wrong_return_string_is_error(dir);
                if (sc == "invoke_produce_stop_on_script_error_sets_shutdown")
                    return invoke_produce_stop_on_script_error_sets_shutdown(dir);
                if (sc == "invoke_on_init_or_stop_script_error_accumulates")
                    return invoke_on_init_or_stop_script_error_accumulates(dir);
                if (sc == "invoke_on_inbox_script_error_increments_count")
                    return invoke_on_inbox_script_error_increments_count(dir);
                if (sc == "eval_syntax_error_returns_script_error")
                    return eval_syntax_error_returns_script_error(dir);

                // Chunk 7b: setup-phase error paths
                if (sc == "load_script_missing_file_returns_false")
                    return load_script_missing_file_returns_false(dir);
                if (sc == "load_script_missing_required_callback_returns_false")
                    return load_script_missing_required_callback_returns_false(dir);
                if (sc == "load_script_syntax_error_returns_false")
                    return load_script_syntax_error_returns_false(dir);
                if (sc == "register_slot_type_bad_field_type_returns_false")
                    return register_slot_type_bad_field_type_returns_false(dir);
                if (sc == "finalize_double_call_is_safe")
                    return finalize_double_call_is_safe(dir);

                fmt::print(stderr,
                           "[lua_engine] ERROR: unknown scenario '{}'\n", sc);
                return 1;
            });
    }
};
static LuaEngineWorkerRegistrar g_lua_engine_registrar;

} // namespace
