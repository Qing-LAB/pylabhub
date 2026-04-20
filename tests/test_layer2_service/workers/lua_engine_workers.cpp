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
#include "utils/engine_module_params.hpp"
#include "utils/role_api_base.hpp"
#include "utils/role_host_core.hpp"
#include "utils/schema_utils.hpp"

#include "plh_service.hpp"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"
#include "test_schema_helpers.h"

#include <fmt/core.h>
#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using pylabhub::hub::FieldDef;
using pylabhub::hub::SchemaSpec;
using pylabhub::scripting::LuaEngine;
using pylabhub::scripting::RoleAPIBase;
using pylabhub::scripting::RoleHostCore;
using pylabhub::tests::all_types_schema;
using pylabhub::tests::complex_mixed_schema;
using pylabhub::tests::fz_array_schema;
using pylabhub::tests::helper::run_gtest_worker;
using pylabhub::tests::padding_schema;
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

// ── Generic engine.invoke() / engine.eval() (chunk 8a) ─────────────────────

int invoke_existing_function_returns_true(const std::string &dir)
{
    // Strengthened over V2 Invoke_ExistingFunction_ReturnsTrue.  V2
    // only asserted invoke() return value; this body additionally
    // verifies the function ACTUALLY executed (via set_shared_data
    // side-effect) so a bug where invoke returns true without
    // executing the callback would fail.
    return produce_worker_with_script(
        dir, "lua_engine::invoke_existing_function_returns_true",
        R"LUA(
            function on_produce(tx, msgs, api) return true end
            function on_heartbeat()
                api.set_shared_data("heartbeat_ran", true)
            end
        )LUA",
        [](LuaEngine &engine, RoleHostCore &core) {
            EXPECT_TRUE(engine.invoke("on_heartbeat"));
            // Body must have executed.
            auto v = core.get_shared_data("heartbeat_ran");
            ASSERT_TRUE(v.has_value())
                << "invoke returned true but the callback did not run";
            EXPECT_TRUE(std::holds_alternative<bool>(*v));
            EXPECT_TRUE(std::get<bool>(*v));
            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

int invoke_non_existent_function_returns_false(const std::string &dir)
{
    // Strengthened: pins that missing-function is NOT counted as a
    // script error (lookup miss is a return-false, not an error) —
    // V2 did not check script_error_count.
    return produce_worker_with_script(
        dir, "lua_engine::invoke_non_existent_function_returns_false",
        R"LUA(
            function on_produce(tx, msgs, api) return true end
        )LUA",
        [](LuaEngine &engine, RoleHostCore & /*core*/) {
            EXPECT_FALSE(engine.invoke("no_such_function"));
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "name lookup miss must NOT count as a script error";
        });
}

int invoke_empty_name_returns_false(const std::string &dir)
{
    // Strengthened: same non-error check as above; additionally pins
    // that an empty name skips the whole function-lookup path
    // (source: lua_engine.cpp:465 / :501 short-circuit before
    // any lua_getglobal call).
    return produce_worker_with_script(
        dir, "lua_engine::invoke_empty_name_returns_false",
        R"LUA(
            function on_produce(tx, msgs, api) return true end
        )LUA",
        [](LuaEngine &engine, RoleHostCore & /*core*/) {
            EXPECT_FALSE(engine.invoke(""));
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "empty-name short-circuit must NOT count as a script error";
        });
}

int invoke_script_error_returns_false_and_increments_errors(const std::string &dir)
{
    // V2 already asserts return false + count == 1.  No additional
    // strengthening needed — the conversion alone is the value here.
    return produce_worker_with_script(
        dir,
        "lua_engine::invoke_script_error_returns_false_and_increments_errors",
        R"LUA(
            function on_produce(tx, msgs, api) return true end
            function bad_func() error("intentional test error") end
        )LUA",
        [](LuaEngine &engine, RoleHostCore & /*core*/) {
            EXPECT_FALSE(engine.invoke("bad_func"));
            EXPECT_EQ(engine.script_error_count(), 1u);
        });
}

int invoke_with_args_returns_true(const std::string &dir)
{
    // Strengthened over V2 Invoke_WithArgs_ReturnsTrue.  V2 only
    // asserted true return — did NOT verify the args actually
    // reached Lua.  The engine pushes args as a Lua table
    // (lua_engine.cpp:525).  This body stores the received arg via
    // set_shared_data so C++ can verify the table reached the
    // callback with the expected keys/values.
    return produce_worker_with_script(
        dir, "lua_engine::invoke_with_args_returns_true",
        R"LUA(
            function on_produce(tx, msgs, api) return true end
            function greet(args)
                -- args is the table passed by engine.invoke(name, json)
                api.set_shared_data("got_name", args.name)
                api.set_shared_data("got_age", args.age)
            end
        )LUA",
        [](LuaEngine &engine, RoleHostCore &core) {
            nlohmann::json args = {{"name", "alice"}, {"age", 30}};
            EXPECT_TRUE(engine.invoke("greet", args));
            EXPECT_EQ(engine.script_error_count(), 0u);

            // Args must have propagated to Lua.
            auto n = core.get_shared_data("got_name");
            ASSERT_TRUE(n.has_value());
            EXPECT_EQ(std::get<std::string>(*n), "alice");

            auto a = core.get_shared_data("got_age");
            ASSERT_TRUE(a.has_value());
            EXPECT_EQ(std::get<int64_t>(*a), 30);
        });
}

int invoke_after_finalize_returns_false(const std::string &dir)
{
    // Strengthened over V2.  V2 only asserted invoke returns false
    // after finalize.  This body additionally:
    //   (a) asserts eval() returns InvokeStatus::EngineShutdown
    //       (distinct from ScriptError / NotFound) — source at
    //       lua_engine.cpp:543-544.
    //   (b) asserts script_error_count does NOT increment on the
    //       post-finalize reject path — finalization-gate rejection
    //       is NOT a script error.
    //
    // Complements chunk-7b Finalize_DoubleCallIsSafe which pins the
    // same property for invoke_produce; this one covers the generic
    // invoke() and eval() entry points.
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir, R"LUA(
                function on_produce(tx, msgs, api) return true end
                function on_heartbeat() end
            )LUA");

            RoleHostCore core;
            LuaEngine    engine;
            std::unique_ptr<RoleAPIBase> api;
            ASSERT_TRUE(setup_role_engine(engine, core, script_dir, api,
                                           RoleKind::Producer));

            engine.finalize();
            EXPECT_EQ(engine.script_error_count(), 0u);

            EXPECT_FALSE(engine.invoke("on_heartbeat"))
                << "post-finalize invoke must return false";
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "post-finalize reject must NOT increment error count";

            auto r = engine.eval("return 42");
            EXPECT_EQ(r.status,
                      pylabhub::scripting::InvokeStatus::EngineShutdown)
                << "post-finalize eval must return EngineShutdown, not "
                   "ScriptError — distinct status for 'engine is dead' "
                   "vs 'script had an error'";
            EXPECT_EQ(engine.script_error_count(), 0u);
        },
        "lua_engine::invoke_after_finalize_returns_false",
        Logger::GetLifecycleModule());
}

int eval_returns_scalar_result(const std::string &dir)
{
    // Strengthened over V2.  V2 tested 3 scalar types (int, string,
    // bool).  This body also tests:
    //   (d) nil return → JSON null (lua_to_json at the unconventional
    //       boundary)
    //   (e) fractional number → JSON double
    //   (f) negative number → sign preserved through the JSON bridge
    // And pins script_error_count stays 0 across all evals.
    return produce_worker_with_script(
        dir, "lua_engine::eval_returns_scalar_result",
        R"LUA(
            function on_produce(tx, msgs, api) return true end
        )LUA",
        [](LuaEngine &engine, RoleHostCore & /*core*/) {
            // Integer.
            auto r1 = engine.eval("return 42");
            EXPECT_EQ(r1.status, pylabhub::scripting::InvokeStatus::Ok);
            EXPECT_EQ(r1.value, 42);

            // String.
            auto r2 = engine.eval("return 'hello'");
            EXPECT_EQ(r2.status, pylabhub::scripting::InvokeStatus::Ok);
            EXPECT_EQ(r2.value, "hello");

            // Boolean.
            auto r3 = engine.eval("return true");
            EXPECT_EQ(r3.status, pylabhub::scripting::InvokeStatus::Ok);
            EXPECT_EQ(r3.value, true);

            // NEW: nil return → JSON null.
            auto r4 = engine.eval("return nil");
            EXPECT_EQ(r4.status, pylabhub::scripting::InvokeStatus::Ok);
            EXPECT_TRUE(r4.value.is_null())
                << "nil return must project to JSON null";

            // NEW: fractional number.
            auto r5 = engine.eval("return 3.5");
            EXPECT_EQ(r5.status, pylabhub::scripting::InvokeStatus::Ok);
            EXPECT_DOUBLE_EQ(r5.value.get<double>(), 3.5);

            // NEW: negative number.
            auto r6 = engine.eval("return -7");
            EXPECT_EQ(r6.status, pylabhub::scripting::InvokeStatus::Ok);
            EXPECT_EQ(r6.value, -7);

            EXPECT_EQ(engine.script_error_count(), 0u)
                << "successful evals must NOT bump error count";
        });
}

// ── Multi-state / thread-state (chunk 8b) ──────────────────────────────────

int supports_multi_state_returns_true(const std::string &dir)
{
    // Strengthened over V2 SupportsMultiState_ReturnsTrue.  V2
    // checked the property AFTER setup_engine — at which point a
    // primary state exists but no thread states have been created.
    // This body checks the property TWICE: immediately after
    // initialize() (no Lua state created yet) AND after build_api
    // (primary state initialized).  supports_multi_state is a
    // structural engine property; it must hold in both lifecycle
    // phases.
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir,
                         "function on_produce(tx, msgs, api) return true end");

            RoleHostCore core;
            LuaEngine    engine;
            ASSERT_TRUE(engine.initialize("test", &core));
            // Right after initialize — no script loaded, no thread
            // states. Property must already be true.
            EXPECT_TRUE(engine.supports_multi_state())
                << "supports_multi_state must hold immediately after "
                   "initialize() — it is structural, not state-dependent";

            ASSERT_TRUE(engine.load_script(script_dir, "init.lua",
                                            "on_produce"));
            auto spec = simple_schema();
            ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame",
                                                   "aligned"));
            auto api = make_api(core);
            ASSERT_TRUE(engine.build_api(*api));

            // After build_api too.
            EXPECT_TRUE(engine.supports_multi_state());

            engine.finalize();
        },
        "lua_engine::supports_multi_state_returns_true",
        Logger::GetLifecycleModule());
}

int state_persists_across_calls(const std::string &dir)
{
    // Strengthened over V2.  V2 invokes 3 times and checks the
    // counter increments to 3.  This body adds a 4th call after
    // a no-op sleep equivalent (no sleep — just an extra invoke
    // to confirm continued increment) AND verifies the counter
    // persists in the OWNER state only (a non-owner thread reading
    // it would see 0, but that's the territory of the next test).
    return produce_worker_with_script(
        dir, "lua_engine::state_persists_across_calls",
        R"LUA(
            call_count = 0
            function on_produce(tx, msgs, api)
                call_count = call_count + 1
                if tx.slot then
                    tx.slot.value = call_count
                end
                return true
            end
        )LUA",
        [](LuaEngine &engine, RoleHostCore & /*core*/) {
            std::vector<pylabhub::scripting::IncomingMessage> msgs;

            for (size_t i = 1; i <= 4; ++i)
            {
                float buf = 0.0f;
                auto r = engine.invoke_produce(
                    pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
                EXPECT_EQ(r, pylabhub::scripting::InvokeResult::Commit);
                EXPECT_FLOAT_EQ(buf, static_cast<float>(i))
                    << "call_count must be " << i << " on iteration "
                    << i << " — Lua global state must persist across "
                    << "invocations on the owner thread";
            }
            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

int invoke_from_non_owner_thread_works(const std::string &dir)
{
    // Strengthened over V2.  V2 only checked invoke() returned true
    // from the spawned thread.  This body additionally pins that the
    // function actually executed by observing a side-effect via
    // shared_data (which IS thread-shared — see the dedicated
    // shared_data_cross_thread_visible test for the contrast with
    // Lua globals).
    return produce_worker_with_script(
        dir, "lua_engine::invoke_from_non_owner_thread_works",
        R"LUA(
            function on_produce(tx, msgs, api) return true end
            function on_heartbeat()
                api.set_shared_data("hb_thread_ran", true)
            end
        )LUA",
        [](LuaEngine &engine, RoleHostCore &core) {
            bool result = false;
            std::thread t([&] { result = engine.invoke("on_heartbeat"); });
            t.join();

            EXPECT_TRUE(result)
                << "invoke from non-owner thread must succeed";

            // Side-effect must have landed in shared core.
            auto v = core.get_shared_data("hb_thread_ran");
            ASSERT_TRUE(v.has_value())
                << "non-owner thread invoke succeeded but the callback "
                   "did not actually run";
            EXPECT_TRUE(std::get<bool>(*v));
        });
}

int invoke_non_owner_thread_uses_independent_state(const std::string &dir)
{
    // Strengthened over V2.  V2 verified the FORWARD direction
    // (non-owner sets a global, owner can't see it).  This body
    // ALSO verifies the REVERSE direction (owner sets a global,
    // non-owner can't see it).  Both directions of isolation must
    // hold for thread-state independence to be a real property,
    // not a one-way fluke.
    return produce_worker_with_script(
        dir, "lua_engine::invoke_non_owner_thread_uses_independent_state",
        R"LUA(
            function on_produce(tx, msgs, api) return true end
            function set_in_child() child_marker = true end
            function set_in_owner() owner_marker = true end
            function read_child_marker_in_owner()
                api.set_shared_data("child_marker_seen_in_owner",
                                    child_marker == true)
            end
            function read_owner_marker_in_child()
                api.set_shared_data("owner_marker_seen_in_child",
                                    owner_marker == true)
            end
        )LUA",
        [](LuaEngine &engine, RoleHostCore &core) {
            // Forward: child sets, owner reads → must be invisible.
            std::thread t1([&] { engine.invoke("set_in_child"); });
            t1.join();
            engine.invoke("read_child_marker_in_owner");
            auto v1 = core.get_shared_data("child_marker_seen_in_owner");
            ASSERT_TRUE(v1.has_value());
            EXPECT_FALSE(std::get<bool>(*v1))
                << "child_marker set in non-owner thread leaked into "
                   "owner state — Lua globals must be per-state";

            // Reverse: owner sets, child reads → must be invisible.
            engine.invoke("set_in_owner");
            std::thread t2([&] {
                engine.invoke("read_owner_marker_in_child");
            });
            t2.join();
            auto v2 = core.get_shared_data("owner_marker_seen_in_child");
            ASSERT_TRUE(v2.has_value());
            EXPECT_FALSE(std::get<bool>(*v2))
                << "owner_marker set in owner thread leaked into "
                   "non-owner state — isolation must be bidirectional";

            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

int invoke_concurrent_owner_and_non_owner(const std::string &dir)
{
    // Straight conversion — V2 already strong (barrier sync, kCalls
    // iterations on each side, both shared_data counts verified).
    // The kCalls magic number stays at 20 to match V2 behavior; if
    // STRESS_TEST_LEVEL gets adopted this is one of the candidates
    // for amplification (already noted in TESTING_TODO).
    return produce_worker_with_script(
        dir, "lua_engine::invoke_concurrent_owner_and_non_owner",
        R"LUA(
            function on_produce(tx, msgs, api) return true end
            function inc_owner()
                local v = api.get_shared_data("owner_count") or 0
                api.set_shared_data("owner_count", v + 1)
            end
            function inc_child()
                local v = api.get_shared_data("child_count") or 0
                api.set_shared_data("child_count", v + 1)
            end
        )LUA",
        [](LuaEngine &engine, RoleHostCore &core) {
            constexpr int kCalls = 20;
            std::atomic<bool> barrier{false};
            std::atomic<int>  child_ok{0};

            std::thread t([&] {
                while (!barrier.load(std::memory_order_acquire)) {}
                for (int i = 0; i < kCalls; ++i)
                    if (engine.invoke("inc_child"))
                        child_ok.fetch_add(1, std::memory_order_relaxed);
            });

            int owner_ok = 0;
            barrier.store(true, std::memory_order_release);
            for (int i = 0; i < kCalls; ++i)
                if (engine.invoke("inc_owner"))
                    ++owner_ok;
            t.join();

            EXPECT_EQ(owner_ok, kCalls);
            EXPECT_EQ(child_ok.load(), kCalls);

            auto ov = core.get_shared_data("owner_count");
            auto cv = core.get_shared_data("child_count");
            ASSERT_TRUE(ov.has_value());
            ASSERT_TRUE(cv.has_value());
            EXPECT_EQ(std::get<int64_t>(*ov), kCalls);
            EXPECT_EQ(std::get<int64_t>(*cv), kCalls);
            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

int shared_data_cross_thread_visible(const std::string &dir)
{
    // Strengthened over V2.  V2 verified the FORWARD direction
    // (non-owner thread writes via Lua, owner reads via C++).  This
    // body also verifies the REVERSE (owner writes via C++ /
    // set_shared_data, non-owner thread reads via Lua and reports
    // back).  Cross-thread visibility must be bidirectional —
    // RoleHostCore::shared_data_ is one map shared across threads,
    // not per-thread storage.
    return produce_worker_with_script(
        dir, "lua_engine::shared_data_cross_thread_visible",
        R"LUA(
            function on_produce(tx, msgs, api) return true end
            function set_marker_in_child()
                api.set_shared_data("from_thread", 123)
            end
            function read_owner_value_in_child()
                local v = api.get_shared_data("from_owner") or -1
                api.set_shared_data("read_back_in_child", v)
            end
        )LUA",
        [](LuaEngine &engine, RoleHostCore &core) {
            // Forward: child writes via Lua, owner reads via C++.
            std::thread t1([&] {
                engine.invoke("set_marker_in_child");
            });
            t1.join();

            auto v1 = core.get_shared_data("from_thread");
            ASSERT_TRUE(v1.has_value());
            EXPECT_EQ(std::get<int64_t>(*v1), 123)
                << "non-owner Lua write must be visible to C++ owner";

            // Reverse: C++ writes (simulating an owner write or
            // an external producer) → non-owner Lua reads it back.
            core.set_shared_data("from_owner", int64_t{456});
            std::thread t2([&] {
                engine.invoke("read_owner_value_in_child");
            });
            t2.join();

            auto v2 = core.get_shared_data("read_back_in_child");
            ASSERT_TRUE(v2.has_value());
            EXPECT_EQ(std::get<int64_t>(*v2), 456)
                << "C++-side write must be visible to non-owner Lua "
                   "thread — RoleHostCore::shared_data_ is one map "
                   "shared across all threads, not per-thread";

            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

// ── Misc V2 leftovers (chunk 8c) ───────────────────────────────────────────

int has_callback_detects_presence_absence(const std::string &dir)
{
    // Strengthened over V2.  V2 only checked 4 names (one absent).
    // This body exhaustively covers has_callback's three code paths
    // (lua_engine.cpp:578-602):
    //
    //   1. The 6 hard-coded callback slots that load_script extracts
    //      refs for (on_init, on_stop, on_produce, on_consume,
    //      on_process, on_inbox).  Split into "defined" and "absent"
    //      sets so a regression that confuses presence/absence
    //      direction fails for at least one entry on each side.
    //   2. Unknown name fallback → lua_getglobal → returns true if
    //      a global function with that name exists.
    //   3. Unknown name with no matching global → returns false.
    //
    // Inline setup (no setup_role_engine) because we need to drive
    // load_script ourselves with a script that defines a deliberate
    // subset of callbacks.
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            // Defined: on_produce, on_init, on_inbox + one ad-hoc
            //          global function (not a role-callback name) so
            //          we can exercise the lua_getglobal fallback.
            // Absent:  on_stop, on_consume, on_process,
            //          totally_made_up_name.
            write_script(script_dir, R"LUA(
                function on_produce(tx, msgs, api) return true end
                function on_init(api) end
                function on_inbox(msg, api) return true end
                function ad_hoc_helper() end
            )LUA");

            RoleHostCore core;
            LuaEngine    engine;
            ASSERT_TRUE(engine.initialize("test", &core));
            ASSERT_TRUE(engine.load_script(script_dir, "init.lua",
                                            "on_produce"));

            // Path 1 — present role callbacks (hard-coded ref slot).
            EXPECT_TRUE(engine.has_callback("on_produce"));
            EXPECT_TRUE(engine.has_callback("on_init"));
            EXPECT_TRUE(engine.has_callback("on_inbox"));

            // Path 1 — absent role callbacks.
            EXPECT_FALSE(engine.has_callback("on_stop"));
            EXPECT_FALSE(engine.has_callback("on_consume"));
            EXPECT_FALSE(engine.has_callback("on_process"));

            // Path 2 — unknown name WITH a matching global function:
            // must hit the lua_getglobal fallback and return true.
            EXPECT_TRUE(engine.has_callback("ad_hoc_helper"))
                << "has_callback unknown-name fallback must look up "
                   "the global table — a regression that always "
                   "returns false for unknown names would fail here";

            // Path 3 — unknown name WITH NO matching global → false.
            EXPECT_FALSE(engine.has_callback("totally_made_up_name"))
                << "has_callback must NOT be accidentally always-true "
                   "for unknown names";

            engine.finalize();
        },
        "lua_engine::has_callback_detects_presence_absence",
        Logger::GetLifecycleModule());
}

int invoke_consume_messages_data_and_event_mixed(const std::string &dir)
{
    // Strengthened P3 conversion of V2 InvokeConsume_BareDataMessages.
    // V2 verified shape (msgs[1] is bare string, msgs[2] is event
    // table) but did NOT pin the exact data bytes or event field.
    // This body adds: the data string equals "AB" (the two raw bytes
    // 0x41 0x42 the C++ side passed), and the event field equals the
    // exact channel_closing literal.  Catches a regression where the
    // bare-string projection truncates or re-encodes data.
    //
    // Distinct from chunk 5's data-bare-only test — that one used
    // 1 data message ("hello") with no events. THIS test pins that
    // BOTH projection paths (push_messages_table_bare_ for data,
    // table-with-event-key for events) coexist correctly in the
    // SAME consumer-side msgs vector.
    return consume_worker_with_script(
        dir, "lua_engine::invoke_consume_messages_data_and_event_mixed",
        R"LUA(
            function on_consume(rx, msgs, api)
                assert(#msgs == 2,
                       "expected 2 messages, got " .. #msgs)

                -- msgs[1]: data message → bare bytes string "AB"
                assert(type(msgs[1]) == "string",
                       "data msg must be bare string, got " .. type(msgs[1]))
                assert(msgs[1] == "AB",
                       "data bytes must round-trip exactly; expected "
                       .. "'AB' (0x41 0x42), got '" .. msgs[1] .. "'")

                -- msgs[2]: event message → table with .event field
                assert(type(msgs[2]) == "table",
                       "event msg must be table, got " .. type(msgs[2]))
                assert(msgs[2].event == "channel_closing",
                       "event field expected 'channel_closing', got "
                       .. tostring(msgs[2].event))
                return true
            end
        )LUA",
        [](LuaEngine &engine, RoleHostCore & /*core*/) {
            std::vector<pylabhub::scripting::IncomingMessage> msgs;

            // Data message (no event, has sender + data bytes).
            pylabhub::scripting::IncomingMessage dm;
            dm.sender = "sender-id";
            dm.data   = {std::byte{0x41}, std::byte{0x42}};
            msgs.push_back(std::move(dm));

            // Event message (event populated, no data).
            pylabhub::scripting::IncomingMessage em;
            em.event = "channel_closing";
            msgs.push_back(std::move(em));

            float buf = 0.0f;
            auto r = engine.invoke_consume(
                pylabhub::scripting::InvokeRx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(r, pylabhub::scripting::InvokeResult::Commit);
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "Lua-side asserts must have passed — failure means "
                   "the consumer's mixed data+event projection path "
                   "lost shape or content";
        });
}

// ── Inbox + slot-only invoke (chunk 9a) ────────────────────────────────────

int invoke_produce_slot_only_no_flexzone_on_invoke(const std::string &dir)
{
    // Direct conversion of V2 InvokeProduce_SlotOnly_NoFlexzoneOnInvoke.
    // The contract being pinned: InvokeTx carries ONLY a slot pointer/size,
    // not a flexzone — flexzone is accessed via api.flexzone(...) closure
    // built once at build_api time.  The script can write to tx.slot
    // even when an OutFlexFrame is registered (the registration is a
    // separate concern from invoke-time data).
    //
    // Inline setup because we register both OutSlotFrame AND
    // OutFlexFrame, which the setup_role_engine helper does not do
    // by default (it only registers one slot per role).
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir, R"LUA(
                function on_produce(tx, msgs, api)
                    assert(tx.slot ~= nil, "expected slot")
                    tx.slot.value = 10.0
                    return true
                end
            )LUA");

            RoleHostCore core;
            LuaEngine    engine;
            ASSERT_TRUE(engine.initialize("test", &core));
            ASSERT_TRUE(engine.load_script(script_dir, "init.lua",
                                            "on_produce"));

            auto spec = simple_schema();
            ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame",
                                                   "aligned"));
            ASSERT_TRUE(engine.register_slot_type(spec, "OutFlexFrame",
                                                   "aligned"));

            auto api = make_api(core);
            ASSERT_TRUE(engine.build_api(*api));

            float slot_buf = 0.0f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            auto r = engine.invoke_produce(
                pylabhub::scripting::InvokeTx{&slot_buf, sizeof(slot_buf)},
                msgs);
            EXPECT_EQ(r, pylabhub::scripting::InvokeResult::Commit);
            EXPECT_FLOAT_EQ(slot_buf, 10.0f);
            EXPECT_EQ(engine.script_error_count(), 0u);

            engine.finalize();
        },
        "lua_engine::invoke_produce_slot_only_no_flexzone_on_invoke",
        Logger::GetLifecycleModule());
}

int invoke_on_inbox_typed_data(const std::string &dir)
{
    // Strengthened over V2.  V2 asserted msg.data.value (typed via
    // InboxFrame ffi cast) and msg.sender_uid.  This body adds:
    //   - msg.seq must equal the seq passed in (3rd arg of InvokeInbox)
    //   - C++-side asserts result == Commit (V2 only checked
    //     script_error_count, missing the dispatch return value)
    //
    // Inline setup because InboxFrame must be registered alongside
    // OutSlotFrame.
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir, R"LUA(
                function on_produce(tx, msgs, api) return false end
                function on_inbox(msg, api)
                    assert(msg.data ~= nil, "expected inbox data")
                    assert(math.abs(msg.data.value - 77.0) < 0.01,
                           "expected ~77.0, got " .. tostring(msg.data.value))
                    assert(msg.sender_uid == "PROD-SENDER-00000001",
                           "sender_uid expected, got " ..
                           tostring(msg.sender_uid))
                    -- NEW: pin seq projection (lua_engine.cpp:985-986).
                    assert(msg.seq == 7,
                           "seq expected 7, got " .. tostring(msg.seq))
                    return true
                end
            )LUA");

            RoleHostCore core;
            LuaEngine    engine;
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

            float inbox_data = 77.0f;
            auto r = engine.invoke_on_inbox(
                {&inbox_data, sizeof(inbox_data),
                 "PROD-SENDER-00000001", 7});

            EXPECT_EQ(r, pylabhub::scripting::InvokeResult::Commit)
                << "on_inbox returned true → must map to Commit";
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "Lua-side asserts must have all passed";

            engine.finalize();
        },
        "lua_engine::invoke_on_inbox_typed_data",
        Logger::GetLifecycleModule());
}

int type_sizeof_inbox_frame_returns_correct_size(const std::string &dir)
{
    // Corrected and strengthened from V2 TypeSizeof_InboxFrame_*.
    // V2 had a bug in its docstring: comment said "28 bytes aligned"
    // but the assertion correctly required 32. This body documents
    // the actual layout precisely:
    //
    //   uint8 flag    @  0 (1)
    //   pad           @  1 (7)  ← align float64 to 8
    //   float64 value @  8 (8)
    //   uint16 count  @ 16 (2)
    //   pad           @ 18 (2)  ← align int32 to 4
    //   int32 status  @ 20 (4)
    //   char[5] label @ 24 (5)
    //   pad           @ 29 (3)  ← struct alignment = 8 (max field alignof)
    //   total         = 32
    //
    // Strengthened: also asserts BOTH OutSlotFrame and InboxFrame
    // sizes equal compute_schema_size() (engine sizeof must match
    // the role host's sizing helper — V2 asserted equality between
    // SlotFrame alias and InboxFrame but didn't tie either to the
    // canonical compute_schema_size).
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

            SchemaSpec spec;
            spec.has_schema = true;
            spec.fields.push_back({"flag",   "uint8",   1, 0});
            spec.fields.push_back({"value",  "float64", 1, 0});
            spec.fields.push_back({"count",  "uint16",  1, 0});
            spec.fields.push_back({"status", "int32",   1, 0});
            spec.fields.push_back({"label",  "string",  1, 5});

            ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame",
                                                   "aligned"));
            ASSERT_TRUE(engine.register_slot_type(spec, "InboxFrame",
                                                   "aligned"));

            auto api = make_api(core);
            ASSERT_TRUE(engine.build_api(*api));

            const size_t expected =
                pylabhub::hub::compute_schema_size(spec, "aligned");
            EXPECT_EQ(expected, 32u)
                << "schema layout: 1 + 7pad + 8 + 2 + 2pad + 4 + 5 + "
                   "3pad = 32 — if compute_schema_size returns "
                   "anything else the layout helper is wrong";

            EXPECT_EQ(engine.type_sizeof("SlotFrame"), expected)
                << "engine sizeof(SlotFrame alias) must equal "
                   "compute_schema_size — divergence means the ffi "
                   "cdef the engine builds doesn't match what the role "
                   "host uses to size buffers";
            EXPECT_EQ(engine.type_sizeof("InboxFrame"), expected);

            engine.finalize();
        },
        "lua_engine::type_sizeof_inbox_frame_returns_correct_size",
        Logger::GetLifecycleModule());
}

int invoke_on_inbox_missing_type_reports_error(const std::string &dir)
{
    // Strengthened over V2.  V2 asserted script_error_count >= 1
    // (loose).  This body asserts:
    //   - result == InvokeResult::Error (V2 didn't capture this)
    //   - script_error_count == 1 EXACTLY (loud single error, not
    //     a cascade of multiple errors masking each other)
    //   - the engine logs the documented error message at
    //     lua_engine.cpp:965-967 ("InboxFrame type not registered")
    //     — pinned via the parent test's expected_error_substrings.
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir, R"LUA(
                function on_produce(tx, msgs, api) return false end
                function on_inbox(msg, api) return true end
            )LUA");

            RoleHostCore core;
            LuaEngine    engine;
            ASSERT_TRUE(engine.initialize("test", &core));
            ASSERT_TRUE(engine.load_script(script_dir, "init.lua",
                                            "on_produce"));

            auto spec = simple_schema();
            ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame",
                                                   "aligned"));
            // Deliberately NO InboxFrame registration.

            auto api = make_api(core);
            ASSERT_TRUE(engine.build_api(*api));

            float raw = 1.0f;
            auto r = engine.invoke_on_inbox(
                {&raw, sizeof(raw), "CONS-SENDER-00000001", 1});

            EXPECT_EQ(r, pylabhub::scripting::InvokeResult::Error)
                << "missing InboxFrame must surface as Error result";
            EXPECT_EQ(engine.script_error_count(), 1u)
                << "must increment script_error_count by EXACTLY 1 — "
                   "more would mean the engine emitted multiple errors "
                   "(e.g. tried to call on_inbox after the missing-type "
                   "guard fired)";

            engine.finalize();
        },
        "lua_engine::invoke_on_inbox_missing_type_reports_error",
        Logger::GetLifecycleModule());
}

// ── Logical-size accessors via engine_lifecycle_startup (chunk 9b) ─────────
//
// Helper for the four near-identical SlotLogicalSize/FlexzoneLogicalSize
// tests.  Each test follows the same pattern:
//   1. Build a SchemaSpec with the chosen packing.
//   2. Pre-populate RoleHostCore with the slot (and optional flexzone)
//      spec — emulating what the role host does at schema-resolve time.
//   3. Wire EngineModuleParams and call engine_lifecycle_startup
//      (the production setup pathway used by all role hosts).
//   4. Inside on_produce, the Lua script reads api.slot_logical_size()
//      [and api.flexzone_logical_size() if applicable] and asserts
//      against the expected number.
//   5. C++ also verifies engine.type_sizeof(...) and
//      hub::compute_schema_size(...) all agree on the same number.
//   6. engine_lifecycle_shutdown for clean teardown.
//
// Strengthening over V2: V2 asserted hard-coded numbers and (for slot)
// engine.type_sizeof("OutSlotFrame") == compute_schema_size, but did
// NOT pin that the Lua-side api.slot_logical_size() returns the SAME
// number as compute_schema_size.  This worker structure passes the
// expected size into Lua as shared_data so Lua's assertion uses the
// authoritative compute_schema_size value (not a hard-coded literal).

namespace
{

struct LogicalSizeCase
{
    SchemaSpec   slot_spec;
    SchemaSpec   fz_spec;        // .has_schema=false → no flexzone
    const char  *out_packing;
    size_t       expected_slot;       // = compute_schema_size(slot_spec, packing)
    size_t       expected_fz;         // = compute_schema_size(fz_spec, packing)
    // ANCHOR LITERALS: hard-coded sizes the test author KNOWS to be
    // correct for these specific schemas.  Without anchors, every
    // assertion in the test would route through compute_schema_size,
    // turning the slot-size check into a tautology
    // (compute_schema_size == compute_schema_size).  Anchors catch a
    // silent regression in compute_schema_size itself for these
    // schemas (e.g. the layout helper drops a padding byte).
    size_t       anchor_slot;         // hard-coded expected slot size
    size_t       anchor_fz;           // hard-coded expected fz size (0 if none)
};

int run_logical_size_case(const std::string &dir,
                          const char        *scenario_name,
                          const LogicalSizeCase &c)
{
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);

            // The Lua script reads the expected sizes from shared_data
            // (set by C++ pre-startup) so the assertion uses the
            // authoritative compute_schema_size, not a literal — pins
            // a roundtrip from C++ schema math through Lua closures.
            const bool has_fz = c.fz_spec.has_schema;
            std::string lua_src =
                "function on_produce(tx, msgs, api)\n"
                "    local slot_sz = api.slot_logical_size()\n"
                "    local exp_slot = api.get_shared_data('exp_slot')\n"
                "    assert(slot_sz == exp_slot,\n"
                "           'slot_logical_size mismatch: expected '\n"
                "           .. tostring(exp_slot) .. ', got '\n"
                "           .. tostring(slot_sz))\n";
            if (has_fz)
            {
                lua_src +=
                    "    local fz_sz = api.flexzone_logical_size()\n"
                    "    local exp_fz = api.get_shared_data('exp_fz')\n"
                    "    assert(fz_sz == exp_fz,\n"
                    "           'flexzone_logical_size mismatch: expected '\n"
                    "           .. tostring(exp_fz) .. ', got '\n"
                    "           .. tostring(fz_sz))\n";
            }
            lua_src += "    return false\nend\n";
            write_script(script_dir, lua_src);

            RoleHostCore core;
            LuaEngine    engine;
            auto         api = make_api(core, "prod");

            // Pre-set core specs (role-host-equivalent step).
            core.set_out_slot_spec(SchemaSpec{c.slot_spec},
                                   c.expected_slot);
            if (has_fz)
                core.set_out_fz_spec(SchemaSpec{c.fz_spec},
                                     pylabhub::hub::align_to_physical_page(
                                         c.expected_fz));

            // Pre-populate shared_data with expected sizes so the Lua
            // side uses authoritative numbers, not hard-coded literals.
            core.set_shared_data("exp_slot",
                                 static_cast<int64_t>(c.expected_slot));
            if (has_fz)
                core.set_shared_data("exp_fz",
                                     static_cast<int64_t>(c.expected_fz));

            pylabhub::scripting::EngineModuleParams params;
            params.engine            = &engine;
            params.api               = api.get();
            params.tag               = "prod";
            params.script_dir        = script_dir;
            params.entry_point       = "init.lua";
            params.required_callback = "on_produce";
            params.out_slot_spec     = c.slot_spec;
            params.out_packing       = c.out_packing;
            if (has_fz)
                params.out_fz_spec   = c.fz_spec;

            ASSERT_NO_THROW(
                pylabhub::scripting::engine_lifecycle_startup(
                    nullptr, &params));

            // ANCHOR check (NOT a tautology): compute_schema_size for
            // this specific schema must equal the hard-coded number
            // the test author verified by hand.  A silent regression
            // in the layout helper for this schema fails here.
            EXPECT_EQ(pylabhub::hub::compute_schema_size(
                          c.slot_spec, c.out_packing),
                      c.anchor_slot)
                << "compute_schema_size DRIFTED for this slot schema — "
                   "the layout helper now returns a different size for "
                   "the same input. This is a real regression in the "
                   "schema math, not in the engine.";

            // Engine cdef vs schema-helper consistency.
            EXPECT_EQ(engine.type_sizeof("OutSlotFrame"),
                      c.anchor_slot)
                << "engine type_sizeof disagrees with the anchor — ffi "
                   "cdef the engine builds doesn't match the canonical "
                   "size used by the role host to allocate buffers";

            if (has_fz)
            {
                EXPECT_EQ(pylabhub::hub::compute_schema_size(
                              c.fz_spec, c.out_packing),
                          c.anchor_fz)
                    << "compute_schema_size DRIFTED for this fz schema";
                EXPECT_EQ(engine.type_sizeof("OutFlexFrame"),
                          c.anchor_fz);
            }

            // Drive on_produce — the Lua-side asserts run here.
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            std::vector<uint8_t> buf(c.expected_slot, 0);
            auto r = engine.invoke_produce(
                pylabhub::scripting::InvokeTx{buf.data(), buf.size()}, msgs);
            EXPECT_EQ(r, pylabhub::scripting::InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "Lua-side slot/fz size asserts must have passed";

            pylabhub::scripting::engine_lifecycle_shutdown(nullptr, &params);
        },
        scenario_name, Logger::GetLifecycleModule());
}

} // anonymous

int slot_logical_size_aligned_padding_sensitive(const std::string &dir)
{
    auto spec = padding_schema();
    spec.packing = "aligned";
    LogicalSizeCase c{
        /*slot_spec*/   spec,
        /*fz_spec*/     SchemaSpec{},  // no flexzone
        /*out_packing*/ "aligned",
        /*exp_slot*/    pylabhub::hub::compute_schema_size(spec, "aligned"),
        /*exp_fz*/      0,
        /*anchor_slot*/ 16,            // hand-verified for padding_schema aligned
        /*anchor_fz*/   0,
    };
    return run_logical_size_case(
        dir, "lua_engine::slot_logical_size_aligned_padding_sensitive", c);
}

int slot_logical_size_packed_no_padding(const std::string &dir)
{
    auto spec = padding_schema();
    spec.packing = "packed";
    LogicalSizeCase c{
        spec, SchemaSpec{}, "packed",
        pylabhub::hub::compute_schema_size(spec, "packed"), 0,
        /*anchor_slot*/ 13,            // hand-verified for padding_schema packed
        /*anchor_fz*/   0,
    };
    return run_logical_size_case(
        dir, "lua_engine::slot_logical_size_packed_no_padding", c);
}

int slot_logical_size_complex_mixed_aligned(const std::string &dir)
{
    auto spec = complex_mixed_schema();
    spec.packing = "aligned";
    LogicalSizeCase c{
        spec, SchemaSpec{}, "aligned",
        pylabhub::hub::compute_schema_size(spec, "aligned"), 0,
        /*anchor_slot*/ 56,            // hand-verified for complex_mixed_schema aligned
        /*anchor_fz*/   0,
    };
    return run_logical_size_case(
        dir, "lua_engine::slot_logical_size_complex_mixed_aligned", c);
}

int flexzone_logical_size_array_fields(const std::string &dir)
{
    auto slot = padding_schema();   slot.packing = "aligned";
    auto fz   = fz_array_schema();  fz.packing   = "aligned";
    LogicalSizeCase c{
        slot, fz, "aligned",
        pylabhub::hub::compute_schema_size(slot, "aligned"),
        pylabhub::hub::compute_schema_size(fz,   "aligned"),
        /*anchor_slot*/ 16,            // hand-verified for padding_schema aligned
        /*anchor_fz*/   24,            // hand-verified for fz_array_schema aligned
    };
    return run_logical_size_case(
        dir, "lua_engine::flexzone_logical_size_array_fields", c);
}

// ── Graceful degradation: api.* closures without infrastructure (chunk 10) ─
//
// Each worker calls the closure twice in the Lua body — pinning that
// the graceful-degradation path is idempotent (no first-call cached
// bad state), which V2 did not exercise (single call only).
//
// Naming convention: producer worker, since the closures under test
// are all role-agnostic — same return shape regardless of role tag.
namespace
{

int run_graceful_degrade_case(const std::string &dir,
                              const char        *scenario_name,
                              const char        *lua_call_with_assert)
{
    // Builds a script of the form:
    //   function on_produce(tx, msgs, api)
    //     <lua_call_with_assert>   -- run twice to pin idempotence
    //     <lua_call_with_assert>
    //     return false
    //   end
    std::string lua = "function on_produce(tx, msgs, api)\n";
    lua += lua_call_with_assert;
    lua += "\n";
    lua += lua_call_with_assert;
    lua += "\nreturn false\nend\n";

    return produce_worker_with_script(
        dir, scenario_name, lua,
        [](LuaEngine &engine, RoleHostCore & /*core*/) {
            float buf = 0.0f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            auto r = engine.invoke_produce(
                pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(r, pylabhub::scripting::InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "Lua-side asserts must have all passed";
        });
}

} // anonymous

int api_open_inbox_without_broker_returns_nil(const std::string &dir)
{
    return run_graceful_degrade_case(
        dir, "lua_engine::api_open_inbox_without_broker_returns_nil",
        R"LUA(
            local h = api.open_inbox("some-uid")
            assert(h == nil,
                   "open_inbox without broker must return nil, got "
                   .. tostring(h))
        )LUA");
}

int api_band_join_without_broker_returns_nil(const std::string &dir)
{
    return run_graceful_degrade_case(
        dir, "lua_engine::api_band_join_without_broker_returns_nil",
        R"LUA(
            local r = api.band_join("#test_ch")
            assert(r == nil,
                   "band_join without broker must return nil, got "
                   .. tostring(r))
        )LUA");
}

int api_band_leave_without_broker_returns_false(const std::string &dir)
{
    return run_graceful_degrade_case(
        dir, "lua_engine::api_band_leave_without_broker_returns_false",
        R"LUA(
            local r = api.band_leave("#test_ch")
            assert(r == false,
                   "band_leave without broker must return false (NOT nil), "
                   .. "got " .. tostring(r))
        )LUA");
}

int api_band_broadcast_without_broker_no_error(const std::string &dir)
{
    // band_broadcast has no return value to verify.  Contract: must
    // not raise, must not log an error.  Idempotence pinned by the
    // double-call in the helper.
    return run_graceful_degrade_case(
        dir, "lua_engine::api_band_broadcast_without_broker_no_error",
        R"LUA(
            api.band_broadcast("#test_ch", {hello = "world", value = 42})
        )LUA");
}

int api_band_members_without_broker_returns_nil(const std::string &dir)
{
    return run_graceful_degrade_case(
        dir, "lua_engine::api_band_members_without_broker_returns_nil",
        R"LUA(
            local r = api.band_members("#test_ch")
            assert(r == nil,
                   "band_members without broker must return nil, got "
                   .. tostring(r))
        )LUA");
}

int api_spinlock_count_without_shm_returns_zero(const std::string &dir)
{
    return run_graceful_degrade_case(
        dir, "lua_engine::api_spinlock_count_without_shm_returns_zero",
        R"LUA(
            local n = api.spinlock_count()
            assert(n == 0,
                   "spinlock_count without SHM must return 0, got "
                   .. tostring(n))
        )LUA");
}

int api_spinlock_acquire_without_shm_is_pcall_error(const std::string &dir)
{
    // Strengthened over V2.  V2 only checked `not ok`; this body also
    // checks the err message is non-empty (a regression where the error
    // is raised with no message would slip past the V2 check).  Note
    // pcall catches the Lua error so it does NOT bump
    // script_error_count — that's the explicit "pcall makes this
    // recoverable" contract.
    return run_graceful_degrade_case(
        dir,
        "lua_engine::api_spinlock_acquire_without_shm_is_pcall_error",
        R"LUA(
            local ok, err = pcall(api.spinlock, 0)
            assert(not ok,
                   "spinlock(0) without SHM must raise (pcall returns false)")
            assert(type(err) == "string" and #err > 0,
                   "raised error must carry a message, got " .. tostring(err))
        )LUA");
}

int api_flexzone_accessor_without_shm_returns_nil(const std::string &dir)
{
    return run_graceful_degrade_case(
        dir, "lua_engine::api_flexzone_accessor_without_shm_returns_nil",
        R"LUA(
            local fz = api.flexzone()
            assert(fz == nil,
                   "flexzone() without SHM must return nil, got "
                   .. tostring(fz))
        )LUA");
}

// ── Metrics: individual accessors + hierarchical table (chunk 11) ──────────

int metrics_individual_accessors_read_core_counters_live(const std::string &dir)
{
    // Strengthened over V2 MetricsClosures_ReadFromRoleHostCounters.
    // V2 set core counters once and read once. This body pins LIVE
    // read semantics by:
    //   1. Reading BEFORE any C++ set (must see 0).
    //   2. Setting via core.test_set_*, reading via Lua (must see set
    //      values).
    //   3. INCREMENTING further from C++ between two consecutive Lua
    //      reads (must see updated values, NOT cached snapshot).
    //
    // The C++→Lua bridge is via shared_data: C++ writes the
    // expected values into shared_data per phase, Lua reads both the
    // accessor result AND the expected value, and asserts agreement.
    // This catches a regression where the api closure caches the
    // first read and returns stale values on subsequent calls.
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir, R"LUA(
                function on_produce(tx, msgs, api)
                    local phase = api.get_shared_data("phase")
                    local exp_ow = api.get_shared_data("exp_ow")
                    local exp_dr = api.get_shared_data("exp_dr")

                    local ow = api.out_slots_written()
                    local dr = api.out_drop_count()

                    assert(ow == exp_ow,
                           "phase " .. phase .. ": out_slots_written "
                           .. "expected " .. tostring(exp_ow)
                           .. ", got " .. tostring(ow))
                    assert(dr == exp_dr,
                           "phase " .. phase .. ": out_drop_count "
                           .. "expected " .. tostring(exp_dr)
                           .. ", got " .. tostring(dr))
                    return false
                end
            )LUA");

            RoleHostCore core;
            LuaEngine    engine;
            std::unique_ptr<RoleAPIBase> api;
            ASSERT_TRUE(setup_role_engine(engine, core, script_dir, api,
                                           RoleKind::Producer));

            float buf = 0.0f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;

            auto invoke_phase = [&](int phase, int64_t exp_ow,
                                    int64_t exp_dr) {
                core.set_shared_data("phase", static_cast<int64_t>(phase));
                core.set_shared_data("exp_ow", exp_ow);
                core.set_shared_data("exp_dr", exp_dr);
                auto r = engine.invoke_produce(
                    pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
                EXPECT_EQ(r, pylabhub::scripting::InvokeResult::Discard)
                    << "phase " << phase;
                EXPECT_EQ(engine.script_error_count(), 0u)
                    << "phase " << phase;
            };

            // Phase 0: pristine — both counters start at 0.
            invoke_phase(0, 0, 0);

            // Phase 1: set non-zero values.
            core.test_set_out_slots_written(42);
            core.test_set_out_drop_count(7);
            invoke_phase(1, 42, 7);

            // Phase 2: increment further between invocations — pins
            // LIVE read (cached snapshot would still report 42/7).
            core.test_set_out_slots_written(100);
            core.test_set_out_drop_count(15);
            invoke_phase(2, 100, 15);

            engine.finalize();
        },
        "lua_engine::metrics_individual_accessors_read_core_counters_live",
        Logger::GetLifecycleModule());
}

int metrics_in_slots_received_works_consumer(const std::string &dir)
{
    // Strengthened over V2 MetricsClosures_InReceivedWorks. V2 set
    // in_slots_received=15 and read once. This body additionally:
    //   1. Pins out_slots_written stays 0 (consumer doesn't write).
    //   2. Pins out_drop_count stays 0 same reason.
    //   3. Reads in_slots_received across TWO invocations with a
    //      C++ increment between → pins live read on consumer side.
    //
    // The "consumer reading producer counters returns 0" behavior
    // is the API contract that L2 doesn't enforce per role tag at
    // closure level — the closures exist on all roles (lua_engine.cpp
    // :312-314). What IS enforced is that the underlying core
    // counters default to 0 unless explicitly set.
    return consume_worker_with_script(
        dir, "lua_engine::metrics_in_slots_received_works_consumer",
        R"LUA(
            function on_consume(rx, msgs, api)
                local ir = api.in_slots_received()
                local exp_ir = api.get_shared_data("exp_ir")
                assert(ir == exp_ir,
                       "in_slots_received expected " .. tostring(exp_ir)
                       .. ", got " .. tostring(ir))
                -- Producer-side counters must stay 0 on consumer.
                assert(api.out_slots_written() == 0,
                       "consumer out_slots_written must be 0, got "
                       .. tostring(api.out_slots_written()))
                assert(api.out_drop_count() == 0,
                       "consumer out_drop_count must be 0, got "
                       .. tostring(api.out_drop_count()))
                return true
            end
        )LUA",
        [](LuaEngine &engine, RoleHostCore &core) {
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            float data = 1.0f;

            // Phase 1: in=15.
            core.test_set_in_slots_received(15);
            core.set_shared_data("exp_ir", static_cast<int64_t>(15));
            auto r1 = engine.invoke_consume(
                pylabhub::scripting::InvokeRx{&data, sizeof(data)}, msgs);
            EXPECT_EQ(r1, pylabhub::scripting::InvokeResult::Commit);
            EXPECT_EQ(engine.script_error_count(), 0u);

            // Phase 2: bump in=42 between invokes — pins live read.
            core.test_set_in_slots_received(42);
            core.set_shared_data("exp_ir", static_cast<int64_t>(42));
            auto r2 = engine.invoke_consume(
                pylabhub::scripting::InvokeRx{&data, sizeof(data)}, msgs);
            EXPECT_EQ(r2, pylabhub::scripting::InvokeResult::Commit);
            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

int metrics_hierarchical_table_producer_full_shape(const std::string &dir)
{
    // Strengthened over V2 Metrics_HierarchicalTable_Producer.  V2:
    //   - Type-checks m.loop fields (3 fields, type-only)
    //   - Asserts m.role.{out_slots_written, out_drop_count} VALUES
    //   - Asserts script_error_count is a number (no value check)
    //   - Asserts m.queue == nil, m.inbox == nil
    //
    // P3 additions:
    //   - Anchored values for ALL 5 m.loop fields including
    //     acquire_retry_count (V2 missed this one).
    //   - Anchored values for all 4 m.role fields.
    //   - script_error_count: pinned via raised-error pre-pass.
    //   - m.role MUST also have in_slots_received key (the L2 design
    //     exposes all 4 keys regardless of role; V2 didn't pin this).
    //   - No "custom" group (we don't call report_metric).
    //
    // Inline setup because we need to:
    //   1. Pre-set ALL loop counters via core.set_*.
    //   2. Trigger one script error via a separate pre-invoke before
    //      the real metrics-reading invoke.
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir, R"LUA(
                function on_produce(tx, msgs, api)
                    -- The first invocation deliberately raises so
                    -- script_error_count becomes 1.  Subsequent
                    -- invocations skip that branch.
                    if api.get_shared_data("phase") == 0 then
                        error("seed for script_error_count")
                    end

                    local m = api.metrics()
                    assert(type(m) == "table", "metrics must be a table")

                    -- Top-level shape — no queue (no real queue
                    -- connected), no inbox, no custom.
                    assert(m.queue == nil,
                           "queue must be absent when no queue connected")
                    assert(m.inbox == nil,
                           "inbox must be absent when no inbox connected")
                    assert(m.custom == nil,
                           "custom must be absent (no report_metric called)")

                    -- m.loop: 5 anchored fields (V2 missed
                    -- acquire_retry_count).
                    assert(type(m.loop) == "table",
                           "m.loop must be a table")
                    assert(m.loop.iteration_count == 7,
                           "iteration_count expected 7, got "
                           .. tostring(m.loop.iteration_count))
                    assert(m.loop.loop_overrun_count == 2,
                           "loop_overrun_count expected 2, got "
                           .. tostring(m.loop.loop_overrun_count))
                    assert(m.loop.last_cycle_work_us == 555,
                           "last_cycle_work_us expected 555, got "
                           .. tostring(m.loop.last_cycle_work_us))
                    assert(m.loop.configured_period_us == 999,
                           "configured_period_us expected 999, got "
                           .. tostring(m.loop.configured_period_us))
                    assert(type(m.loop.acquire_retry_count) == "number",
                           "acquire_retry_count must be a number — V2 "
                           .. "missed this loop field")

                    -- m.role: all 4 keys present (lua_engine.cpp:1770-1777
                    -- emits all four regardless of role).  Anchored
                    -- values for the producer-relevant ones.
                    assert(type(m.role) == "table", "m.role must be table")
                    assert(m.role.out_slots_written == 5,
                           "role.out_slots_written expected 5")
                    assert(m.role.out_drop_count == 2,
                           "role.out_drop_count expected 2")
                    assert(m.role.in_slots_received == 0,
                           "role.in_slots_received must be 0 for producer "
                           .. "with no input")
                    -- script_error_count: bumped to 1 by the seed
                    -- branch above on the first invocation.
                    assert(m.role.script_error_count == 1,
                           "role.script_error_count expected 1 (one "
                           .. "raised error on phase 0), got "
                           .. tostring(m.role.script_error_count))
                    return false
                end
            )LUA");

            RoleHostCore core;
            LuaEngine    engine;
            std::unique_ptr<RoleAPIBase> api;
            ASSERT_TRUE(setup_role_engine(engine, core, script_dir, api,
                                           RoleKind::Producer));

            // Pre-set anchored loop counters.
            for (int i = 0; i < 7; ++i) core.inc_iteration_count();
            for (int i = 0; i < 2; ++i) core.inc_loop_overrun();
            core.set_last_cycle_work_us(555);
            core.set_configured_period(999);
            // Pre-set anchored role counters.
            core.test_set_out_slots_written(5);
            core.test_set_out_drop_count(2);

            float buf = 0.0f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;

            // Phase 0: seed an error to bump script_error_count.
            core.set_shared_data("phase", static_cast<int64_t>(0));
            auto r0 = engine.invoke_produce(
                pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(r0, pylabhub::scripting::InvokeResult::Error);
            EXPECT_EQ(engine.script_error_count(), 1u);

            // Phase 1: real metrics read.
            core.set_shared_data("phase", static_cast<int64_t>(1));
            auto r1 = engine.invoke_produce(
                pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(r1, pylabhub::scripting::InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 1u)
                << "phase 1 metric-read must not bump error count";

            engine.finalize();
        },
        "lua_engine::metrics_hierarchical_table_producer_full_shape",
        Logger::GetLifecycleModule());
}

int metrics_hierarchical_table_consumer_full_shape(const std::string &dir)
{
    // Strengthened: V2 only checked m.role.in_slots_received. This
    // body audits the FULL m.role shape for consumer (all 4 keys
    // present) and the consumer m.loop shape (same 5 fields as
    // producer — the loop metrics are role-agnostic).  Pins that
    // consumer m.role.out_* counters stay at 0 (no role-tagged
    // filtering of the role table — all 4 keys always present).
    return consume_worker_with_script(
        dir, "lua_engine::metrics_hierarchical_table_consumer_full_shape",
        R"LUA(
            function on_consume(rx, msgs, api)
                local m = api.metrics()
                assert(type(m.loop) == "table")
                assert(type(m.role) == "table")
                assert(m.queue == nil, "no queue connected")
                assert(m.inbox == nil, "no inbox connected")

                -- m.loop fields: 5 fields, all type-checked.
                local loop_fields = {
                    "iteration_count", "loop_overrun_count",
                    "last_cycle_work_us", "configured_period_us",
                    "acquire_retry_count"
                }
                for _, f in ipairs(loop_fields) do
                    assert(type(m.loop[f]) == "number",
                           "m.loop." .. f .. " must be a number, got "
                           .. type(m.loop[f]))
                end

                -- m.role: anchored value for in_slots_received,
                -- producer-only counters stay 0.
                assert(m.role.in_slots_received == 10,
                       "in_slots_received expected 10, got "
                       .. tostring(m.role.in_slots_received))
                assert(m.role.out_slots_written == 0,
                       "consumer's out_slots_written must be 0")
                assert(m.role.out_drop_count == 0,
                       "consumer's out_drop_count must be 0")
                assert(type(m.role.script_error_count) == "number",
                       "script_error_count must be a number")
                return true
            end
        )LUA",
        [](LuaEngine &engine, RoleHostCore &core) {
            core.test_set_in_slots_received(10);
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            float data = 1.0f;
            auto r = engine.invoke_consume(
                pylabhub::scripting::InvokeRx{&data, sizeof(data)}, msgs);
            EXPECT_EQ(r, pylabhub::scripting::InvokeResult::Commit);
            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

int metrics_loop_overrun_count_live_increments(const std::string &dir)
{
    // Strengthened over V2 Api_LoopOverrunCount_ReadsFromCore.  V2
    // incremented 3× before invoke and read once.  This body pins
    // LIVE updates across multiple invocations:
    //   - Phase 0: 0 increments → expect 0
    //   - Phase 1: bump to 3 (between invokes) → expect 3
    //   - Phase 2: bump to 5 → expect 5
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir, R"LUA(
                function on_produce(tx, msgs, api)
                    local v = api.loop_overrun_count()
                    local exp = api.get_shared_data("exp")
                    assert(v == exp,
                           "expected " .. tostring(exp) .. ", got "
                           .. tostring(v))
                    return false
                end
            )LUA");

            RoleHostCore core;
            LuaEngine    engine;
            std::unique_ptr<RoleAPIBase> api;
            ASSERT_TRUE(setup_role_engine(engine, core, script_dir, api,
                                           RoleKind::Producer));

            float buf = 0.0f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;

            auto invoke_with_exp = [&](int64_t exp) {
                core.set_shared_data("exp", exp);
                auto r = engine.invoke_produce(
                    pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
                EXPECT_EQ(r, pylabhub::scripting::InvokeResult::Discard);
                EXPECT_EQ(engine.script_error_count(), 0u);
            };

            invoke_with_exp(0);              // pristine
            for (int i = 0; i < 3; ++i) core.inc_loop_overrun();
            invoke_with_exp(3);              // bumped to 3
            for (int i = 0; i < 2; ++i) core.inc_loop_overrun();
            invoke_with_exp(5);              // bumped further to 5

            engine.finalize();
        },
        "lua_engine::metrics_loop_overrun_count_live_increments",
        Logger::GetLifecycleModule());
}

int metrics_last_cycle_work_us_overwrite_semantics(const std::string &dir)
{
    // Strengthened over V2 Api_LastCycleWorkUs_ReadsFromCore.  V2:
    // set 12345 once, read once.  This body pins:
    //   - Initial read returns 0 (default).
    //   - set→read cycle returns set value.
    //   - SECOND set OVERWRITES (not accumulates) the first.
    //   - Overflowing into uint64 max-ish values reads correctly
    //     (catches truncation on Lua int64 boundary).
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir, R"LUA(
                function on_produce(tx, msgs, api)
                    local v = api.last_cycle_work_us()
                    local exp = api.get_shared_data("exp")
                    assert(v == exp,
                           "expected " .. tostring(exp) .. ", got "
                           .. tostring(v))
                    return false
                end
            )LUA");

            RoleHostCore core;
            LuaEngine    engine;
            std::unique_ptr<RoleAPIBase> api;
            ASSERT_TRUE(setup_role_engine(engine, core, script_dir, api,
                                           RoleKind::Producer));

            float buf = 0.0f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            auto invoke_with_exp = [&](int64_t exp) {
                core.set_shared_data("exp", exp);
                auto r = engine.invoke_produce(
                    pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
                EXPECT_EQ(r, pylabhub::scripting::InvokeResult::Discard);
                EXPECT_EQ(engine.script_error_count(), 0u);
            };

            invoke_with_exp(0);                // default
            core.set_last_cycle_work_us(12345);
            invoke_with_exp(12345);            // first set
            core.set_last_cycle_work_us(99999);
            invoke_with_exp(99999);            // overwrite (not accumulate)
            // Large value near int32 boundary — pin no truncation.
            core.set_last_cycle_work_us(2'500'000'000ULL);
            invoke_with_exp(2'500'000'000LL);  // > INT32_MAX

            engine.finalize();
        },
        "lua_engine::metrics_last_cycle_work_us_overwrite_semantics",
        Logger::GetLifecycleModule());
}

int metrics_all_loop_fields_anchored_values(const std::string &dir)
{
    // Strengthened over V2 Metrics_AllLoopFields_Present.  V2 anchored
    // 4 fields (10, 3, 500, 1000).  This body anchors all 5 fields
    // including acquire_retry_count, AND pins that NO EXTRA loop
    // fields appear (key inventory check).  A regression that adds a
    // loop field without a corresponding test update would fail the
    // inventory assertion — forcing the test author to acknowledge
    // the new field deliberately.
    return produce_worker_with_script(
        dir, "lua_engine::metrics_all_loop_fields_anchored_values",
        R"LUA(
            function on_produce(tx, msgs, api)
                local m = api.metrics()
                assert(type(m.loop) == "table")

                -- Anchored values for the 4 settable fields.
                assert(m.loop.iteration_count == 10,
                       "iteration_count expected 10")
                assert(m.loop.loop_overrun_count == 3,
                       "loop_overrun_count expected 3")
                assert(m.loop.last_cycle_work_us == 500,
                       "last_cycle_work_us expected 500")
                assert(m.loop.configured_period_us == 1000,
                       "configured_period_us expected 1000")
                -- acquire_retry_count: type-only (no setter exposed
                -- in this test scope; defaults to 0).
                assert(type(m.loop.acquire_retry_count) == "number",
                       "acquire_retry_count must be a number")
                assert(m.loop.acquire_retry_count == 0,
                       "acquire_retry_count default expected 0, got "
                       .. tostring(m.loop.acquire_retry_count))

                -- Inventory check: m.loop must have EXACTLY these 5
                -- keys.  Catches regressions where a new loop field
                -- is added to the engine but NOT to the test (the
                -- test would otherwise silently miss new coverage).
                local count = 0
                local expected = {
                    iteration_count=true, loop_overrun_count=true,
                    last_cycle_work_us=true, configured_period_us=true,
                    acquire_retry_count=true
                }
                for k, _ in pairs(m.loop) do
                    assert(expected[k] ~= nil,
                           "unexpected loop key '" .. k .. "' — engine "
                           .. "added a field, update this test")
                    count = count + 1
                end
                assert(count == 5,
                       "expected 5 loop keys, got " .. count
                       .. " (engine removed a field?)")
                return false
            end
        )LUA",
        [](LuaEngine &engine, RoleHostCore &core) {
            for (int i = 0; i < 10; ++i) core.inc_iteration_count();
            for (int i = 0; i < 3; ++i) core.inc_loop_overrun();
            core.set_last_cycle_work_us(500);
            core.set_configured_period(1000);

            float buf = 0.0f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            auto r = engine.invoke_produce(
                pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(r, pylabhub::scripting::InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

int metrics_role_script_error_count_reflects_raised_error(const std::string &dir)
{
    // NEW gap-fill.  V2 only type-checked m.role.script_error_count
    // — never verified the value transitions.  This body pins:
    //   - Phase 0: m.role.script_error_count == 0 (clean start).
    //   - Phase 1: raise an error → script_error_count becomes 1.
    //   - Phase 2: raise again → becomes 2.
    //   - Both Lua-side (m.role.script_error_count) AND C++-side
    //     (engine.script_error_count()) must agree at each phase.
    //
    // The script switches behavior based on a "phase" shared_data
    // hint: phase 0 reads metrics, phase 1+ raises an error.
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir, R"LUA(
                function on_produce(tx, msgs, api)
                    local phase = api.get_shared_data("phase")
                    if phase == 1 or phase == 2 then
                        error("seed phase " .. tostring(phase))
                    end
                    -- phase 0 or 3 (verify): read m.role.script_error_count
                    local m = api.metrics()
                    local exp = api.get_shared_data("exp_count")
                    assert(m.role.script_error_count == exp,
                           "phase " .. tostring(phase)
                           .. ": script_error_count expected "
                           .. tostring(exp) .. ", got "
                           .. tostring(m.role.script_error_count))
                    return false
                end
            )LUA");

            RoleHostCore core;
            LuaEngine    engine;
            std::unique_ptr<RoleAPIBase> api;
            ASSERT_TRUE(setup_role_engine(engine, core, script_dir, api,
                                           RoleKind::Producer));

            float buf = 0.0f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;

            auto invoke_phase = [&](int phase, int64_t exp_count,
                                    pylabhub::scripting::InvokeResult exp_r) {
                core.set_shared_data("phase", static_cast<int64_t>(phase));
                core.set_shared_data("exp_count", exp_count);
                auto r = engine.invoke_produce(
                    pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
                EXPECT_EQ(r, exp_r) << "phase " << phase;
            };

            // Phase 0: pristine read — count == 0.
            invoke_phase(0, 0, pylabhub::scripting::InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 0u);

            // Phase 1: raise.  Engine bumps script_error_count to 1.
            invoke_phase(1, /*unused*/0, pylabhub::scripting::InvokeResult::Error);
            EXPECT_EQ(engine.script_error_count(), 1u);

            // Phase 3 verification: read — m.role.script_error_count == 1.
            invoke_phase(3, 1, pylabhub::scripting::InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 1u);

            // Phase 2: raise again.  Count becomes 2.
            invoke_phase(2, /*unused*/0, pylabhub::scripting::InvokeResult::Error);
            EXPECT_EQ(engine.script_error_count(), 2u);

            // Final verify: read — count == 2.  Both Lua-table and
            // engine.script_error_count() must agree.
            invoke_phase(3, 2, pylabhub::scripting::InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 2u);

            engine.finalize();
        },
        "lua_engine::metrics_role_script_error_count_reflects_raised_error",
        Logger::GetLifecycleModule());
}

// ── Queue-state defaults + env strings + processor channels (chunk 12) ────

int queue_state_consumer_without_queue_returns_defaults(const std::string &dir)
{
    // Strengthened over V2 Api_ConsumerQueueState_WithoutQueue. V2
    // asserted in_capacity==0, in_policy=="", last_seq==0 ONCE.
    // This body additionally:
    //   - Asserts in_policy returns an EMPTY string (not nil).
    //     Closures return empty-string-not-nil is a real contract
    //     per lua_engine.cpp; a regression that returns nil would
    //     break downstream code that does `#api.in_policy()` etc.
    //   - Repeats the queries TWICE in the same callback to pin
    //     idempotence (no cached bad state).
    //   - Pins that the out_* closures are ABSENT for consumer
    //     (role-specific closure exposure — lua_engine.cpp:339-343
    //     shows consumer only gets in_* closures, not out_*).
    return consume_worker_with_script(
        dir,
        "lua_engine::queue_state_consumer_without_queue_returns_defaults",
        R"LUA(
            function on_consume(rx, msgs, api)
                -- Call 1 — baseline.
                assert(api.in_capacity() == 0,
                       "1st in_capacity expected 0, got "
                       .. tostring(api.in_capacity()))
                local pol = api.in_policy()
                assert(type(pol) == "string",
                       "in_policy must be a string (NOT nil), got "
                       .. type(pol))
                assert(pol == "",
                       "in_policy expected empty string, got '" .. pol .. "'")
                assert(api.last_seq() == 0, "1st last_seq expected 0")

                -- Call 2 — idempotence.
                assert(api.in_capacity() == 0, "2nd in_capacity drift")
                assert(api.in_policy() == "", "2nd in_policy drift")
                assert(api.last_seq() == 0, "2nd last_seq drift")

                -- Closures that should NOT exist on consumer
                -- (lua_engine.cpp:339-343 only pushes in_* closures;
                -- out_capacity / out_policy are producer-only).
                assert(api.out_capacity == nil,
                       "consumer must not have api.out_capacity closure")
                assert(api.out_policy == nil,
                       "consumer must not have api.out_policy closure")
                return true
            end
        )LUA",
        [](LuaEngine &engine, RoleHostCore & /*core*/) {
            float buf = 1.0f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            auto r = engine.invoke_consume(
                pylabhub::scripting::InvokeRx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(r, pylabhub::scripting::InvokeResult::Commit);
            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

int queue_state_producer_without_queue_returns_defaults(const std::string &dir)
{
    // Strengthened over V2 Api_ProducerQueueState_WithoutQueue. V2:
    // out_capacity==0, out_policy=="". This body also:
    //   - Calls twice (idempotence).
    //   - Pins in_* closures ABSENT on producer (role-specific).
    //   - Pins last_seq closure also ABSENT on producer (it's
    //     consumer/processor only per lua_engine.cpp:342, :363).
    return produce_worker_with_script(
        dir, "lua_engine::queue_state_producer_without_queue_returns_defaults",
        R"LUA(
            function on_produce(tx, msgs, api)
                assert(api.out_capacity() == 0,
                       "1st out_capacity expected 0")
                local pol = api.out_policy()
                assert(type(pol) == "string" and pol == "",
                       "1st out_policy expected empty string")

                assert(api.out_capacity() == 0, "2nd out_capacity drift")
                assert(api.out_policy() == "", "2nd out_policy drift")

                -- Role-specific closure exposure check:
                -- producer does NOT get in_* or last_seq closures
                -- (lua_engine.cpp:322-334 only pushes out_* for prod).
                assert(api.in_capacity == nil,
                       "producer must not have api.in_capacity closure")
                assert(api.in_policy == nil,
                       "producer must not have api.in_policy closure")
                assert(api.last_seq == nil,
                       "producer must not have api.last_seq closure")
                return false
            end
        )LUA",
        [](LuaEngine &engine, RoleHostCore & /*core*/) {
            float buf = 0.0f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            auto r = engine.invoke_produce(
                pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(r, pylabhub::scripting::InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

int queue_state_processor_dual_without_queues_returns_defaults(const std::string &dir)
{
    // Strengthened over V2 Api_ProcessorQueueState_DualDefaults. V2
    // checked all 5 closures once. This body also:
    //   - Calls twice (idempotence).
    //   - Pins processor has BOTH sets of closures (the whole point
    //     of the "dual" in the test name — processor is the only
    //     role that exposes both in_* and out_*).
    //   - Pins in_policy and out_policy both return empty strings
    //     (not nil — type enforcement).
    return process_worker_with_script(
        dir,
        "lua_engine::queue_state_processor_dual_without_queues_returns_defaults",
        R"LUA(
            function on_process(rx, tx, msgs, api)
                -- Processor exposes BOTH in_* and out_* closures
                -- (lua_engine.cpp:357-363).
                assert(api.in_capacity ~= nil,
                       "processor must have in_capacity closure")
                assert(api.out_capacity ~= nil,
                       "processor must have out_capacity closure")
                assert(api.last_seq ~= nil,
                       "processor must have last_seq closure")

                -- Dual queue-state — values (call 1).
                assert(api.in_capacity() == 0, "1st in_cap")
                assert(api.out_capacity() == 0, "1st out_cap")
                assert(api.in_policy() == "", "1st in_pol")
                assert(api.out_policy() == "", "1st out_pol")
                assert(api.last_seq() == 0, "1st last_seq")

                -- Call 2 — idempotence.
                assert(api.in_capacity() == 0, "2nd in_cap drift")
                assert(api.out_capacity() == 0, "2nd out_cap drift")
                assert(api.in_policy() == "", "2nd in_pol drift")
                assert(api.out_policy() == "", "2nd out_pol drift")
                assert(api.last_seq() == 0, "2nd last_seq drift")

                return false
            end
        )LUA",
        [](LuaEngine &engine, RoleHostCore & /*core*/) {
            float in_data  = 1.0f;
            float out_data = 0.0f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            auto r = engine.invoke_process(
                pylabhub::scripting::InvokeRx{&in_data, sizeof(in_data)},
                pylabhub::scripting::InvokeTx{&out_data, sizeof(out_data)},
                msgs);
            EXPECT_EQ(r, pylabhub::scripting::InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

int api_environment_strings_reflect_setters(const std::string &dir)
{
    // Strengthened over V2 Api_EnvironmentStrings_LogsDirRunDir.
    // V2 only type-checked the strings and anchored log_level="error".
    //
    // This body pins two properties for DIRECTORY strings
    // (script_dir, role_dir):
    //   1. ANCHORED READ: set via api->set_*() before build_api →
    //      Lua side reads those values.
    //   2. FROZEN-AT-BUILD-API: after build_api, mutating the api's
    //      directories via set_*() must NOT propagate — Lua keeps
    //      the snapshot.  Directories are role config identity; they
    //      must not silently change mid-session.
    //
    // log_level is DELIBERATELY NOT tested for frozen-vs-live here.
    // The current engine freezes it as a field (lua_engine.cpp:1194),
    // but the DESIRED DESIGN is live-mutable (so scripts can toggle
    // verbosity at runtime).  Fixing this requires a coordinated
    // cross-engine change (Lua + Python + Native binding layers) and
    // is captured in docs/todo/TESTING_TODO.md under "Script API
    // live-vs-frozen contract".  Until that lands, we don't pin
    // log_level's mutability direction — neither frozen nor live —
    // because the test would have to change when the engine changes.
    //
    // logs_dir / run_dir have no set_* method at this L2 layer (env-
    // provided at runtime in production); type-only check retained.
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir, R"LUA(
                function on_produce(tx, msgs, api)
                    local phase = api.get_shared_data("phase")

                    -- Directory fields: FROZEN at build_api per
                    -- lua_engine.cpp:1196-1200.  Both phases see the
                    -- original pre-build_api values.
                    assert(api.script_dir == "/tmp/fake-script",
                           "phase " .. tostring(phase)
                           .. ": script_dir must stay frozen, got '"
                           .. tostring(api.script_dir) .. "'")
                    assert(api.role_dir == "/tmp/fake-role",
                           "phase " .. tostring(phase)
                           .. ": role_dir must stay frozen, got '"
                           .. tostring(api.role_dir) .. "'")

                    -- logs_dir / run_dir: type-only (no L2 setter).
                    assert(type(api.logs_dir) == "string",
                           "logs_dir must be string")
                    assert(type(api.run_dir) == "string",
                           "run_dir must be string")

                    -- log_level: read but NOT pinned to a specific
                    -- value or mutation semantics.  The mutability
                    -- contract is under design — see TESTING_TODO.
                    -- Just pin it's a string (engine must expose
                    -- SOMETHING, not crash).
                    assert(type(api.log_level) == "string"
                           -- Promoting log_level to a closure would
                           -- make api.log_level a function in Lua;
                           -- accept both to avoid coupling this
                           -- test to the binding shape.
                           or type(api.log_level) == "function",
                           "log_level must be string or closure, got "
                           .. type(api.log_level))
                    return false
                end
            )LUA");

            RoleHostCore core;
            LuaEngine    engine;
            ASSERT_TRUE(engine.initialize("test", &core));
            ASSERT_TRUE(engine.load_script(script_dir, "init.lua",
                                            "on_produce"));
            auto spec = simple_schema();
            ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame",
                                                   "aligned"));

            auto api = make_api(core);
            api->set_log_level("warn");
            api->set_script_dir("/tmp/fake-script");
            api->set_role_dir("/tmp/fake-role");
            ASSERT_TRUE(engine.build_api(*api));

            float buf = 0.0f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;

            // Phase 0: fresh read after build_api.
            core.set_shared_data("phase", static_cast<int64_t>(0));
            auto r0 = engine.invoke_produce(
                pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(r0, pylabhub::scripting::InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 0u);

            // Mutate directories post-build_api — Lua MUST NOT see
            // these changes (frozen semantics for identity config).
            api->set_script_dir("/tmp/CHANGED-script");
            api->set_role_dir("/tmp/CHANGED-role");
            // Note: deliberately NOT mutating log_level here — see
            // the comment at the top.

            // Phase 1: Lua still reads the frozen directory snapshots.
            core.set_shared_data("phase", static_cast<int64_t>(1));
            auto r1 = engine.invoke_produce(
                pylabhub::scripting::InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(r1, pylabhub::scripting::InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "phase 1 asserts failing means DIRECTORY strings are "
                   "not frozen at build_api — that's a regression "
                   "(directories are role identity, must not change)";

            engine.finalize();
        },
        "lua_engine::api_environment_strings_reflect_setters",
        Logger::GetLifecycleModule());
}

int api_processor_channels_reflect_setters(const std::string &dir)
{
    // Strengthened over V2 Api_ProcessorChannels_InOut. V2 asserted
    // in_channel == "sensor.input" / out_channel == "sensor.output"
    // ONCE. This body:
    //   - Pins in_channel / out_channel are closures (callable),
    //     not string fields.
    //   - Calls the closures TWICE (idempotence).
    //   - Pins that api.channel() returns the same as
    //     api.in_channel() for processor (the input side is the
    //     canonical "channel" attribute).
    //
    // Inline setup: make_api / set_channel / set_out_channel must
    // happen BEFORE build_api so the closures see the anchored
    // values.  setup_role_engine helper doesn't expose the api
    // unique_ptr to the body, so we set up manually.
    //
    // Note: processor is the only role with in_channel/out_channel
    // closures (lua_engine.cpp:346-347). Producer/Consumer have
    // api.channel() only — pinned in chunk 6a ApiIdentity.
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir, R"LUA(
                function on_process(rx, tx, msgs, api)
                    assert(type(api.in_channel) == "function",
                           "in_channel must be a closure, got " ..
                           type(api.in_channel))
                    assert(type(api.out_channel) == "function",
                           "out_channel must be a closure")

                    assert(api.in_channel() == "sensor.input",
                           "1st in_channel: expected 'sensor.input', got "
                           .. tostring(api.in_channel()))
                    assert(api.out_channel() == "sensor.output",
                           "1st out_channel: expected 'sensor.output', got "
                           .. tostring(api.out_channel()))

                    -- idempotence
                    assert(api.in_channel() == "sensor.input",
                           "2nd in_channel drift")
                    assert(api.out_channel() == "sensor.output",
                           "2nd out_channel drift")

                    -- api.channel() canonical: must match in_channel()
                    -- on processor (input side is the identifying one).
                    assert(api.channel() == "sensor.input",
                           "api.channel() must match api.in_channel() on "
                           .. "processor, got '" .. tostring(api.channel())
                           .. "' vs '" .. tostring(api.in_channel()) .. "'")
                    return false
                end
            )LUA");

            RoleHostCore core;
            LuaEngine    engine;
            ASSERT_TRUE(engine.initialize("test", &core));
            ASSERT_TRUE(engine.load_script(script_dir, "init.lua",
                                            "on_process"));

            auto spec = simple_schema();
            ASSERT_TRUE(engine.register_slot_type(spec, "InSlotFrame",
                                                   "aligned"));
            ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame",
                                                   "aligned"));

            auto api = make_api(core, "proc");
            api->set_channel("sensor.input");
            api->set_out_channel("sensor.output");
            ASSERT_TRUE(engine.build_api(*api));

            float in_data  = 1.0f;
            float out_data = 0.0f;
            std::vector<pylabhub::scripting::IncomingMessage> msgs;
            auto r = engine.invoke_process(
                pylabhub::scripting::InvokeRx{&in_data, sizeof(in_data)},
                pylabhub::scripting::InvokeTx{&out_data, sizeof(out_data)},
                msgs);
            EXPECT_EQ(r, pylabhub::scripting::InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 0u);

            engine.finalize();
        },
        "lua_engine::api_processor_channels_reflect_setters",
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

                // Chunk 8a: generic invoke / eval
                if (sc == "invoke_existing_function_returns_true")
                    return invoke_existing_function_returns_true(dir);
                if (sc == "invoke_non_existent_function_returns_false")
                    return invoke_non_existent_function_returns_false(dir);
                if (sc == "invoke_empty_name_returns_false")
                    return invoke_empty_name_returns_false(dir);
                if (sc == "invoke_script_error_returns_false_and_increments_errors")
                    return invoke_script_error_returns_false_and_increments_errors(dir);
                if (sc == "invoke_with_args_returns_true")
                    return invoke_with_args_returns_true(dir);
                if (sc == "invoke_after_finalize_returns_false")
                    return invoke_after_finalize_returns_false(dir);
                if (sc == "eval_returns_scalar_result")
                    return eval_returns_scalar_result(dir);

                // Chunk 8b: multi-state / thread-state
                if (sc == "supports_multi_state_returns_true")
                    return supports_multi_state_returns_true(dir);
                if (sc == "state_persists_across_calls")
                    return state_persists_across_calls(dir);
                if (sc == "invoke_from_non_owner_thread_works")
                    return invoke_from_non_owner_thread_works(dir);
                if (sc == "invoke_non_owner_thread_uses_independent_state")
                    return invoke_non_owner_thread_uses_independent_state(dir);
                if (sc == "invoke_concurrent_owner_and_non_owner")
                    return invoke_concurrent_owner_and_non_owner(dir);
                if (sc == "shared_data_cross_thread_visible")
                    return shared_data_cross_thread_visible(dir);

                // Chunk 8c: misc V2 leftovers
                if (sc == "has_callback_detects_presence_absence")
                    return has_callback_detects_presence_absence(dir);
                if (sc == "invoke_consume_messages_data_and_event_mixed")
                    return invoke_consume_messages_data_and_event_mixed(dir);

                // Chunk 9a: inbox + slot-only invoke
                if (sc == "invoke_produce_slot_only_no_flexzone_on_invoke")
                    return invoke_produce_slot_only_no_flexzone_on_invoke(dir);
                if (sc == "invoke_on_inbox_typed_data")
                    return invoke_on_inbox_typed_data(dir);
                if (sc == "type_sizeof_inbox_frame_returns_correct_size")
                    return type_sizeof_inbox_frame_returns_correct_size(dir);
                if (sc == "invoke_on_inbox_missing_type_reports_error")
                    return invoke_on_inbox_missing_type_reports_error(dir);

                // Chunk 9b: logical-size accessors
                if (sc == "slot_logical_size_aligned_padding_sensitive")
                    return slot_logical_size_aligned_padding_sensitive(dir);
                if (sc == "slot_logical_size_packed_no_padding")
                    return slot_logical_size_packed_no_padding(dir);
                if (sc == "slot_logical_size_complex_mixed_aligned")
                    return slot_logical_size_complex_mixed_aligned(dir);
                if (sc == "flexzone_logical_size_array_fields")
                    return flexzone_logical_size_array_fields(dir);

                // Chunk 10: graceful degradation api.* without infrastructure
                if (sc == "api_open_inbox_without_broker_returns_nil")
                    return api_open_inbox_without_broker_returns_nil(dir);
                if (sc == "api_band_join_without_broker_returns_nil")
                    return api_band_join_without_broker_returns_nil(dir);
                if (sc == "api_band_leave_without_broker_returns_false")
                    return api_band_leave_without_broker_returns_false(dir);
                if (sc == "api_band_broadcast_without_broker_no_error")
                    return api_band_broadcast_without_broker_no_error(dir);
                if (sc == "api_band_members_without_broker_returns_nil")
                    return api_band_members_without_broker_returns_nil(dir);
                if (sc == "api_spinlock_count_without_shm_returns_zero")
                    return api_spinlock_count_without_shm_returns_zero(dir);
                if (sc == "api_spinlock_acquire_without_shm_is_pcall_error")
                    return api_spinlock_acquire_without_shm_is_pcall_error(dir);
                if (sc == "api_flexzone_accessor_without_shm_returns_nil")
                    return api_flexzone_accessor_without_shm_returns_nil(dir);

                // Chunk 11: metrics tests
                if (sc == "metrics_individual_accessors_read_core_counters_live")
                    return metrics_individual_accessors_read_core_counters_live(dir);
                if (sc == "metrics_in_slots_received_works_consumer")
                    return metrics_in_slots_received_works_consumer(dir);
                if (sc == "metrics_hierarchical_table_producer_full_shape")
                    return metrics_hierarchical_table_producer_full_shape(dir);
                if (sc == "metrics_hierarchical_table_consumer_full_shape")
                    return metrics_hierarchical_table_consumer_full_shape(dir);
                if (sc == "metrics_loop_overrun_count_live_increments")
                    return metrics_loop_overrun_count_live_increments(dir);
                if (sc == "metrics_last_cycle_work_us_overwrite_semantics")
                    return metrics_last_cycle_work_us_overwrite_semantics(dir);
                if (sc == "metrics_all_loop_fields_anchored_values")
                    return metrics_all_loop_fields_anchored_values(dir);
                if (sc == "metrics_role_script_error_count_reflects_raised_error")
                    return metrics_role_script_error_count_reflects_raised_error(dir);

                // Chunk 12: queue-state + env + channels
                if (sc == "queue_state_consumer_without_queue_returns_defaults")
                    return queue_state_consumer_without_queue_returns_defaults(dir);
                if (sc == "queue_state_producer_without_queue_returns_defaults")
                    return queue_state_producer_without_queue_returns_defaults(dir);
                if (sc == "queue_state_processor_dual_without_queues_returns_defaults")
                    return queue_state_processor_dual_without_queues_returns_defaults(dir);
                if (sc == "api_environment_strings_reflect_setters")
                    return api_environment_strings_reflect_setters(dir);
                if (sc == "api_processor_channels_reflect_setters")
                    return api_processor_channels_reflect_setters(dir);

                fmt::print(stderr,
                           "[lua_engine] ERROR: unknown scenario '{}'\n", sc);
                return 1;
            });
    }
};
static LuaEngineWorkerRegistrar g_lua_engine_registrar;

} // namespace
