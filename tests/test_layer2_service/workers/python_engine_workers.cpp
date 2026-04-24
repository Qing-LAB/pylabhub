/**
 * @file python_engine_workers.cpp
 * @brief PythonEngine worker bodies — Pattern 3 test bodies.
 *
 * Layer scope: L2 (scripting). Each body constructs PythonEngine +
 * RoleHostCore + RoleAPIBase and exercises the ScriptEngine contract
 * using raw memory buffers through InvokeTx/InvokeRx — no L3
 * ShmQueue/ZmqQueue/DataBlock/Broker involvement.
 *
 * Mirrors workers/lua_engine_workers.cpp with Python-specific
 * adjustments. See that file for detailed design rationale; this
 * file adopts the same helpers (setup_role_engine, script_worker,
 * produce/consume/process_worker_with_script) and the same
 * chunk-local review-and-augment methodology.
 */
#include "python_engine_workers.h"

#include "python_engine.hpp"
#include "utils/engine_module_params.hpp"
#include "utils/role_api_base.hpp"
#include "utils/role_host_core.hpp"
#include "utils/schema_utils.hpp"

#include "plh_service.hpp"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"
#include "test_schema_helpers.h"
#include "test_sync_utils.h"

#include <fmt/core.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using pylabhub::hub::FieldDef;
using pylabhub::hub::SchemaSpec;
using pylabhub::scripting::IncomingMessage;
using pylabhub::scripting::InvokeResult;
using pylabhub::scripting::InvokeRx;
using pylabhub::scripting::InvokeTx;
using pylabhub::scripting::PythonEngine;
using pylabhub::scripting::RoleAPIBase;
using pylabhub::scripting::RoleHostCore;
using pylabhub::tests::all_types_schema;
using pylabhub::tests::complex_mixed_schema;
using pylabhub::tests::fz_array_schema;
using pylabhub::tests::helper::run_gtest_worker;
using pylabhub::tests::multifield_schema;
using pylabhub::tests::padding_schema;
using pylabhub::tests::simple_schema;
using pylabhub::utils::Logger;

namespace pylabhub::tests::worker
{
namespace python_engine
{
namespace
{

/// Writes the given Python source to `<dir>/script/python/__init__.py`.
/// Creates the script/python subdirectory if needed (matching what
/// PythonEngine::load_script expects — a package layout).
void write_script(const fs::path &dir, const std::string &content)
{
    const fs::path pkg = dir / "script" / "python";
    fs::create_directories(pkg);
    std::ofstream f(pkg / "__init__.py");
    f << content;
}

/// Produces a fresh RoleAPIBase wired to the given core.  Mirrors
/// the original fixture's make_api helper.  uid is role-tagged so
/// Python scripts can distinguish between role contexts if needed.
std::unique_ptr<RoleAPIBase> make_api(RoleHostCore &core,
                                      const std::string &tag = "prod")
{
    std::string uid;
    if (tag == "prod") uid = "prod.testengine.uid00000001";
    else if (tag == "cons") uid = "cons.testengine.uid00000001";
    else if (tag == "proc") uid = "proc.testengine.uid00000001";
    else                    uid = "TEST-" + tag + "-00000001";
    auto api = std::make_unique<RoleAPIBase>(core, tag, uid);
    api->set_name("TestEngine");
    api->set_channel("test.channel");
    api->set_log_level("error");
    api->set_stop_on_script_error(false);
    return api;
}

/// Which role the PythonEngine is being set up for.  Per-role
/// defaults: required callback, API tag, default slot registrations.
enum class RoleKind
{
    Producer,   ///< on_produce, tag "prod", register OutSlotFrame
    Consumer,   ///< on_consume, tag "cons", register InSlotFrame
    Processor,  ///< on_process, tag "proc", register InSlotFrame + OutSlotFrame
};

/// Composes set_python_venv + initialize + load_script +
/// register_slot_type + build_api.  The Python variant differs from
/// Lua's setup_role_engine in:
///   (a) engine.set_python_venv("") call before initialize
///   (b) script package path is <dir>/script/python/, not just <dir>
bool setup_role_engine(PythonEngine &engine, RoleHostCore &core,
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

    engine.set_python_venv("");
    if (!engine.initialize("test", &core))
        return false;
    if (!engine.load_script(dir / "script" / "python", "__init__.py",
                             required_cb))
        return false;
    auto spec = simple_schema();
    if (register_in && !engine.register_slot_type(spec, "InSlotFrame", "aligned"))
        return false;
    if (register_out && !engine.register_slot_type(spec, "OutSlotFrame", "aligned"))
        return false;
    api_out = make_api(core, tag);
    return engine.build_api(*api_out);
}

/// Unified script-worker template: writes __init__.py with
/// `py_source`, sets up a PythonEngine for the given role, calls
/// body(engine, core), finalizes.
template <typename F>
int script_worker(const std::string &dir, const char *scenario_name,
                  RoleKind kind, const std::string &py_source, F &&body)
{
    return run_gtest_worker(
        [&]()
        {
            const fs::path script_dir(dir);
            write_script(script_dir, py_source);

            RoleHostCore  core;
            PythonEngine  engine;
            std::unique_ptr<RoleAPIBase> api;
            ASSERT_TRUE(setup_role_engine(engine, core, script_dir, api, kind));

            body(engine, core);

            engine.finalize();
        },
        scenario_name, Logger::GetLifecycleModule());
}

template <typename F>
int produce_worker_with_script(const std::string &dir, const char *name,
                               const std::string &py_source, F &&body)
{
    return script_worker(dir, name, RoleKind::Producer,
                         py_source, std::forward<F>(body));
}
template <typename F>
int consume_worker_with_script(const std::string &dir, const char *name,
                               const std::string &py_source, F &&body)
{
    return script_worker(dir, name, RoleKind::Consumer,
                         py_source, std::forward<F>(body));
}
template <typename F>
int process_worker_with_script(const std::string &dir, const char *name,
                               const std::string &py_source, F &&body)
{
    return script_worker(dir, name, RoleKind::Processor,
                         py_source, std::forward<F>(body));
}

/// Fill the role-agnostic fields of an EngineModuleParams that every
/// chunk 15/16 FullStartup test uses identically: engine+api pointers,
/// tag, script_dir (with /script/python suffix), entry_point, and
/// required_callback.  Each test still sets its own schema/packing
/// fields directly — the bits that legitimately differ per test.
inline void fill_base_params(pylabhub::scripting::EngineModuleParams &params,
                             PythonEngine                           &engine,
                             RoleAPIBase                            *api,
                             const fs::path                         &script_dir,
                             std::string                             tag,
                             std::string                             required_callback)
{
    params.engine            = &engine;
    params.api               = api;
    params.tag               = std::move(tag);
    params.script_dir        = script_dir / "script" / "python";
    params.entry_point       = "__init__.py";
    params.required_callback = std::move(required_callback);
}

} // namespace

// ── Lifecycle ───────────────────────────────────────────────────────────────

int full_lifecycle(const std::string &dir)
{
    // Strengthened vs V2 FullLifecycle.  V2 had empty `pass` bodies in
    // on_init/on_stop — a no-op engine implementation would pass.
    //
    // This body reports a metric from each callback via
    // api.report_metric (pybind11-bound at producer_api.cpp),
    // then asserts core.custom_metrics_snapshot() contains both
    // keys post-invoke.  Source-traced:
    //   - invoke_on_init  (python_engine.cpp): acquires
    //     GIL, calls py_on_init_(api_obj_); catches py::error_already_set.
    //   - invoke_on_stop  (python_engine.cpp): same shape.
    // Both dispatch to the user's Python function only if
    // is_callable(py_on_init_/py_on_stop_).  api_obj_ is set at
    // build_api time (python_engine.cpp) as the
    // role-specific C++ wrapper (ProducerAPI/ConsumerAPI/ProcessorAPI)
    // that exposes report_metric as a pybind11 method.
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir, R"PY(
def on_produce(tx, msgs, api):
    return True

def on_init(api):
    api.report_metric("on_init_called", 1.0)

def on_stop(api):
    api.report_metric("on_stop_called", 1.0)
)PY");

            RoleHostCore  core;
            PythonEngine  engine;
            std::unique_ptr<RoleAPIBase> api;
            ASSERT_TRUE(setup_role_engine(engine, core, script_dir, api,
                                           RoleKind::Producer));

            auto before = core.custom_metrics_snapshot();
            EXPECT_EQ(before.count("on_init_called"), 0u);
            EXPECT_EQ(before.count("on_stop_called"), 0u);

            engine.invoke_on_init();
            auto after_init = core.custom_metrics_snapshot();
            EXPECT_EQ(after_init.count("on_init_called"), 1u)
                << "on_init must have dispatched into the Python runtime";
            EXPECT_EQ(after_init.count("on_stop_called"), 0u);

            engine.invoke_on_stop();
            auto after_stop = core.custom_metrics_snapshot();
            EXPECT_EQ(after_stop.count("on_init_called"), 1u);
            EXPECT_EQ(after_stop.count("on_stop_called"), 1u)
                << "on_stop must have dispatched into the Python runtime";

            EXPECT_EQ(engine.script_error_count(), 0u);
            engine.finalize();
        },
        "python_engine::full_lifecycle", Logger::GetLifecycleModule());
}

int initialize_and_finalize_succeeds(const std::string &dir)
{
    // Renamed from V2 InitializeFailsGracefully: the V2 body was
    // single initialize + finalize (happy-path), but the name
    // suggested failure-injection testing.  PythonEngine has no
    // public failure-injection hook (verified by reading
    // python_engine.hpp's public API: set_python_venv + initialize +
    // load_script + build_api + finalize are the only setup
    // methods).  The new name reflects what the body actually tests.
    // set_python_venv("") pins empty-venv → embedded interpreter
    // (no venv activation), per PythonEngine's set_python_venv
    // inline at python_engine.hpp.
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            (void)script_dir;  // no script loaded

            RoleHostCore core;
            PythonEngine engine;
            engine.set_python_venv("");
            ASSERT_TRUE(engine.initialize("test", &core));
            engine.finalize();
        },
        "python_engine::initialize_and_finalize_succeeds",
        Logger::GetLifecycleModule());
}

// ── Type registration ──────────────────────────────────────────────────────

int register_slot_type_sizeof_correct(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir,
                         "def on_produce(tx, msgs, api):\n"
                         "    return True\n");

            RoleHostCore core;
            PythonEngine engine;
            engine.set_python_venv("");
            ASSERT_TRUE(engine.initialize("test", &core));
            ASSERT_TRUE(engine.load_script(script_dir / "script" / "python",
                                            "__init__.py", "on_produce"));

            auto spec = simple_schema();
            ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame",
                                                   "aligned"));
            EXPECT_EQ(engine.type_sizeof("OutSlotFrame"), 4u)
                << "simple_schema is a single float32 field";

            // Reach the full lifecycle state for uniformity across
            // chunk-1 tests — trivial safety net.
            auto api = make_api(core);
            ASSERT_TRUE(engine.build_api(*api));
            engine.finalize();
        },
        "python_engine::register_slot_type_sizeof_correct",
        Logger::GetLifecycleModule());
}

int register_slot_type_multi_field(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir,
                         "def on_produce(tx, msgs, api):\n"
                         "    return True\n");

            RoleHostCore core;
            PythonEngine engine;
            engine.set_python_venv("");
            ASSERT_TRUE(engine.initialize("test", &core));
            ASSERT_TRUE(engine.load_script(script_dir / "script" / "python",
                                            "__init__.py", "on_produce"));

            SchemaSpec spec;
            spec.has_schema = true;
            spec.fields.push_back({"x", "float32", 1, 0});
            spec.fields.push_back({"y", "float32", 1, 0});
            spec.fields.push_back({"z", "float32", 1, 0});
            ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame",
                                                   "aligned"));
            EXPECT_EQ(engine.type_sizeof("OutSlotFrame"), 12u)
                << "3 x float32 = 12 bytes";

            auto api = make_api(core);
            ASSERT_TRUE(engine.build_api(*api));
            engine.finalize();
        },
        "python_engine::register_slot_type_multi_field",
        Logger::GetLifecycleModule());
}

int register_slot_type_packed_packing(const std::string &dir)
{
    // Pins the user-observable outcome: registering a bool+int32
    // schema with "packed" packing produces the hand-verified 5-byte
    // layout (bool(1) + int32(4), no padding).
    //
    // Why this is enough (no separate "aligned" side-check):
    // register_slot_type (python_engine.cpp) internally
    // cross-validates ctypes_sizeof(type) against
    // compute_schema_size(spec, packing).  If a regression silently
    // ignored the packing arg and always built aligned (size 8 for
    // this schema), the internal check would fire —
    //   actual=8, expected=5 → "size mismatch" ERROR + return false —
    // and ASSERT_TRUE(register_slot_type(...)) here would fail.
    //
    // So the engine's internal cross-validation IS the real guard
    // against silent packing-ignore.  This test's unique value is
    // pinning the size anchor (5 bytes) for this specific
    // bool+int32 combination so that a compute_schema_size drift
    // for this schema would surface.
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir,
                         "def on_produce(tx, msgs, api):\n"
                         "    return True\n");

            RoleHostCore core;
            PythonEngine engine;
            engine.set_python_venv("");
            ASSERT_TRUE(engine.initialize("test", &core));
            ASSERT_TRUE(engine.load_script(script_dir / "script" / "python",
                                            "__init__.py", "on_produce"));

            SchemaSpec spec;
            spec.has_schema = true;
            spec.fields.push_back({"flag", "bool",  1, 0});
            spec.fields.push_back({"val",  "int32", 1, 0});

            ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame",
                                                   "packed"));
            EXPECT_EQ(engine.type_sizeof("OutSlotFrame"), 5u)
                << "packed: bool(1) + int32(4) = 5 — hand-verified "
                   "anchor for this schema";

            auto api = make_api(core);
            ASSERT_TRUE(engine.build_api(*api));

            // After build_api, the SlotFrame alias (created by
            // build_api_ for producer role) must use the same packing
            // that register_slot_type was called with — not silently
            // default to aligned.  Pins the register_slot_type
            // spec.packing normalization (python_engine.cpp) — without
            // it the alias would be 8 bytes (aligned), mismatching the
            // 5-byte packed OutSlotFrame.
            EXPECT_EQ(engine.type_sizeof("SlotFrame"), 5u)
                << "SlotFrame alias must inherit packed packing from "
                   "the OutSlotFrame registration; a mismatch here "
                   "means build_api_ built the alias with different "
                   "packing than the original slot";
            engine.finalize();
        },
        "python_engine::register_slot_type_packed_packing",
        Logger::GetLifecycleModule());
}

int register_slot_type_has_schema_false_returns_false(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir,
                         "def on_produce(tx, msgs, api):\n"
                         "    return True\n");

            RoleHostCore core;
            PythonEngine engine;
            engine.set_python_venv("");
            ASSERT_TRUE(engine.initialize("test", &core));
            ASSERT_TRUE(engine.load_script(script_dir / "script" / "python",
                                            "__init__.py", "on_produce"));

            SchemaSpec spec;
            spec.has_schema = false;
            EXPECT_FALSE(engine.register_slot_type(spec, "OutSlotFrame",
                                                    "aligned"));

            // Additional invariant: a failed registration must not
            // leave the engine wedged.  build_api still succeeds
            // (alias creation in build_api_ is gated on `.is_none()`
            // which holds when register_slot_type bailed early) and
            // type_sizeof("OutSlotFrame") must stay 0 (no type was
            // successfully cached).  This pins that an engine which
            // rejected a bad register call is still useable for a
            // correct subsequent flow.
            auto api = make_api(core);
            ASSERT_TRUE(engine.build_api(*api))
                << "build_api must not wedge after a register failure";
            EXPECT_EQ(engine.type_sizeof("OutSlotFrame"), 0u)
                << "failed register must not cache a partial type";
            engine.finalize();
        },
        "python_engine::register_slot_type_has_schema_false_returns_false",
        Logger::GetLifecycleModule());
}

int register_slot_type_all_supported_types(const std::string &dir)
{
    // NEW coverage fill (parallel to the Lua chunk-1 gap-fill).
    // V2 never registered bool/int8/int16/uint64 etc. via the Python
    // type-registration path, leaving the ctypes-type-build branches
    // for those types untested at L2.
    //
    // Unique coverage vs the engine's internal check:
    // register_slot_type internally cross-validates ctypes_sizeof vs
    // compute_schema_size (python_engine.cpp), so a size-
    // mismatch bug WOULD be caught by the engine itself.  But a
    // DIFFERENT bug class — missing or wrong ctypes-type-dispatcher
    // entry for a specific scalar (e.g. int8 case accidentally
    // dropped from json_type_to_ctypes at python_helpers.hpp) —
    // gets caught only by actually exercising every type.  That's
    // what this test does.
    //
    // Single packing ("aligned") is sufficient: the internal check
    // guards against per-packing layout bugs; this test guards
    // against per-type-dispatcher bugs.  No need to test both.
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir,
                         "def on_produce(tx, msgs, api):\n"
                         "    return True\n");

            RoleHostCore core;
            PythonEngine engine;
            engine.set_python_venv("");
            ASSERT_TRUE(engine.initialize("test", &core));
            ASSERT_TRUE(engine.load_script(script_dir / "script" / "python",
                                            "__init__.py", "on_produce"));

            auto spec = all_types_schema();
            ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame",
                                                   "aligned"))
                << "every scalar type in all_types_schema must build a "
                   "ctypes struct — a missing dispatcher branch for any "
                   "scalar type would fail here";
            EXPECT_EQ(engine.type_sizeof("OutSlotFrame"),
                      pylabhub::hub::compute_schema_size(spec, "aligned"))
                << "engine ctypes_sizeof must match the canonical "
                   "compute_schema_size for every scalar type";

            auto api = make_api(core);
            ASSERT_TRUE(engine.build_api(*api));
            engine.finalize();
        },
        "python_engine::register_slot_type_all_supported_types",
        Logger::GetLifecycleModule());
}

// ── Alias creation ─────────────────────────────────────────────────────────

int alias_slot_frame_producer(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir,
                         "def on_produce(tx, msgs, api):\n"
                         "    return True\n");

            RoleHostCore core;
            PythonEngine engine;
            engine.set_python_venv("");
            ASSERT_TRUE(engine.initialize("test", &core));
            ASSERT_TRUE(engine.load_script(script_dir / "script" / "python",
                                            "__init__.py", "on_produce"));

            auto spec = simple_schema();
            ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame",
                                                   "aligned"));

            auto api = make_api(core);
            ASSERT_TRUE(engine.build_api(*api));

            EXPECT_EQ(engine.type_sizeof("SlotFrame"),
                      engine.type_sizeof("OutSlotFrame"))
                << "producer build_api must expose OutSlotFrame under "
                   "the role-agnostic 'SlotFrame' alias";
            EXPECT_GT(engine.type_sizeof("SlotFrame"), 0u);

            engine.finalize();
        },
        "python_engine::alias_slot_frame_producer",
        Logger::GetLifecycleModule());
}

int alias_slot_frame_consumer(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir,
                         "def on_consume(rx, msgs, api):\n"
                         "    return True\n");

            RoleHostCore core;
            PythonEngine engine;
            engine.set_python_venv("");
            ASSERT_TRUE(engine.initialize("test", &core));
            ASSERT_TRUE(engine.load_script(script_dir / "script" / "python",
                                            "__init__.py", "on_consume"));

            auto spec = simple_schema();
            ASSERT_TRUE(engine.register_slot_type(spec, "InSlotFrame",
                                                   "aligned"));

            auto api = make_api(core, "cons");
            ASSERT_TRUE(engine.build_api(*api));

            EXPECT_EQ(engine.type_sizeof("SlotFrame"),
                      engine.type_sizeof("InSlotFrame"))
                << "consumer build_api must expose InSlotFrame under "
                   "the role-agnostic 'SlotFrame' alias";
            EXPECT_GT(engine.type_sizeof("SlotFrame"), 0u);

            engine.finalize();
        },
        "python_engine::alias_slot_frame_consumer",
        Logger::GetLifecycleModule());
}

int alias_no_alias_processor(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir,
                         "def on_process(rx, tx, msgs, api):\n"
                         "    return True\n");

            RoleHostCore core;
            PythonEngine engine;
            engine.set_python_venv("");
            ASSERT_TRUE(engine.initialize("test", &core));
            ASSERT_TRUE(engine.load_script(script_dir / "script" / "python",
                                            "__init__.py", "on_process"));

            auto spec = simple_schema();
            ASSERT_TRUE(engine.register_slot_type(spec, "InSlotFrame",
                                                   "aligned"));
            ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame",
                                                   "aligned"));

            auto api = make_api(core, "proc");
            ASSERT_TRUE(engine.build_api(*api));

            // Processor: no 'SlotFrame' alias (ambiguous between
            // InSlotFrame and OutSlotFrame).
            EXPECT_EQ(engine.type_sizeof("SlotFrame"), 0u)
                << "processor must NOT have a bare 'SlotFrame' alias";
            EXPECT_GT(engine.type_sizeof("InSlotFrame"), 0u);
            EXPECT_GT(engine.type_sizeof("OutSlotFrame"), 0u);

            engine.finalize();
        },
        "python_engine::alias_no_alias_processor",
        Logger::GetLifecycleModule());
}

int alias_flex_frame_producer(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir,
                         "def on_produce(tx, msgs, api):\n"
                         "    return True\n");

            RoleHostCore core;
            PythonEngine engine;
            engine.set_python_venv("");
            ASSERT_TRUE(engine.initialize("test", &core));
            ASSERT_TRUE(engine.load_script(script_dir / "script" / "python",
                                            "__init__.py", "on_produce"));

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
        "python_engine::alias_flex_frame_producer",
        Logger::GetLifecycleModule());
}

int alias_producer_no_fz_no_flex_frame_alias(const std::string &dir)
{
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir,
                         "def on_produce(tx, msgs, api):\n"
                         "    return True\n");

            RoleHostCore core;
            PythonEngine engine;
            engine.set_python_venv("");
            ASSERT_TRUE(engine.initialize("test", &core));
            ASSERT_TRUE(engine.load_script(script_dir / "script" / "python",
                                            "__init__.py", "on_produce"));

            auto spec = simple_schema();
            ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame",
                                                   "aligned"));
            // Deliberately NO OutFlexFrame registered.

            auto api = make_api(core);
            ASSERT_TRUE(engine.build_api(*api));

            EXPECT_GT(engine.type_sizeof("SlotFrame"), 0u)
                << "SlotFrame alias exists (OutSlotFrame registered)";
            EXPECT_EQ(engine.type_sizeof("FlexFrame"), 0u)
                << "FlexFrame alias must not exist when no OutFlexFrame "
                   "was registered";

            engine.finalize();
        },
        "python_engine::alias_producer_no_fz_no_flex_frame_alias",
        Logger::GetLifecycleModule());
}

// ── Engine-internal dispatcher contract (chunk 1 gap-fill) ─────────────────

int supports_multi_state_returns_false(const std::string &dir)
{
    // Pins PythonEngine's supports_multi_state() contract.  Per
    // script_engine.hpp this flag tells the engine's own
    // threading-dispatch path whether to route non-owner-thread
    // invoke() calls directly (true, Lua) or queue them to the
    // owner thread (false, Python).  It is NOT exposed to scripts —
    // the engine handles threading transparently.
    //
    // Python returns false (python_engine.hpp): single
    // interpreter, non-owner requests queued to owner thread.  A
    // regression flipping this to true (e.g., a subinterpreter
    // refactor) without updating the dispatcher would silently
    // violate the GIL-constrained threading model.  This test
    // surfaces that immediately.
    //
    // Tested twice: once before load_script (pristine engine, pre-
    // any role setup) and once after build_api (fully-built engine),
    // to pin that the property is STRUCTURAL (not dependent on
    // role/script/schema state).
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir,
                         "def on_produce(tx, msgs, api):\n"
                         "    return True\n");

            RoleHostCore core;
            PythonEngine engine;
            engine.set_python_venv("");

            // Pre-initialize: property is structural, must already
            // hold before any state is built.  This would catch a
            // regression where the flag is set at build_api time
            // (post-facto) rather than being a compile-time constant.
            EXPECT_FALSE(engine.supports_multi_state())
                << "supports_multi_state must be false for Python pre-"
                   "initialize — it is a structural engine property, "
                   "not state-dependent";

            ASSERT_TRUE(engine.initialize("test", &core));
            ASSERT_TRUE(engine.load_script(script_dir / "script" / "python",
                                            "__init__.py", "on_produce"));

            auto spec = simple_schema();
            ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame",
                                                   "aligned"));

            auto api = make_api(core);
            ASSERT_TRUE(engine.build_api(*api));

            // Post-build_api: still false.
            EXPECT_FALSE(engine.supports_multi_state())
                << "supports_multi_state must remain false after full "
                   "engine setup — a regression that flipped it based "
                   "on role or schema would surface here";

            engine.finalize();
        },
        "python_engine::supports_multi_state_returns_false",
        Logger::GetLifecycleModule());
}

// ── invoke_produce (chunk 2) ───────────────────────────────────────────────
//
// Exercises the on_produce(tx, msgs, api) return-value contract.
// Source-traced against python_engine.cpp (invoke_produce) and
// python_engine.cpp (parse_return_value_).  Python-specific
// vs Lua:
//
//   - parse_return_value_ checks py::isinstance<py::bool_> first
//     (IMPORTANT: isinstance(True, int) is True in Python, so bool must
//     be checked before int; though the current implementation also
//     does not check int at all — it falls through to the generic
//     non-boolean ERROR path).
//   - None return → ERROR log (unified with Lua's nil-return severity).
//   - Non-boolean-non-None → ERROR log with the Python type name
//     (e.g. "int", "str").  Both paths: inc_script_error_count, return
//     InvokeResult::Error, and (if stop_on_script_error) request_stop.
//
// Harness note: ExpectWorkerOk's expected_error_substrings matches
// against captured ERROR logs from the worker subprocess.  Worth
// pinning the diagnostic text for all three error paths (None, int,
// str) so a reworded log that still preserves the InvokeResult would
// be caught.

int invoke_produce_commit_on_true(const std::string &dir)
{
    // Strengthened vs V2 InvokeProduce_CommitOnTrue by asserting
    // script_error_count == 0 post-invoke.  The V2 body only checked
    // result == Commit and buf == 42.0 — a regression where the engine
    // silently logged a script error on the Commit path would slip
    // through.  Pinning the counter at 0 closes that gap.
    return produce_worker_with_script(
        dir, "python_engine::invoke_produce_commit_on_true",
        R"PY(
def on_produce(tx, msgs, api):
    if tx.slot is not None:
        tx.slot.value = 42.0
    return True
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            float buf = 0.0f;
            std::vector<IncomingMessage> msgs;
            auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf)},
                                                 msgs);
            EXPECT_EQ(result, InvokeResult::Commit);
            EXPECT_FLOAT_EQ(buf, 42.0f);
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "commit path must not emit script errors";
        });
}

int invoke_produce_discard_on_false(const std::string &dir)
{
    // Strengthened vs V2 InvokeProduce_DiscardOnFalse.  V2 inited
    // buf = 0.0f — if the engine secretly zeroed buf on Discard, the
    // test would still pass (0.0 == 0.0 is indistinguishable from
    // "uninitialized sentinel").
    //
    // This body inits buf to a non-default sentinel (777.0f) that the
    // Python script does NOT write, then asserts buf is STILL 777.0f
    // post-invoke.  Catches any engine-internal write on the Discard
    // path.
    return produce_worker_with_script(
        dir, "python_engine::invoke_produce_discard_on_false",
        R"PY(
def on_produce(tx, msgs, api):
    # deliberately do NOT touch tx.slot; return False
    return False
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            float buf = 777.0f;  // sentinel — Python script doesn't write
            std::vector<IncomingMessage> msgs;
            auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf)},
                                                 msgs);
            EXPECT_EQ(result, InvokeResult::Discard);
            EXPECT_FLOAT_EQ(buf, 777.0f)
                << "engine must not write buf on Discard when Python didn't";
            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

int invoke_produce_none_return_is_error(const std::string &dir)
{
    // Pins the None-return diagnostic.  parse_return_value_ logs
    // ERROR "<cb> returned None — explicit 'return True' or 'return
    // False' is required. Treating as error." then increments the
    // error counter and returns InvokeResult::Error.  A worded rewrite
    // that preserved the InvokeResult (keeping this test green) would
    // be caught by the expected_error_substrings check at the parent.
    return produce_worker_with_script(
        dir, "python_engine::invoke_produce_none_return_is_error",
        R"PY(
def on_produce(tx, msgs, api):
    pass  # no explicit return -> None -> error
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            float buf = 0.0f;
            std::vector<IncomingMessage> msgs;
            auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf)},
                                                 msgs);
            EXPECT_EQ(result, InvokeResult::Error)
                << "missing return is an error — must be explicit True/False";
            EXPECT_EQ(engine.script_error_count(), 1u);
        });
}

int invoke_produce_none_slot(const std::string &dir)
{
    // Strengthened vs V2 InvokeProduce_NoneSlot.  V2 relied on a Python
    // `assert tx.slot is None` inside on_produce but never verified
    // from the C++ side whether that assert passed.  A failed assert
    // would raise AssertionError, route through on_python_error_,
    // and return InvokeResult::Error — but the V2 test asserted
    // Discard, which would fail in the wrong way (AssertionError has
    // nothing to do with the slot contract).
    //
    // This body pins script_error_count == 0 to confirm the Python
    // assert passed, i.e. the engine really did pass None for
    // InvokeTx{nullptr, 0}.
    return produce_worker_with_script(
        dir, "python_engine::invoke_produce_none_slot",
        R"PY(
def on_produce(tx, msgs, api):
    assert tx.slot is None, "expected None slot"
    return False
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            std::vector<IncomingMessage> msgs;
            auto result = engine.invoke_produce(InvokeTx{nullptr, 0}, msgs);
            EXPECT_EQ(result, InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "the Python `assert tx.slot is None` must have passed; "
                   "a non-zero count means the engine dispatched a non-None "
                   "slot when given InvokeTx{nullptr, 0}";
        });
}

int invoke_produce_script_error(const std::string &dir)
{
    return produce_worker_with_script(
        dir, "python_engine::invoke_produce_script_error",
        R"PY(
def on_produce(tx, msgs, api):
    raise RuntimeError("intentional error")
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            float buf = 0.0f;
            std::vector<IncomingMessage> msgs;
            EXPECT_EQ(engine.script_error_count(), 0u);
            auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf)},
                                                 msgs);
            EXPECT_EQ(result, InvokeResult::Error);
            EXPECT_EQ(engine.script_error_count(), 1u);
        });
}

int invoke_produce_wrong_return_type_is_error(const std::string &dir)
{
    // Returning an int (not bool) must be rejected.  IMPORTANT:
    // isinstance(True, int) is True in Python — if a future
    // parse_return_value_ refactor replaced the bool check with an
    // int check, `return 42` would silently be treated as
    // Commit-on-truthy.  Pinning this behaviour explicitly guards
    // against that regression.
    return produce_worker_with_script(
        dir, "python_engine::invoke_produce_wrong_return_type_is_error",
        R"PY(
def on_produce(tx, msgs, api):
    return 42
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            float buf = 0.0f;
            std::vector<IncomingMessage> msgs;
            auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf)},
                                                 msgs);
            EXPECT_EQ(result, InvokeResult::Error)
                << "returning an int must be Error, not Commit";
            EXPECT_EQ(engine.script_error_count(), 1u);
        });
}

int invoke_produce_wrong_return_string_is_error(const std::string &dir)
{
    // Returning a non-empty str (which is truthy in Python) must be
    // rejected.  Guards against a truthiness-based check slipping in.
    return produce_worker_with_script(
        dir, "python_engine::invoke_produce_wrong_return_string_is_error",
        R"PY(
def on_produce(tx, msgs, api):
    return "ok"
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            float buf = 0.0f;
            std::vector<IncomingMessage> msgs;
            auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf)},
                                                 msgs);
            EXPECT_EQ(result, InvokeResult::Error)
                << "returning a str must be Error, not Commit";
            EXPECT_EQ(engine.script_error_count(), 1u);
        });
}

// ── invoke_consume (chunk 3) ───────────────────────────────────────────────
//
// Consumer callback contract: `rx` is documented as "Input direction
// (read-only slot)".  The V2 ReceivesReadOnlySlot test asserted the
// name but never attempted a write — so the "read-only" half was
// untested.  Here it is split into:
//
//   - invoke_consume_receives_slot: happy-path + return-value dispatch
//   - invoke_consume_rx_slot_is_read_only (NEW gap-fill): actually
//     exercises the write → AttributeError path pinned on all four
//     observable channels (buf unchanged, InvokeResult::Error,
//     script_error_count++, and — at the parent level — the
//     "read-only slot" ERROR log substring).
//
// Python vs Lua: the read-only guard is a ctypes subclass with a
// __setattr__ override that raises AttributeError (python_helpers.hpp:
// 96-112).  Lua uses LuaJIT's `ffi.typeof("<Name> const*")` which
// raises a Lua error on write (lua_state.cpp).  Different
// runtime mechanics, same design contract: loud failure, never
// silent.  Python has one known limitation documented in
// python_helpers.hpp — mutation through array subscript
// (rx.slot.array[0] = X) is not blocked.  Tests here use
// simple_schema() (single scalar `value`) where the guard fires.
//
// invoke_consume returns InvokeResult but the production consumer
// data loop currently ignores it ("Currently ignored … reserved for
// future flow control"); tests still assert on it so a regression in
// the dispatch path is caught.

int invoke_consume_receives_slot(const std::string &dir)
{
    // Renamed from V2 InvokeConsume_ReceivesReadOnlySlot.  V2 only
    // checked script_error_count == 0 (the Python asserts passed).
    // Strengthened here to also assert result == Commit — catches
    // a regression in invoke_consume's return-value dispatch that
    // would be silent in the current consumer loop but surface the
    // moment flow-control uses the return value.
    return consume_worker_with_script(
        dir, "python_engine::invoke_consume_receives_slot",
        R"PY(
def on_consume(rx, msgs, api):
    assert rx.slot is not None, "expected non-None slot"
    assert abs(rx.slot.value - 99.5) < 0.01, \
        f"expected value ~99.5, got {rx.slot.value}"
    return True
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            float data = 99.5f;
            std::vector<IncomingMessage> msgs;
            auto result = engine.invoke_consume(InvokeRx{&data, sizeof(data)},
                                                 msgs);
            EXPECT_EQ(result, InvokeResult::Commit)
                << "on_consume returning True must map to Commit even though "
                   "the data loop currently ignores the return value";
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "the Python assertions must have passed — a non-zero "
                   "count means the engine passed the wrong value in rx.slot";
        });
}

int invoke_consume_none_slot(const std::string &dir)
{
    // Strengthened vs V2 InvokeConsume_NoneSlot (which only checked
    // script_error_count == 0).  Also asserts result == Commit.
    return consume_worker_with_script(
        dir, "python_engine::invoke_consume_none_slot",
        R"PY(
def on_consume(rx, msgs, api):
    assert rx.slot is None, "expected None"
    return True
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            std::vector<IncomingMessage> msgs;
            auto result = engine.invoke_consume(InvokeRx{nullptr, 0}, msgs);
            EXPECT_EQ(result, InvokeResult::Commit);
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "the Python `assert rx.slot is None` must have passed";
        });
}

int invoke_consume_script_error_detected(const std::string &dir)
{
    // Strengthened vs V2 InvokeConsume_ScriptErrorDetected (which only
    // checked script_error_count == 1).  Also asserts result == Error,
    // i.e. the engine translated the raised exception to Error, not
    // just bumped the counter and left the result indeterminate.
    return consume_worker_with_script(
        dir, "python_engine::invoke_consume_script_error_detected",
        R"PY(
def on_consume(rx, msgs, api):
    raise RuntimeError("consume error")
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            float data = 1.0f;
            std::vector<IncomingMessage> msgs;
            auto result = engine.invoke_consume(InvokeRx{&data, sizeof(data)},
                                                 msgs);
            EXPECT_EQ(result, InvokeResult::Error);
            EXPECT_EQ(engine.script_error_count(), 1u);
        });
}

int invoke_consume_rx_slot_is_read_only(const std::string &dir)
{
    // NEW gap-fill — verifies that the library implements rx
    // read-only as a LOUD failure, not a silent no-op.
    // Source-confirmed:
    //
    //   src/scripting/python_engine.cpp
    //     register_slot_type("InSlotFrame") →
    //     in_slot_type_ro_ = wrap_readonly_(type)
    //
    //   src/scripting/python_helpers.hpp (wrap_as_readonly_ctypes)
    //     Creates a subclass with __setattr__ that raises
    //     AttributeError("read-only slot: field '<name>' cannot be
    //     written (in_slot is a zero-copy SHM view -- use out_slot
    //     to write)").
    //
    //   src/scripting/python_engine.cpp (invoke_consume path)
    //     rx_ch.slot = make_slot_view_(..., in_slot_type_ro_, …,
    //                                   readonly=true);
    //
    //   src/scripting/python_engine.cpp (catch)
    //     py::error_already_set → on_python_error_ → ERROR log +
    //     inc_script_error_count + return InvokeResult::Error.
    //
    // Design rationale: a user whose buggy script does
    // `rx.slot.value = X` wants to know immediately — silent no-op
    // would let the broken script run, appear healthy, and corrupt
    // downstream logic that depended on the "modified" value.  The
    // library is correct to reject the write explicitly; this test
    // enforces the choice.
    //
    // Python-specific note: the read-only guard is override-of-
    // __setattr__ on the subclass.  It fires on field assignment
    // (rx.slot.value = X).  A known limitation documented at
    // python_helpers.hpp is that array-subscript mutation
    // (rx.slot.array[0] = X) is not blocked by this mechanism;
    // our simple_schema() has a single scalar field `value`, so
    // the __setattr__ guard applies fully here.
    return consume_worker_with_script(
        dir, "python_engine::invoke_consume_rx_slot_is_read_only",
        R"PY(
def on_consume(rx, msgs, api):
    # Attempt to mutate rx.slot.  The library's contract is that
    # this MUST raise AttributeError, which propagates to
    # InvokeResult::Error via the engine's catch path.
    rx.slot.value = 777.0
    return True  # unreachable if the write raises
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            float data = 99.5f;
            std::vector<IncomingMessage> msgs;
            auto result = engine.invoke_consume(InvokeRx{&data, sizeof(data)},
                                                 msgs);

            // Four coupled assertions that together pin the design:
            //
            //  (a) buf unchanged — the read-only invariant.
            EXPECT_FLOAT_EQ(data, 99.5f)
                << "rx.slot write must NOT propagate to the underlying "
                   "C buffer.  data==777.0 here would indicate the "
                   "__setattr__ override was bypassed.";

            //  (b) the library signals Error — loud, observable result.
            EXPECT_EQ(result, InvokeResult::Error)
                << "rx write must surface as InvokeResult::Error, not "
                   "Discard or Commit.  A silent no-op (Commit here) "
                   "would let broken scripts appear healthy while "
                   "silently producing incorrect data downstream.";

            //  (c) script-error counter tracks the raise — for metrics.
            EXPECT_EQ(engine.script_error_count(), 1u)
                << "the raised AttributeError must be counted so role "
                   "hosts' metrics reflect buggy scripts.";

            //  (d) the ERROR log is the fourth observable channel;
            //      parent's expected_error_substrings enforces the
            //      "read-only slot" fragment, so no C++-side
            //      assertion is needed here.
        });
}

// ── invoke_process (chunk 4) ───────────────────────────────────────────────
//
// Processor dual-slot callback.  The Python path
// (python_engine.cpp) builds BOTH an rx and a tx channel
// per iteration: rx via in_slot_type_ro_ (readonly subclass with
// __setattr__ override), tx via out_slot_type_ (writable).
//
// Processor role: always has an input channel by definition (a role
// without an input channel is a consumer, not a processor).  None
// rx.slot inside on_process represents runtime state (input queue
// timeout / no data); None tx.slot similarly represents "output
// queue has no slot right now" (backpressure).  Test names match
// the Lua sweep to keep the cross-engine contract explicit.

int invoke_process_dual_slots(const std::string &dir)
{
    // Strengthened vs V2 InvokeProcess_DualSlots:
    //
    //   a) Asserts script_error_count == 0 (catches a regression
    //      where the engine silently logs a script error alongside
    //      returning Commit).
    //
    //   b) Asserts the rx buffer was NOT mutated even though Python
    //      read rx.slot.value.  rx is read-only per the processor
    //      contract; the pre-conversion test only checked out_data,
    //      leaving an aliasing-bug regression undetectable.
    return process_worker_with_script(
        dir, "python_engine::invoke_process_dual_slots",
        R"PY(
def on_process(rx, tx, msgs, api):
    if rx.slot is not None and tx.slot is not None:
        tx.slot.value = rx.slot.value * 2.0
        return True
    return False
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            float in_data  = 21.0f;
            float out_data = 0.0f;
            std::vector<IncomingMessage> msgs;
            auto result = engine.invoke_process(
                InvokeRx{&in_data, sizeof(in_data)},
                InvokeTx{&out_data, sizeof(out_data)}, msgs);
            EXPECT_EQ(result, InvokeResult::Commit);
            EXPECT_FLOAT_EQ(out_data, 42.0f)
                << "Python computed tx.slot.value = rx.slot.value * 2.0";
            EXPECT_FLOAT_EQ(in_data, 21.0f)
                << "rx.slot is read-only — input buffer must not be "
                   "mutated by the dual-slot dispatch path";
            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

int invoke_process_both_slots_none(const std::string &dir)
{
    // Renamed vs V2 InvokeProcess_NoneInput.  The V2 name suggested
    // "processor with no input channel", which is semantically wrong
    // (such a role is a consumer).  The actual scenario: both rx and
    // tx paths produced None — input queue timed out AND output queue
    // has no slot.  Matches the Lua chunk 4 naming.
    return process_worker_with_script(
        dir, "python_engine::invoke_process_both_slots_none",
        R"PY(
def on_process(rx, tx, msgs, api):
    assert rx.slot is None, "expected None input (timeout / no data)"
    assert tx.slot is None, "expected None output (backpressure)"
    return False
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            std::vector<IncomingMessage> msgs;
            auto result = engine.invoke_process(
                InvokeRx{nullptr, 0},
                InvokeTx{nullptr, 0}, msgs);
            EXPECT_EQ(result, InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "Python `assert both None` must have passed; failure "
                   "means the engine dispatched a non-None slot for a "
                   "null/zero-size InvokeRx or InvokeTx";
        });
}

int invoke_process_rx_present_tx_none(const std::string &dir)
{
    // Renamed vs V2 InvokeProcess_InputOnlyNoOutput.  The V2 name
    // could be misread as "processor without output channel"; actual
    // scenario is "input has data, but the output queue is
    // backpressured / no slot available this iteration."  Matches
    // Lua chunk 4 naming.
    //
    // Strengthened: asserts script_error_count == 0 and in_data
    // unchanged.
    return process_worker_with_script(
        dir, "python_engine::invoke_process_rx_present_tx_none",
        R"PY(
def on_process(rx, tx, msgs, api):
    assert rx.slot is not None, "expected input data"
    assert tx.slot is None, "expected None output (backpressure)"
    return False
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            float in_data = 10.0f;
            std::vector<IncomingMessage> msgs;
            auto result = engine.invoke_process(
                InvokeRx{&in_data, sizeof(in_data)},
                InvokeTx{nullptr, 0}, msgs);
            EXPECT_EQ(result, InvokeResult::Discard);
            EXPECT_FLOAT_EQ(in_data, 10.0f)
                << "rx is still read-only in the rx-only-no-tx path";
            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

int invoke_process_rx_slot_is_read_only(const std::string &dir)
{
    // NEW gap-fill — the processor's dual-slot code path must enforce
    // the same loud-failure rx-read-only contract as the consumer
    // path (see invoke_consume_rx_slot_is_read_only doc block for the
    // full source-trace).  invoke_process is a separate engine entry
    // (python_engine.cpp builds rx_ch via in_slot_type_ro_
    // — the same readonly subclass the consumer path uses), so the
    // contract should hold the same way.
    //
    // Because the __setattr__ override raises AttributeError on the
    // rx-write line, the body never reaches the subsequent tx-write
    // — so out_data stays at its caller-initialised 0.0f.  We assert
    // that explicitly to pin the "raise aborts the callback"
    // sequencing, identical to Lua's processor gap-fill.
    return process_worker_with_script(
        dir, "python_engine::invoke_process_rx_slot_is_read_only",
        R"PY(
def on_process(rx, tx, msgs, api):
    rx.slot.value = 777.0   # raises AttributeError; aborts callback
    tx.slot.value = 3.25    # unreachable
    return True             # unreachable
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            float in_data  = 99.5f;
            float out_data = 0.0f;
            std::vector<IncomingMessage> msgs;
            auto result = engine.invoke_process(
                InvokeRx{&in_data, sizeof(in_data)},
                InvokeTx{&out_data, sizeof(out_data)}, msgs);

            // Four coupled assertions pinning the design contract:
            EXPECT_FLOAT_EQ(in_data, 99.5f)
                << "rx.slot write must NOT propagate to the underlying "
                   "rx buffer.";
            EXPECT_EQ(result, InvokeResult::Error)
                << "rx write in invoke_process must surface as "
                   "InvokeResult::Error, same loud contract as "
                   "invoke_consume.";
            EXPECT_EQ(engine.script_error_count(), 1u);
            // tx never written because the AttributeError aborted the
            // callback on the preceding line.
            EXPECT_FLOAT_EQ(out_data, 0.0f)
                << "the AttributeError on the rx write must abort "
                   "on_process — the tx write on the next line was "
                   "unreachable, so out_data stays at its initial 0.0f.";
        });
}

// ── Messages (chunk 5) ─────────────────────────────────────────────────────
//
// Two projection paths, canonical reference at
// src/scripting/python_engine.cpp and :1143:
//
//   - build_messages_list_        — producer + processor
//                                   event → dict with flattened details
//                                   data  → (sender_hex_str, bytes) TUPLE
//
//   - build_messages_list_bare_   — consumer
//                                   event → dict (same shape)
//                                   data  → bare `bytes` (no sender)
//
// Python-vs-Lua divergence on the producer data-message shape:
//   - Python:  (sender_hex_str, bytes)  — 2-tuple
//   - Lua:     {sender="<hex>", data="<bytes>"}  — table
// Both engines converge on the consumer bare-bytes shape.
//
// sender_hex is rendered via format_tools::bytes_to_hex (lowercase,
// no separator).  Two bytes 0xCA 0xFE project to "cafe".

int invoke_produce_receives_messages_event_with_details(const std::string &dir)
{
    // Strengthened vs V2 InvokeProduce_ReceivesMessages.  The V2 body
    // set `m1.details["identity"] = "abc123"` in C++ but Python never
    // verified the details map reached the script — the engine could
    // be silently dropping or re-shaping details and the test would
    // pass.  The Python contract per python_engine.cpp is that
    // details keys are PROMOTED to sibling fields on the event dict:
    //     msg["event"] = "consumer_joined"
    //     msg["identity"] = "abc123"   # NOT msg["details"]["identity"]
    //
    // This body pins the promoted (not nested) contract and covers a
    // multi-key details path on the second message.
    return produce_worker_with_script(
        dir, "python_engine::invoke_produce_receives_messages_event_with_details",
        R"PY(
def on_produce(tx, msgs, api):
    assert len(msgs) == 2, f"expected 2 messages, got {len(msgs)}"
    # Event shape: dict with 'event' key plus flattened detail fields.
    assert isinstance(msgs[0], dict), f"msgs[0] should be dict, got {type(msgs[0])}"
    assert msgs[0]["event"] == "consumer_joined", msgs[0]["event"]
    # NEW: verify details-map promotion on m1.
    assert msgs[0].get("identity") == "abc123", (
        f"msgs[0].identity expected 'abc123', got {msgs[0].get('identity')}")

    assert isinstance(msgs[1], dict), f"msgs[1] should be dict, got {type(msgs[1])}"
    assert msgs[1]["event"] == "channel_closing", msgs[1]["event"]
    # NEW: verify multi-key details promotion on m2.
    assert msgs[1].get("reason") == "voluntary", msgs[1].get("reason")
    assert msgs[1].get("code") == 0, msgs[1].get("code")
    return False
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            std::vector<IncomingMessage> msgs;

            IncomingMessage m1;
            m1.event = "consumer_joined";
            m1.details["identity"] = "abc123";
            msgs.push_back(std::move(m1));

            IncomingMessage m2;
            m2.event = "channel_closing";
            m2.details["reason"] = "voluntary";
            m2.details["code"]   = 0;
            msgs.push_back(std::move(m2));

            float buf = 0.0f;
            auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf)},
                                                 msgs);
            EXPECT_EQ(result, InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "a failing Python assert here indicates either #msgs "
                   "was wrong, the event fields didn't match, or "
                   "details-map promotion is broken";
        });
}

int invoke_produce_receives_messages_empty_list(const std::string &dir)
{
    // Strengthened vs V2 InvokeProduce_EmptyMessagesList.  V2 only
    // checked len(msgs) == 0.  This body also pins:
    //   - msgs is a list (not None, not some iterable proxy)
    //   - iteration over empty msgs produces no entries (range-
    //     iteration bug protection, same motivation as Lua's
    //     empty-vector test)
    return produce_worker_with_script(
        dir, "python_engine::invoke_produce_receives_messages_empty_list",
        R"PY(
def on_produce(tx, msgs, api):
    assert isinstance(msgs, list), f"msgs should be list, got {type(msgs)}"
    assert len(msgs) == 0, f"msgs should be empty, got len={len(msgs)}"
    # Iteration over empty list must produce no entries.
    any_seen = False
    for _m in msgs:
        any_seen = True
    assert any_seen is False, "empty msgs should yield no entries"
    return False
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            std::vector<IncomingMessage> msgs;  // empty
            float buf = 0.0f;
            auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf)},
                                                 msgs);
            EXPECT_EQ(result, InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

int invoke_produce_receives_messages_data_message(const std::string &dir)
{
    // NEW gap-fill — data-message shape for producer/processor
    // (build_messages_list_).  A data message has empty event and
    // populated data+sender fields; Python receives a TUPLE
    // (sender_hex_str, data_bytes).  V2 never tested this shape at
    // all, leaving the sender-hex formatting and bytes conversion
    // paths untested.
    //
    // Mirrors the Lua gap-fill but asserts the tuple shape instead
    // of the Lua table shape.
    return produce_worker_with_script(
        dir, "python_engine::invoke_produce_receives_messages_data_message",
        R"PY(
def on_produce(tx, msgs, api):
    assert len(msgs) == 1, "expected 1 message"
    m = msgs[0]
    # Python producer data-message shape: 2-tuple (sender_hex, bytes).
    assert isinstance(m, tuple), f"data msg should be tuple, got {type(m)}"
    assert len(m) == 2, f"data msg tuple should have 2 elements, got {len(m)}"
    sender, data = m
    assert isinstance(sender, str), f"sender should be str, got {type(sender)}"
    # Two bytes 0xCA 0xFE project to lowercase hex "cafe".
    assert sender == "cafe", f"sender hex expected 'cafe', got {sender!r}"
    assert isinstance(data, bytes), f"data should be bytes, got {type(data)}"
    assert data == b"pong", f"data expected b'pong', got {data!r}"
    return False
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            std::vector<IncomingMessage> msgs;
            IncomingMessage m;
            m.event = "";  // data message — empty event triggers bare/tuple projection
            m.sender = std::string("\xCA\xFE", 2);
            for (char c : std::string{"pong"})
                m.data.push_back(static_cast<std::byte>(c));
            msgs.push_back(std::move(m));

            float buf = 0.0f;
            auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf)},
                                                 msgs);
            EXPECT_EQ(result, InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "a failing Python assert here indicates the "
                   "(sender_hex, bytes) tuple projection differs from "
                   "the documented shape at python_engine.cpp";
        });
}

int invoke_consume_receives_messages_data_bare_format(const std::string &dir)
{
    // Strengthened vs V2 InvokeConsume_BareDataMessages.  V2 only
    // checked isinstance(msgs[0], bytes) and the event-message dict
    // on msgs[1].  This body additionally asserts:
    //   - exact data bytes (catches a wrong-memcpy-size regression)
    //   - result == Commit (V2 didn't check the return dispatch)
    //
    // Pins the producer-vs-consumer divergence explicitly: the
    // consumer projection DROPS the sender and exposes data as bare
    // bytes directly at msgs[i] (not a tuple).  Python contract at
    // python_engine.cpp.
    return consume_worker_with_script(
        dir, "python_engine::invoke_consume_receives_messages_data_bare_format",
        R"PY(
def on_consume(rx, msgs, api):
    assert len(msgs) == 2, f"expected 2 messages, got {len(msgs)}"
    # Consumer bare format: msgs[0] is raw bytes, NOT a tuple.  This
    # is the key divergence from the producer/processor path.
    assert isinstance(msgs[0], bytes), (
        f"consumer data msg should be bare bytes, got {type(msgs[0])}")
    assert msgs[0] == b"AB", f"data expected b'AB', got {msgs[0]!r}"
    # Event message: dict (same shape as producer path).
    assert isinstance(msgs[1], dict), (
        f"event msg should be dict, got {type(msgs[1])}")
    assert msgs[1]["event"] == "channel_closing"
    return True
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            std::vector<IncomingMessage> msgs;
            // Data message.
            IncomingMessage dm;
            dm.sender = "sender-id";  // consumer drops sender in projection
            dm.data   = {std::byte{0x41}, std::byte{0x42}};  // "AB"
            msgs.push_back(std::move(dm));
            // Event message.
            IncomingMessage em;
            em.event = "channel_closing";
            msgs.push_back(std::move(em));

            auto result = engine.invoke_consume(InvokeRx{nullptr, 0}, msgs);
            EXPECT_EQ(result, InvokeResult::Commit)
                << "on_consume returning True must map to Commit";
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "a failing Python assert here indicates the consumer "
                   "bare-bytes projection differs from the documented "
                   "shape at python_engine.cpp";
        });
}

// ── API closures (chunk 6) ─────────────────────────────────────────────────
//
// pybind11 bindings on ProducerAPI/ConsumerAPI/ProcessorAPI exposed
// as methods on the `api` parameter to callbacks.  Source-traced:
//
//   - version_info: pylabhub_producer.version_info() at module level
//     (not api.*); returns a JSON string.
//   - api.stop(): RoleAPIBase::stop() → core->request_stop()
//     (role_api_base.cpp)
//   - api.set_critical_error(): three side effects
//     (role_api_base.cpp → core->set_critical_error):
//     (a) critical_error_ = true → api.critical_error() returns True
//     (b) shutdown_requested_ = true
//     (c) stop_reason_ = CriticalError(3) → api.stop_reason()
//         returns "critical_error"
//   - api.stop_reason(): RoleAPIBase::stop_reason() → core->
//     stop_reason_string() → "normal"/"peer_dead"/"hub_dead"/
//     "critical_error" per StopReason enum
//     (role_host_core.hpp).
//
// Wrong-module-import: build_api removes the two inactive roles'
// pybind11 modules from sys.modules so a producer script can't
// accidentally import consumer or processor APIs.  Covered via
// eval() to trigger the importation directly.

int api_version_info_returns_json_string(const std::string &dir)
{
    // Strengthened vs V2 ApiVersionInfo.  V2 only asserted
    // isinstance(str) + len > 10 + contains '{'.  This body pins:
    //   - matching '}' (bracket balance — catches truncation)
    //   - "version" substring (pins the schema key, not just "some
    //     JSON")
    //   - result == Discard (return-value dispatch)
    return produce_worker_with_script(
        dir, "python_engine::api_version_info_returns_json_string",
        R"PY(
import pylabhub_producer

def on_produce(tx, msgs, api):
    info = pylabhub_producer.version_info()
    assert isinstance(info, str), f"version_info should return str, got {type(info)}"
    assert len(info) > 10, f"version_info too short: {info!r}"
    assert "{" in info, f"version_info should contain '{{': {info!r}"
    assert "}" in info, f"version_info should contain '}}' (truncation check): {info!r}"
    # Pins the presence of a canonical schema key ("script_api_major"
    # is part of the ABI handshake) — a reworded version-info output
    # that dropped this key would be caught.
    assert "script_api_major" in info, (
        f"version_info should contain 'script_api_major' key: {info!r}")
    return False
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            float buf = 0.0f;
            std::vector<IncomingMessage> msgs;
            auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf)},
                                                 msgs);
            EXPECT_EQ(result, InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

int wrong_role_module_import_raises_error(const std::string &dir)
{
    // Strengthened vs V2.  V2 only tested that importing
    // pylabhub_consumer from a producer engine fails.  This body
    // covers BOTH other-role modules (consumer AND processor) to
    // ensure build_api removes ALL inactive role modules from
    // sys.modules — a regression that left one role accessible
    // would slip through the V2 coverage.
    //
    // Each eval() returns InvokeStatus::ScriptError and increments
    // the engine's error counter (via execute_direct_ →
    // on_python_error_ → inc_script_error_count).
    return produce_worker_with_script(
        dir, "python_engine::wrong_role_module_import_raises_error",
        R"PY(
def on_produce(tx, msgs, api):
    return False
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            // Engine set up as producer.  Consumer + processor must fail.
            auto r1 = engine.eval("__import__('pylabhub_consumer')");
            EXPECT_EQ(r1.status,
                      pylabhub::scripting::InvokeStatus::ScriptError)
                << "importing pylabhub_consumer from producer must fail";

            auto r2 = engine.eval("__import__('pylabhub_processor')");
            EXPECT_EQ(r2.status,
                      pylabhub::scripting::InvokeStatus::ScriptError)
                << "importing pylabhub_processor from producer must fail";

            // Both failures should have bumped the counter.
            EXPECT_GE(engine.script_error_count(), 2u)
                << "each blocked import must increment script_error_count";
        });
}

int api_stop_sets_shutdown_requested(const std::string &dir)
{
    // Strengthened: also asserts result == Discard (V2 didn't check
    // the dispatch return value).
    return produce_worker_with_script(
        dir, "python_engine::api_stop_sets_shutdown_requested",
        R"PY(
def on_produce(tx, msgs, api):
    api.stop()
    return False
)PY",
        [](PythonEngine &engine, RoleHostCore &core) {
            EXPECT_FALSE(core.is_shutdown_requested());

            float buf = 0.0f;
            std::vector<IncomingMessage> msgs;
            auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf)},
                                                 msgs);
            EXPECT_EQ(result, InvokeResult::Discard);
            EXPECT_TRUE(core.is_shutdown_requested())
                << "api.stop() must set shutdown_requested";
            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

int api_critical_error_set_and_read_and_stop_reason(const std::string &dir)
{
    // Renamed from V2 ApiSetCriticalError.  V2 only verified 2 of
    // the 3 side-effects of api.set_critical_error().  This body
    // pins all three (role_api_base.cpp → core->
    // set_critical_error()):
    //   (a) critical_error_ = true    → api.critical_error() == True
    //   (b) shutdown_requested_ = true → core.is_shutdown_requested()
    //   (c) stop_reason_ = CriticalError → api.stop_reason() ==
    //       "critical_error"   ← V2 missed this third side-effect
    //
    // Also reads api.critical_error() BEFORE and AFTER the write to
    // confirm the read closure tracks state (V2 didn't exercise the
    // reader at all).
    return produce_worker_with_script(
        dir, "python_engine::api_critical_error_set_and_read_and_stop_reason",
        R"PY(
def on_produce(tx, msgs, api):
    # Pre-state: critical_error read returns False, stop_reason "normal".
    assert api.critical_error() is False, (
        f"pre-state: critical_error must be False, got {api.critical_error()!r}")
    assert api.stop_reason() == "normal", (
        f"pre-state: stop_reason must be 'normal', got {api.stop_reason()!r}")

    api.set_critical_error()

    # Post-state: all three side-effects must be observable.
    assert api.critical_error() is True, (
        f"post-state: critical_error must be True, got {api.critical_error()!r}")
    assert api.stop_reason() == "critical_error", (
        f"post-state: stop_reason must be 'critical_error', got {api.stop_reason()!r}")
    return False
)PY",
        [](PythonEngine &engine, RoleHostCore &core) {
            EXPECT_FALSE(core.is_critical_error());
            EXPECT_FALSE(core.is_shutdown_requested());

            float buf = 0.0f;
            std::vector<IncomingMessage> msgs;
            auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf)},
                                                 msgs);
            EXPECT_EQ(result, InvokeResult::Discard);
            EXPECT_TRUE(core.is_critical_error())
                << "api.set_critical_error() must set critical_error_";
            EXPECT_TRUE(core.is_shutdown_requested())
                << "api.set_critical_error() must also set "
                   "shutdown_requested_";
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "the in-script assertions must have all passed";
        });
}

int api_stop_reason_reflects_all_enum_values(const std::string &dir)
{
    // Strengthened vs V2 (which had two separate tests: StopReason_
    // Default covering "normal" and StopReason_PeerDead covering
    // PeerDead).  HubDead was NEVER tested in V2 — clear gap,
    // matches Lua's chunk 6 strengthening.
    //
    // Each iteration injects a different enum value from C++ and
    // verifies the Python closure returns the right string.  Covers:
    //   StopReason::Normal         (0) → "normal"
    //   StopReason::PeerDead       (1) → "peer_dead"
    //   StopReason::HubDead        (2) → "hub_dead"    ← NEW coverage
    //   StopReason::CriticalError  (3) → "critical_error"
    return produce_worker_with_script(
        dir, "python_engine::api_stop_reason_reflects_all_enum_values",
        R"PY(
_expected = None

def _set_expected(v):
    global _expected
    _expected = v

def on_produce(tx, msgs, api):
    got = api.stop_reason()
    assert got == _expected, (
        f"stop_reason mismatch: expected {_expected!r}, got {got!r}")
    return False
)PY",
        [](PythonEngine &engine, RoleHostCore &core) {
            // Execute once per enum value.  Set _expected via eval()
            // before each invoke; eval+invoke share the same
            // interpreter state so the global persists.
            struct Case
            {
                RoleHostCore::StopReason reason;
                const char *expected;
            };
            const Case cases[] = {
                {RoleHostCore::StopReason::Normal,        "normal"},
                {RoleHostCore::StopReason::PeerDead,      "peer_dead"},
                {RoleHostCore::StopReason::HubDead,       "hub_dead"},
                {RoleHostCore::StopReason::CriticalError, "critical_error"},
            };
            for (const auto &c : cases)
            {
                core.set_stop_reason(c.reason);
                // Set _expected via eval in the script module.
                engine.eval(std::string("_set_expected('") + c.expected
                            + "')");

                float buf = 0.0f;
                std::vector<IncomingMessage> msgs;
                auto result = engine.invoke_produce(
                    InvokeTx{&buf, sizeof(buf)}, msgs);
                EXPECT_EQ(result, InvokeResult::Discard);
                EXPECT_EQ(engine.script_error_count(), 0u)
                    << "stop_reason mismatch for enum value → "
                    << c.expected;
            }
        });
}

// ── Metrics + error accumulation (chunk 7) ────────────────────────────────
//
// Python exposes two metric surfaces via pybind11:
//   - Individual accessors on the api object
//     (out_slots_written / in_slots_received / out_drop_count /
//     loop_overrun_count / script_error_count / last_cycle_work_us)
//   - api.metrics() → py::dict assembled from snapshot_metrics_json
//     (role_api_base.cpp) with top-level groups: queue /
//     in_queue+out_queue / loop / role / inbox / custom.
//
// The `loop` group's canonical field list is defined in
// role_host_core.hpp via PYLABHUB_LOOP_METRICS_FIELDS and has
// FIVE fields: iteration_count, loop_overrun_count,
// last_cycle_work_us, configured_period_us, acquire_retry_count.
// V2's Metrics_AllLoopFields_Present only checked 4 of 5 — the
// all_loop_fields_anchored_values body closes that gap.

int metrics_individual_accessors_read_core_counters_live(const std::string &dir)
{
    // Strengthened vs V2 MetricsClosures_ReadFromRoleHostCounters.
    // V2 set counters once before build_api and read once from the
    // callback.  This body additionally:
    //   - reads BEFORE the set to confirm initial zeros
    //   - mutates via core.test_set_* and asserts propagation
    //   - covers loop_overrun_count (V2 missed it entirely on the
    //     individual-accessor path)
    return produce_worker_with_script(
        dir, "python_engine::metrics_individual_accessors_read_core_counters_live",
        R"PY(
_phase = 0

def _next_phase():
    global _phase
    _phase += 1

def on_produce(tx, msgs, api):
    if _phase == 1:
        # Pre-set state: all counters zero.
        assert api.out_slots_written() == 0, api.out_slots_written()
        assert api.out_drop_count()    == 0, api.out_drop_count()
        assert api.loop_overrun_count() == 0, api.loop_overrun_count()
    elif _phase == 2:
        # Post-set state: values seeded from C++.
        assert api.out_slots_written() == 42, api.out_slots_written()
        assert api.out_drop_count()    == 7,  api.out_drop_count()
        assert api.loop_overrun_count() == 3, api.loop_overrun_count()
    return False
)PY",
        [](PythonEngine &engine, RoleHostCore &core) {
            float buf = 0.0f;
            std::vector<IncomingMessage> msgs;

            // Phase 1: all zero.
            engine.eval("_next_phase()");
            auto r1 = engine.invoke_produce(InvokeTx{&buf, sizeof(buf)},
                                             msgs);
            EXPECT_EQ(r1, InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 0u);

            // Mutate core counters.
            core.test_set_out_slots_written(42);
            core.test_set_out_drop_count(7);
            core.inc_loop_overrun();
            core.inc_loop_overrun();
            core.inc_loop_overrun();

            // Phase 2: read-back from callback.
            engine.eval("_next_phase()");
            auto r2 = engine.invoke_produce(InvokeTx{&buf, sizeof(buf)},
                                             msgs);
            EXPECT_EQ(r2, InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

int metrics_in_slots_received_works_consumer(const std::string &dir)
{
    // Strengthened: before/after transition (V2 set once, read once).
    return consume_worker_with_script(
        dir, "python_engine::metrics_in_slots_received_works_consumer",
        R"PY(
_phase = 0

def _next_phase():
    global _phase
    _phase += 1

def on_consume(rx, msgs, api):
    if _phase == 1:
        assert api.in_slots_received() == 0, api.in_slots_received()
    elif _phase == 2:
        assert api.in_slots_received() == 15, api.in_slots_received()
    return True
)PY",
        [](PythonEngine &engine, RoleHostCore &core) {
            std::vector<IncomingMessage> msgs;

            engine.eval("_next_phase()");
            auto r1 = engine.invoke_consume(InvokeRx{nullptr, 0}, msgs);
            EXPECT_EQ(r1, InvokeResult::Commit);
            EXPECT_EQ(engine.script_error_count(), 0u);

            core.test_set_in_slots_received(15);

            engine.eval("_next_phase()");
            auto r2 = engine.invoke_consume(InvokeRx{nullptr, 0}, msgs);
            EXPECT_EQ(r2, InvokeResult::Commit);
            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

int multiple_errors_count_accumulates(const std::string &dir)
{
    // Strengthened vs V2 MultipleErrors_CountAccumulates.  V2 only
    // asserted the engine-side accessor `engine.script_error_count()
    // == 5`.  This body additionally cross-links to api.metrics()
    // ["role"]["script_error_count"] to pin that both surfaces
    // observe the same counter — catches a regression where they
    // diverge (e.g. one path bumps a different counter).
    return produce_worker_with_script(
        dir, "python_engine::multiple_errors_count_accumulates",
        R"PY(
_in_check_mode = False

def on_produce(tx, msgs, api):
    global _in_check_mode
    if _in_check_mode:
        m = api.metrics()
        sec = m["role"]["script_error_count"]
        assert sec == 5, f"api.metrics role.script_error_count == {sec}, expected 5"
        return False
    raise RuntimeError("oops")

def _flip_to_check():
    global _in_check_mode
    _in_check_mode = True
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            float buf = 0.0f;
            std::vector<IncomingMessage> msgs;

            // 5 raising invocations → script_error_count becomes 5.
            for (int i = 0; i < 5; ++i)
                engine.invoke_produce(InvokeTx{&buf, sizeof(buf)}, msgs);

            EXPECT_EQ(engine.script_error_count(), 5u)
                << "engine accessor must reflect 5 accumulated raises";

            // Flip the script into check mode and run one more invoke
            // that reads the same counter via api.metrics().  That
            // invocation is itself a successful (non-raising) Discard,
            // so the counter stays at 5.
            engine.eval("_flip_to_check()");
            auto r = engine.invoke_produce(InvokeTx{&buf, sizeof(buf)},
                                            msgs);
            EXPECT_EQ(r, InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 5u)
                << "successful check invocation must NOT bump counter";
        });
}

int stop_on_script_error_sets_shutdown_on_error(const std::string &dir)
{
    // Strengthened vs V2.  V2 asserted result == Error and
    // shutdown_requested == true.  This body additionally pins:
    //   - critical_error stays FALSE (stop_on_script_error is a
    //     controlled shutdown, not a critical-error path)
    //   - stop_reason stays "normal" (distinct from the enum value
    //     StopReason::CriticalError which requires api.
    //     set_critical_error or an explicit set_stop_reason call)
    //   - script_error_count increments to 1
    //
    // Custom setup inline (doesn't use setup_role_engine) because the
    // helper's make_api sets stop_on_script_error(false) by default;
    // we need true for this test.  Mirrors the Lua equivalent at
    // lua_engine_workers.cpp.
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir, R"PY(
def on_produce(tx, msgs, api):
    raise RuntimeError("intentional error")
)PY");

            RoleHostCore core;
            PythonEngine engine;
            engine.set_python_venv("");
            ASSERT_TRUE(engine.initialize("test", &core));
            ASSERT_TRUE(engine.load_script(script_dir / "script" / "python",
                                            "__init__.py", "on_produce"));

            auto spec = simple_schema();
            ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame",
                                                   "aligned"));

            auto api = make_api(core);
            api->set_stop_on_script_error(true);  // ← key difference
            ASSERT_TRUE(engine.build_api(*api));

            EXPECT_FALSE(core.is_shutdown_requested());
            EXPECT_FALSE(core.is_critical_error());
            EXPECT_EQ(core.stop_reason_string(), "normal");

            float buf = 0.0f;
            std::vector<IncomingMessage> msgs;
            auto r = engine.invoke_produce(InvokeTx{&buf, sizeof(buf)}, msgs);

            EXPECT_EQ(r, InvokeResult::Error);
            EXPECT_EQ(engine.script_error_count(), 1u);
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
        "python_engine::stop_on_script_error_sets_shutdown_on_error",
        Logger::GetLifecycleModule());
}

int metrics_all_loop_fields_anchored_values(const std::string &dir)
{
    // Strengthened vs V2 Metrics_AllLoopFields_Present.  V2 only
    // covered 4 of the 5 canonical loop fields defined in
    // role_host_core.hpp PYLABHUB_LOOP_METRICS_FIELDS:
    //   iteration_count, loop_overrun_count, last_cycle_work_us,
    //   configured_period_us, acquire_retry_count  ← V2 missed this
    //
    // A regression adding/removing/renaming a loop field must be
    // loud.  Type-checks all 5 as int + anchors each to a specific
    // non-default value from C++.
    return produce_worker_with_script(
        dir, "python_engine::metrics_all_loop_fields_anchored_values",
        R"PY(
def on_produce(tx, msgs, api):
    m = api.metrics()
    assert "loop" in m, f"'loop' group missing from metrics: {list(m.keys())}"
    loop = m["loop"]

    # All 5 canonical fields per PYLABHUB_LOOP_METRICS_FIELDS must be
    # present, typed as int, and reflect the values seeded from C++.
    expected = {
        "iteration_count":      5,
        "loop_overrun_count":   2,
        "last_cycle_work_us":   999,
        "configured_period_us": 10000,
        "acquire_retry_count":  17,   # NEW vs V2
    }
    for key, val in expected.items():
        assert key in loop, f"'{key}' missing from loop group: {list(loop.keys())}"
        assert isinstance(loop[key], int), (
            f"{key} must be int, got {type(loop[key]).__name__}")
        assert loop[key] == val, (
            f"{key} expected {val}, got {loop[key]}")

    # No extras: the field inventory is closed.  If a new field is
    # added to PYLABHUB_LOOP_METRICS_FIELDS this assertion catches it
    # so the test is updated in the same commit.
    unexpected = set(loop.keys()) - set(expected.keys())
    assert not unexpected, (
        f"unexpected loop fields (update test): {sorted(unexpected)}")
    return False
)PY",
        [](PythonEngine &engine, RoleHostCore &core) {
            // Seed all 5 loop fields from C++ before the invoke.
            for (int i = 0; i < 5; ++i) core.inc_iteration_count();
            core.inc_loop_overrun();
            core.inc_loop_overrun();
            core.set_last_cycle_work_us(999);
            core.set_configured_period(10000);
            for (int i = 0; i < 17; ++i) core.inc_acquire_retry();

            float buf = 0.0f;
            std::vector<IncomingMessage> msgs;
            auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf)},
                                                 msgs);
            EXPECT_EQ(result, InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

int metrics_hierarchical_table_producer_full_shape(const std::string &dir)
{
    // NEW gap-fill — pins the full top-level group inventory returned
    // by api.metrics() on a producer with no queue wiring (L2 scope).
    // Without a queue, the inventory is: {loop, role}.  Adding
    // infrastructure at L3/L4 tests the other groups.  This test
    // ensures the L2 shape stays stable; a regression adding a
    // phantom group (e.g. half-initialized queue exposing partial
    // metrics) surfaces here.
    return produce_worker_with_script(
        dir, "python_engine::metrics_hierarchical_table_producer_full_shape",
        R"PY(
def on_produce(tx, msgs, api):
    m = api.metrics()
    assert isinstance(m, dict), f"metrics must be dict, got {type(m)}"

    # L2 producer with no queue wiring: expect exactly {loop, role}.
    # - 'queue' / 'in_queue' / 'out_queue' require pImpl->tx_queue
    #   or rx_queue non-null (role_api_base.cpp)
    # - 'inbox' requires pImpl->inbox_queue (L3)
    # - 'custom' only appears if non-empty
    expected_keys = {"loop", "role"}
    got_keys = set(m.keys())
    assert got_keys == expected_keys, (
        f"L2 producer metrics groups mismatch — expected {sorted(expected_keys)}, "
        f"got {sorted(got_keys)}")

    # role group: 4 fields (out_slots_written, in_slots_received,
    # out_drop_count, script_error_count) per role_api_base.cpp.
    role_expected = {"out_slots_written", "in_slots_received",
                     "out_drop_count", "script_error_count"}
    role_got = set(m["role"].keys())
    assert role_got == role_expected, (
        f"role fields mismatch — expected {sorted(role_expected)}, "
        f"got {sorted(role_got)}")
    return False
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            float buf = 0.0f;
            std::vector<IncomingMessage> msgs;
            auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf)},
                                                 msgs);
            EXPECT_EQ(result, InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

int metrics_role_script_error_count_reflects_raised_error(const std::string &dir)
{
    // NEW gap-fill — pins that api.metrics()["role"]["script_error_count"]
    // and the engine's internal counter observe the same source.
    // A regression where the metrics path reads a different counter
    // (e.g. a stale snapshot) would slip through the individual-
    // accessor tests above but fail here.
    //
    // Sequence: seed two raises then check api.metrics readback
    // == 2 from within a third (non-raising) invocation.
    return produce_worker_with_script(
        dir, "python_engine::metrics_role_script_error_count_reflects_raised_error",
        R"PY(
_phase = 0

def _next_phase():
    global _phase
    _phase += 1

def on_produce(tx, msgs, api):
    if _phase in (1, 2):
        raise RuntimeError(f"seed phase {_phase}")
    # _phase == 3: non-raising readback.
    m = api.metrics()
    sec = m["role"]["script_error_count"]
    assert sec == 2, f"script_error_count expected 2, got {sec}"
    return False
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            float buf = 0.0f;
            std::vector<IncomingMessage> msgs;

            // Phase 1 + 2: raise twice.
            engine.eval("_next_phase()");
            engine.invoke_produce(InvokeTx{&buf, sizeof(buf)}, msgs);
            engine.eval("_next_phase()");
            engine.invoke_produce(InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(engine.script_error_count(), 2u);

            // Phase 3: read back via api.metrics().
            engine.eval("_next_phase()");
            auto r = engine.invoke_produce(InvokeTx{&buf, sizeof(buf)},
                                            msgs);
            EXPECT_EQ(r, InvokeResult::Discard)
                << "the readback invocation must succeed";
            EXPECT_EQ(engine.script_error_count(), 2u)
                << "readback invocation did not raise — counter stays at 2";
        });
}

// ── Load script + script errors (chunk 8) ─────────────────────────────────
//
// load_script + register_slot_type + callback-error paths.  Covers:
//   - nonexistent script path
//   - script present but required_callback absent
//   - register_slot_type with bad field type (post canonical-name
//     tightening, must use a valid name or the test short-circuits
//     on the name check — this was a V2 bug)
//   - Python syntax error in script
//   - has_callback presence/absence + non-canonical names
//   - on_init / on_stop / on_inbox raising Python exceptions

int load_script_missing_file(const std::string &dir)
{
    // Strengthened vs V2.  V2 only checked return value.  The catch
    // path at python_engine.cpp logs ERROR "Failed to load
    // script from '<dir>': <exc>".  Pin that ERROR fragment at
    // parent level.
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            RoleHostCore core;
            PythonEngine engine;
            engine.set_python_venv("");
            ASSERT_TRUE(engine.initialize("test", &core));
            EXPECT_FALSE(engine.load_script(
                script_dir / "nonexistent" / "path",
                "__init__.py", "on_produce"));
            engine.finalize();
        },
        "python_engine::load_script_missing_file",
        Logger::GetLifecycleModule());
}

int load_script_missing_required_callback(const std::string &dir)
{
    // Strengthened vs V2.  V2 only checked return value.  Python
    // logs ERROR "Script has no 'on_produce' function"
    // (python_engine.cpp) when required_callback is absent.
    // Pin the exact fragment at parent level.
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir, R"PY(
def on_init(api):
    pass
# on_produce intentionally absent
)PY");
            RoleHostCore core;
            PythonEngine engine;
            engine.set_python_venv("");
            ASSERT_TRUE(engine.initialize("test", &core));
            EXPECT_FALSE(engine.load_script(
                script_dir / "script" / "python",
                "__init__.py", "on_produce"));
            engine.finalize();
        },
        "python_engine::load_script_missing_required_callback",
        Logger::GetLifecycleModule());
}

int register_slot_type_bad_field_type(const std::string &dir)
{
    // IMPORTANT: FIXED vs V2.  V2 used type_name="BadFrame" (non-
    // canonical) with an unsupported field type "complex128".  After
    // the canonical-name tightening (python_engine.cpp),
    // register_slot_type now rejects "BadFrame" UP FRONT before
    // reaching the field-type dispatcher.  V2 therefore passed via
    // the wrong branch — it tested canonical-name rejection, not
    // bad-field-type.
    //
    // This body uses "OutSlotFrame" (canonical) with a bad field
    // type so we actually exercise the build_ctypes_type_ exception
    // path at python_engine.cpp.  The canonical-name
    // rejection has its own test (register_slot_type_has_schema_
    // false_returns_false in chunk 1 covers a related invariant).
    //
    // Log fragment: "register_slot_type('OutSlotFrame') failed:
    // <exc>" from python_engine.cpp.
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir,
                         "def on_produce(tx, msgs, api):\n"
                         "    return True\n");
            RoleHostCore core;
            PythonEngine engine;
            engine.set_python_venv("");
            ASSERT_TRUE(engine.initialize("test", &core));
            ASSERT_TRUE(engine.load_script(
                script_dir / "script" / "python",
                "__init__.py", "on_produce"));

            hub::SchemaSpec bad_spec;
            bad_spec.has_schema = true;
            hub::FieldDef f;
            f.name     = "x";
            f.type_str = "complex128";  // unsupported type
            f.count    = 1;
            f.length   = 0;
            bad_spec.fields.push_back(f);

            EXPECT_FALSE(engine.register_slot_type(bad_spec,
                                                    "OutSlotFrame",
                                                    "aligned"))
                << "bad field type must fail via the build_ctypes_type_ "
                   "exception path, not the canonical-name check";
            engine.finalize();
        },
        "python_engine::register_slot_type_bad_field_type",
        Logger::GetLifecycleModule());
}

int load_script_syntax_error(const std::string &dir)
{
    // Strengthened vs V2.  V2 only checked return value.  A Python
    // SyntaxError during import surfaces via the catch at
    // python_engine.cpp as "Failed to load script from
    // '<dir>': <exc>" where <exc> includes "SyntaxError".  Pin the
    // "SyntaxError" fragment.
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            // Missing colon after signature → SyntaxError.
            write_script(script_dir,
                         "def on_produce(tx, msgs, api)\n"
                         "    return True\n");
            RoleHostCore core;
            PythonEngine engine;
            engine.set_python_venv("");
            ASSERT_TRUE(engine.initialize("test", &core));
            EXPECT_FALSE(engine.load_script(
                script_dir / "script" / "python",
                "__init__.py", "on_produce"));
            engine.finalize();
        },
        "python_engine::load_script_syntax_error",
        Logger::GetLifecycleModule());
}

int has_callback(const std::string &dir)
{
    // Strengthened vs V2.  V2 tested 4 names (3 defined + 1 absent
    // canonical).  This body additionally exercises NON-canonical
    // names to pin the documented behaviour: Python's has_callback
    // uses py::getattr which will return true for ANY attribute
    // present on the module (including user-defined helpers).  Per
    // the 2026-04-20 design review (Finding #14, resolved no-action):
    // Python's current contract is "returns true for any resolvable
    // symbol" — a canonical-names-only restriction is native-only.
    //
    // This test pins that contract so a regression restricting
    // Python to canonical-only would surface.
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir, R"PY(
def on_produce(tx, msgs, api):
    return True

def on_init(api):
    pass

def user_helper():
    return 42
# on_stop intentionally absent
)PY");
            RoleHostCore core;
            PythonEngine engine;
            engine.set_python_venv("");
            ASSERT_TRUE(engine.initialize("test", &core));
            ASSERT_TRUE(engine.load_script(
                script_dir / "script" / "python",
                "__init__.py", "on_produce"));

            // Canonical callbacks — presence reflects what the script
            // actually defines.
            EXPECT_TRUE(engine.has_callback("on_produce"));
            EXPECT_TRUE(engine.has_callback("on_init"));
            EXPECT_FALSE(engine.has_callback("on_stop"));
            EXPECT_FALSE(engine.has_callback("on_consume"));

            // NEW: non-canonical user function — Python returns true.
            EXPECT_TRUE(engine.has_callback("user_helper"))
                << "Python has_callback probes arbitrary attributes "
                   "(see TESTING_TODO 'has_callback for non-canonical "
                   "names' — resolved no-action 2026-04-20).  A "
                   "regression restricting to canonical-only would "
                   "break here.";

            // NEW: non-existent non-canonical name — returns false.
            EXPECT_FALSE(engine.has_callback("nonexistent_fn"));

            engine.finalize();
        },
        "python_engine::has_callback",
        Logger::GetLifecycleModule());
}

int invoke_on_init_script_error(const std::string &dir)
{
    // Strengthened vs V2 (EXPECT_GE → EXPECT_EQ == 1u, + ERROR text).
    return produce_worker_with_script(
        dir, "python_engine::invoke_on_init_script_error",
        R"PY(
def on_produce(tx, msgs, api):
    return True

def on_init(api):
    raise RuntimeError("init exploded")
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            EXPECT_EQ(engine.script_error_count(), 0u);
            engine.invoke_on_init();
            EXPECT_EQ(engine.script_error_count(), 1u)
                << "on_init raising exactly once must set count to 1";
        });
}

int invoke_on_stop_script_error(const std::string &dir)
{
    // Strengthened vs V2 (EXPECT_GE → EXPECT_EQ == 1u, + ERROR text).
    return produce_worker_with_script(
        dir, "python_engine::invoke_on_stop_script_error",
        R"PY(
def on_produce(tx, msgs, api):
    return True

def on_stop(api):
    raise RuntimeError("stop exploded")
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            EXPECT_EQ(engine.script_error_count(), 0u);
            engine.invoke_on_stop();
            EXPECT_EQ(engine.script_error_count(), 1u)
                << "on_stop raising exactly once must set count to 1";
        });
}

int invoke_on_inbox_script_error(const std::string &dir)
{
    // Strengthened vs V2 (EXPECT_GE → EXPECT_EQ == 1u, + ERROR text).
    // ALSO strengthened vs the previous P3 body (review finding F9,
    // 2026-04-20): the previous body only counted the exception; it
    // never verified the msg payload reached the script BEFORE the
    // raise.  A regression in from_buffer_copy dropping the typed
    // data would pass silently.  Two belt-and-suspenders:
    //
    //   (1) api.report_metric("inbox_reached", 1) — proves the
    //       callback actually ran (pins dispatch).  Read back via
    //       core.custom_metrics_snapshot() after the invoke.
    //
    //   (2) Three in-script asserts on msg.data / msg.sender_uid /
    //       msg.seq — pins the payload projection:
    //         - data must not be None (catches from_buffer_copy
    //           skipped, or inbox_type_ro_ unregistered)
    //         - sender_uid / seq must match the C++ side (catches
    //           raw copy vs string-convert regressions)
    //       These asserts run BEFORE the raise, so they must pass
    //       for the body to reach the exception.  If any fails, the
    //       AssertionError (not RuntimeError) would be counted —
    //       caught by the script_error_count still == 1 but the
    //       "inbox exploded" ERROR substring pinning at the parent
    //       level would miss.
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir, R"PY(
def on_produce(tx, msgs, api):
    return False

def on_inbox(msg, api):
    # Payload projection sanity:
    #   - msg.data not None   → from_buffer_copy produced a typed struct
    #   - msg.data.value ==   → from_buffer_copy bound to the correct bytes
    #                           (catches a regression where the buffer
    #                           is typed but wrong / stale memory)
    #   - sender_uid / seq    → non-typed fields still propagate
    assert msg.data is not None, "msg.data must be a typed ctypes object"
    assert abs(msg.data.value - 3.25) < 1e-6, (
        f"data.value expected 3.25 (sent from C++), got {msg.data.value}")
    assert msg.sender_uid == "SENDER-00000001", (
        f"sender_uid expected 'SENDER-00000001', got {msg.sender_uid!r}")
    assert msg.seq == 42, f"seq expected 42, got {msg.seq}"

    # Side-effect proving the callback actually ran (pins dispatch).
    api.report_metric("inbox_reached", 1)

    # Now raise — this is what the test is really about.
    raise RuntimeError("inbox exploded")
)PY");
            RoleHostCore core;
            PythonEngine engine;
            engine.set_python_venv("");
            ASSERT_TRUE(engine.initialize("test", &core));
            ASSERT_TRUE(engine.load_script(
                script_dir / "script" / "python",
                "__init__.py", "on_produce"));

            auto spec = simple_schema();
            ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame",
                                                   "aligned"));
            ASSERT_TRUE(engine.register_slot_type(spec, "InboxFrame",
                                                   "aligned"));

            auto api = make_api(core);
            ASSERT_TRUE(engine.build_api(*api));

            EXPECT_EQ(engine.script_error_count(), 0u);
            // Distinctive non-default value so the Python in-script
            // assert on msg.data.value has real discriminating power.
            // If from_buffer_copy bound to the wrong bytes (e.g. a
            // zero page), data.value would be 0.0 and the assert
            // would fire.
            float inbox_data = 3.25f;
            engine.invoke_on_inbox({&inbox_data, sizeof(inbox_data),
                                     "SENDER-00000001", 42});
            EXPECT_EQ(engine.script_error_count(), 1u)
                << "on_inbox raising exactly once must set count to 1";

            // Side-effect verification: report_metric must have fired
            // BEFORE the raise, so 'inbox_reached' is in custom_metrics.
            auto cm = core.custom_metrics_snapshot();
            EXPECT_EQ(cm.count("inbox_reached"), 1u)
                << "on_inbox must have reached report_metric before the "
                   "raise — a non-1 count means either the callback never "
                   "ran (dispatch regression) or one of the in-script "
                   "msg.* assertions failed (payload projection bug)";

            engine.finalize();
        },
        "python_engine::invoke_on_inbox_script_error",
        Logger::GetLifecycleModule());
}

// ── State + slot + inbox (chunk 9) ─────────────────────────────────────────

int state_persists_across_calls(const std::string &dir)
{
    // Strengthened over V2 — and strengthened Python-specifically
    // rather than mirroring Lua's state_persists test.  Lua has
    // per-thread lua_State; Python has a single interpreter + GIL,
    // so the risk profile is different.  For Python the two
    // regressions that would actually break persistence are:
    //
    //   (a) Module re-execution / re-import between invocations
    //       (would reset ALL module-level globals — immutables and
    //        mutables alike).
    //   (b) __dict__ snapshotting / copying at build_api time
    //       (an int counter rebinding would be invisible — a single
    //        new assignment reassigns the global anyway — but a
    //        mutable container growing in place would be visible only
    //        if the SAME dict object persists).
    //
    // So the body pins BOTH:
    //   - an int counter (rebind-semantics, catches re-import)
    //   - a list appended to in-place (identity-semantics, catches
    //     any deep-copy of module globals that would leave the
    //     script with a DIFFERENT list object per call)
    //
    // 4 invocations (not V2's 3) also let us verify first/last
    // entries in the list are both preserved — a "keeps last 3"
    // bounded-ring regression would only fail on call 4.
    return produce_worker_with_script(
        dir, "python_engine::state_persists_across_calls",
        R"PY(
call_count = 0
call_log   = []

def on_produce(tx, msgs, api):
    global call_count
    call_count += 1
    # Mutable container: identity must persist across calls — a
    # regression that deep-copies or re-creates the module dict
    # per invocation would leave call_log reset on call 2.
    # If a regression replaced the module __dict__ (or deep-copied it)
    # between invocations, call_log would point to a fresh list each
    # time and len(call_log) would stay at 1.
    call_log.append(call_count)
    assert len(call_log) == call_count, (
        f"call_log length ({len(call_log)}) != call_count ({call_count}) — "
        f"module-level list identity was not preserved across invocations")
    if tx.slot is not None:
        tx.slot.value = float(call_count)
    return True
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            std::vector<IncomingMessage> msgs;
            for (size_t i = 1; i <= 4; ++i)
            {
                float buf = 0.0f;
                auto  r   = engine.invoke_produce(
                    InvokeTx{&buf, sizeof(buf)}, msgs);
                EXPECT_EQ(r, InvokeResult::Commit);
                EXPECT_FLOAT_EQ(buf, static_cast<float>(i))
                    << "call_count must be " << i << " on iteration " << i
                    << " — Python module-level globals must persist "
                       "across invocations on the owner thread";
            }
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "any script-side assert failure (mutable-list identity "
                   "or length check) would increment this counter";
        });
}

int invoke_produce_slot_only_no_flexzone_on_invoke(const std::string &dir)
{
    // Strengthened over V2 — Python-specific + flex-combo-aware.
    //
    // Post-L3.ζ contract: InvokeTx carries only a slot pointer/size;
    // flexzone is reached via api.flexzone(side), never via a tx.fz
    // field.  V2 registered only OutSlotFrame — a WEAKER test because
    // tx having no fz field is trivially true when no flex exists.
    // This body deliberately registers BOTH OutSlotFrame AND
    // OutFlexFrame (with two distinct complex multi-field schemas) to
    // prove the Python-specific contract even when a flex IS
    // registered.  That is the regression the V2 body couldn't catch.
    //
    // What is pinned:
    //
    //   (1) Even with OutFlexFrame registered, PyTxChannel (defined in
    //       python_helpers.hpp:302) exposes ONLY `slot`.
    //       `hasattr(tx, "fz")` MUST be False.  A regression adding
    //       `.def_readwrite("fz", ...)` would flip to True.
    //
    //   (2) tx.slot is a ctypes view onto the caller's buffer
    //       (from_buffer, NOT from_buffer_copy).  Writes must
    //       propagate back — this is the opposite direction from the
    //       inbox path (from_buffer_copy, see invoke_on_inbox_typed_data).
    //
    //   (3) api.flexzone("out") returns None when no queue is wired.
    //       The Python engine's build_api path caches a flex typed
    //       view ONLY when api.flexzone(side) yields a non-null
    //       pointer with size > 0 (python_engine.cpp:461 guard).
    //       In pure L2 (no hub::QueueWriter wired into RoleAPIBase),
    //       the cache_fz helper returns nullopt → set_tx_flexzone(
    //       nullopt) → api.flexzone() yields None.  Pins that guard.
    //
    //   (4) api.update_flexzone_checksum() returns False when no
    //       tx_queue — pins the checksum-api guard on the no-queue
    //       path (role_api_base.cpp: returns false if !pImpl->tx_queue).
    //       Combined with the complex OutFlexFrame registration,
    //       this proves the guard is queue-driven, not
    //       registration-driven.
    //
    // Transport note: engine is transport-agnostic.  SHM-vs-ZMQ
    // combinations are exercised at L3 (test_layer3_datahub/) where
    // real hub::ShmQueue / hub::ZmqQueue writers are wired.  The L2
    // engine test here pins the engine's interface contract
    // independent of the queue backing.
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir, R"PY(
def on_produce(tx, msgs, api):
    assert tx.slot is not None, "expected slot"
    # (1) PyTxChannel must NOT expose `fz`, even when OutFlexFrame IS
    # registered — flexzone access is closure-only via api.flexzone().
    assert not hasattr(tx, "fz"), (
        "PyTxChannel must NOT expose fz (flexzone is accessed via "
        f"api.flexzone() closure) — tx attributes: {dir(tx)}")

    # (2) Field-level writes through the adversarial padding_schema
    # layout (ts @ 0, flag @ 8, count @ 12 after a 3-byte interior
    # pad).  Any ctypes-Structure padding regression would place
    # `count` at offset 9 and the C++ read-back at offset 12 would
    # see zero noise.
    tx.slot.ts    = 42.5
    tx.slot.flag  = 7
    tx.slot.count = -3

    # (3) api.flexzone() must return None when no queue is wired at
    # the RoleAPIBase level (pure L2 — no tx_queue).  Pins the
    # build_api cache_fz guard (python_engine.cpp:461): if
    # api.flexzone(side) returns null OR size==0, the cached view is
    # nullopt → Python sees None.  Producer's api.flexzone() defaults
    # to the Tx side when called with no argument.
    fz = api.flexzone()
    assert fz is None, f"api.flexzone() expected None in L2, got {fz!r}"
    # Same result via explicit Tx-side integer (producer_api.cpp:253
    # exposes ChannelSide::Tx as a static int on ProducerAPI).
    fz_tx = api.flexzone(api.Tx)
    assert fz_tx is None, f"api.flexzone(Tx) expected None, got {fz_tx!r}"

    # (4) update_flexzone_checksum must be False when no tx_queue —
    # role_api_base.cpp: returns false if !pImpl->tx_queue.  Combined
    # with the complex OutFlexFrame registration (non-empty flex
    # spec), this proves the guard is queue-driven, not
    # registration-driven.
    ok = api.update_flexzone_checksum()
    assert ok is False, (
        f"update_flexzone_checksum expected False without tx_queue, got {ok!r}")

    return True
)PY");

            RoleHostCore core;
            PythonEngine engine;
            engine.set_python_venv("");
            ASSERT_TRUE(engine.initialize("test", &core));
            ASSERT_TRUE(engine.load_script(
                script_dir / "script" / "python",
                "__init__.py", "on_produce"));

            // Adversarial-padding slot schema (padding_schema):
            //   ts    float64   @  0 (8)         ← aligned, no pre-pad
            //   flag  uint8     @  8 (1)
            //   pad             @  9 (3)         ← align int32 to 4
            //   count int32     @ 12 (4)         → ends @ 16
            //   struct align = 8, 16 is multiple → total 16 bytes
            //
            // Three distinct padding transitions (1→4 via 3-byte pad,
            // 2-field alignment stride, natural struct tail) — all of
            // which Python ctypes must honour for the field offsets
            // below to round-trip.  A regression in the ctypes
            // Structure builder dropping the interior pad would place
            // `count` at offset 9 instead of 12, and the C++-side
            // read-back would see count=0 (or whatever noise lives in
            // the padding bytes).
            auto slot_spec = padding_schema();
            ASSERT_TRUE(engine.register_slot_type(slot_spec, "OutSlotFrame",
                                                   "aligned"));

            // Adversarial-padding flex schema (complex_mixed_schema):
            //   timestamp float64    @  0 (8)
            //   data      float32[3] @  8 (12)     ← align 4, ends @ 20
            //   status    uint16     @ 20 (2)      ← align 2
            //   raw       bytes[5]   @ 22 (5)      ← align 1, ends @ 27
            //   label     string[16] @ 27 (16)     ← align 1, ends @ 43
            //   pad                  @ 43 (5)      ← align int64 to 8
            //   seq       int64      @ 48 (8)      → ends @ 56
            //   struct align = 8, 56 is multiple  → total 56 bytes
            //
            // Even more padding paths (5-byte run before int64,
            // mixed 4/2/1-byte-alignment interior, trailing 8-byte
            // struct align with no trailing pad needed).  Flex is
            // registered here but deliberately not queue-backed —
            // the L2 guard path (api.flexzone returns None) is what
            // we want to exercise alongside the complex registration.
            auto flex_spec = complex_mixed_schema();
            ASSERT_TRUE(engine.register_slot_type(flex_spec, "OutFlexFrame",
                                                   "aligned"));

            const size_t slot_sz =
                pylabhub::hub::compute_schema_size(slot_spec, "aligned");
            ASSERT_EQ(slot_sz, 16u);
            // Sanity: engine's ctypes sizeof must agree with
            // compute_schema_size under the padding schema — any
            // divergence here would make the in-script field assign
            // land outside the buffer.
            ASSERT_EQ(engine.type_sizeof("OutSlotFrame"), slot_sz);
            ASSERT_EQ(pylabhub::hub::compute_schema_size(flex_spec, "aligned"),
                      56u);
            ASSERT_EQ(engine.type_sizeof("OutFlexFrame"), 56u);

            auto api = make_api(core);
            ASSERT_TRUE(engine.build_api(*api));

            // 16-byte buffer matching the adversarial slot layout.
            std::vector<std::byte> slot_buf(slot_sz, std::byte{0});
            std::vector<IncomingMessage> msgs;
            auto r = engine.invoke_produce(
                InvokeTx{slot_buf.data(), slot_buf.size()}, msgs);
            EXPECT_EQ(r, InvokeResult::Commit);
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "any in-script assert failure (hasattr fz, "
                   "flexzone() None, update_flexzone_checksum False, "
                   "field values) would increment this counter";

            // C++-side field-offset read-back — verifies ctypes placed
            // each field at the right offset under the adversarial
            // padding schema.  If the padding builder were broken,
            // these would read the wrong bytes (zero from the
            // interior pad, or noise from an adjacent field).
            double  ts    = 0.0;
            uint8_t flag  = 0;
            int32_t count = 0;
            std::memcpy(&ts,    slot_buf.data() +  0, sizeof(ts));
            std::memcpy(&flag,  slot_buf.data() +  8, sizeof(flag));
            std::memcpy(&count, slot_buf.data() + 12, sizeof(count));
            EXPECT_DOUBLE_EQ(ts, 42.5)
                << "tx.slot.ts must round-trip to offset 0 (float64)";
            EXPECT_EQ(flag, 7u)
                << "tx.slot.flag must round-trip to offset 8 (uint8)";
            EXPECT_EQ(count, -3)
                << "tx.slot.count must round-trip to offset 12 — the "
                   "3-byte pad (offset 9..11) between flag and count "
                   "is the padding regression guard";

            engine.finalize();
        },
        "python_engine::invoke_produce_slot_only_no_flexzone_on_invoke",
        Logger::GetLifecycleModule());
}

int invoke_on_inbox_typed_data(const std::string &dir)
{
    // Strengthened over V2 — Python-specific rigor.
    //
    // Python-specific inbox contract (distinct from the Lua sibling,
    // which uses ffi.cast / view semantics):
    //   - Engine builds InboxFrame as a ctypes.Structure subclass,
    //     then wraps it read-only via wrap_readonly_
    //     (python_engine.cpp:849) — writes to msg.data.<field> must
    //     raise AttributeError from _plh_readonly_setattr.
    //   - The read-only wrap's override of __setattr__ means the
    //     from_buffer_copy contract is belt-and-suspenders: the
    //     inbox buffer bytes are copied AND the copy is marked
    //     read-only so script writes cannot reach either side.
    //     Test the observable contract (read-only enforcement on
    //     ALL fields), not the internal implementation detail.
    //
    // V2 only checked msg.data.value + msg.sender_uid — missed
    //   (a) exact type pinning (isinstance ctypes.Structure)
    //   (b) msg.seq projection
    //   (c) adversarial-padding field offsets (1-field schema hid
    //       all padding bugs trivially)
    //   (d) read-only enforcement (the Python-specific invariant).
    //
    // What this body pins:
    //   (1) isinstance(msg.data, ctypes.Structure).
    //   (2) Six distinct padding offsets via multifield_schema
    //       (40 bytes: float64 @ 0, uint8 @ 8, 3-pad, int32 @ 12,
    //       float32[3] @ 16, bytes[8] @ 28, 4-pad trailing).
    //   (3) sender_uid and seq projection paths.
    //   (4) Read-only enforcement — one per field — catches any
    //       regression that unwrapped the read-only guard.
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            // Inbox schema: multifield_schema (adversarial padding,
            // 40 bytes aligned):
            //   ts     float64   @  0 (8)
            //   flag   uint8     @  8 (1)
            //   pad              @  9 (3)   ← align int32
            //   count  int32     @ 12 (4)
            //   values float32[3]@ 16 (12)  ← align 4, ends @ 28
            //   tag    bytes[8]  @ 28 (8)   ← align 1
            //   pad              @ 36 (4)   ← struct align 8
            //   total  = 40
            //
            // Field values are assigned offset-by-offset on the C++
            // side (memcpy into a 40-byte buffer) and read back in the
            // Python callback through the ctypes Structure.  If the
            // engine's ctypes Structure placed any field at the wrong
            // offset, the script-side values would diverge from the
            // expected constants and the assert(s) would fire.
            write_script(script_dir, R"PY(
import ctypes

def on_produce(tx, msgs, api):
    return False

def on_inbox(msg, api):
    # (1) msg.data presence + exact type (Python-specific: ctypes.Structure,
    #     not a duck-typed object with a .value attr).
    assert msg.data is not None, "expected inbox data"
    assert isinstance(msg.data, ctypes.Structure), (
        f"msg.data must be a ctypes.Structure subclass, got {type(msg.data)}")

    # (2) Field-by-field round-trip through the adversarial
    # multifield_schema layout.  Each field read exercises a distinct
    # offset; any ctypes padding regression would misread at least one.
    assert abs(msg.data.ts - 77.0) < 1e-9, (
        f"ts@0 float64 expected 77.0, got {msg.data.ts}")
    assert msg.data.flag == 3, (
        f"flag@8 uint8 expected 3, got {msg.data.flag}")
    assert msg.data.count == -42, (
        f"count@12 int32 expected -42 (after 3-byte interior pad), "
        f"got {msg.data.count}")
    # values@16 float32[3]: array access through ctypes
    assert abs(msg.data.values[0] - 1.5) < 1e-6, (
        f"values[0]@16 expected 1.5, got {msg.data.values[0]}")
    assert abs(msg.data.values[1] - 2.5) < 1e-6, (
        f"values[1]@20 expected 2.5, got {msg.data.values[1]}")
    assert abs(msg.data.values[2] - 3.5) < 1e-6, (
        f"values[2]@24 expected 3.5, got {msg.data.values[2]}")
    # tag@28 bytes[8]: python_helpers.hpp:140 maps "bytes" to
    # c_uint8 * length — comparison requires explicit bytes() conv.
    # (The "string" type_str maps to c_char * length, which IS
    # directly bytes-comparable — distinction pinned here so a
    # regression that conflated the two would surface.)
    assert bytes(msg.data.tag) == b"ABCDEFGH", (
        f"tag@28 expected b'ABCDEFGH', got {bytes(msg.data.tag)!r}")

    # (3) Sender-UID and seq projection paths.
    assert msg.sender_uid == "prod.sender.uid00000001", (
        f"sender_uid expected 'PROD-SENDER-00000001', got {msg.sender_uid!r}")
    assert msg.seq == 7, f"seq expected 7, got {msg.seq}"

    # (4) Read-only enforcement — wrap_readonly_ (python_engine.cpp:849)
    # must reject ALL attribute writes on inbox data.  Pin one attempt
    # per scalar type used in the schema so a regression that only
    # blocked a subset (say, only float fields) would still fail.
    # _plh_readonly_setattr raises AttributeError with a specific
    # message.
    for fld, v in (("ts", 1.0), ("flag", 1), ("count", 1)):
        try:
            setattr(msg.data, fld, v)
            assert False, f"msg.data.{fld} write was not blocked by read-only wrap"
        except AttributeError:
            pass  # expected — wrap_readonly_ blocked the write
    return True
)PY");

            RoleHostCore core;
            PythonEngine engine;
            engine.set_python_venv("");
            ASSERT_TRUE(engine.initialize("test", &core));
            ASSERT_TRUE(engine.load_script(
                script_dir / "script" / "python",
                "__init__.py", "on_produce"));

            // Producer requires OutSlotFrame.  Kept simple here; the
            // adversarial padding belongs on the inbox path (the
            // subject of this test).
            ASSERT_TRUE(engine.register_slot_type(simple_schema(),
                                                  "OutSlotFrame",
                                                  "aligned"));

            // InboxFrame: adversarial multifield_schema (40 bytes).
            auto inbox_spec = multifield_schema();
            ASSERT_TRUE(engine.register_slot_type(inbox_spec, "InboxFrame",
                                                   "aligned"));

            // Also register OutFlexFrame with a distinct complex spec
            // (fz_array_schema: uint32 + float64[2] = 24 bytes).
            // Proves inbox dispatch is independent of flex
            // registration — a regression that accidentally mixed up
            // inbox_type_ro_ and out_fz_type_ in python_engine.cpp
            // would surface as either wrong size or wrong layout on
            // msg.data.
            auto flex_spec = fz_array_schema();
            ASSERT_TRUE(engine.register_slot_type(flex_spec, "OutFlexFrame",
                                                   "aligned"));

            const size_t inbox_sz =
                pylabhub::hub::compute_schema_size(inbox_spec, "aligned");
            ASSERT_EQ(inbox_sz, 40u);
            ASSERT_EQ(engine.type_sizeof("InboxFrame"), inbox_sz);
            // Distinct flex size proves name-keying under the combo.
            ASSERT_EQ(engine.type_sizeof("OutFlexFrame"), 24u);
            ASSERT_NE(engine.type_sizeof("InboxFrame"),
                      engine.type_sizeof("OutFlexFrame"));

            auto api = make_api(core);
            ASSERT_TRUE(engine.build_api(*api));

            // Prepare a 40-byte inbox buffer with one field per
            // distinct offset under the multifield layout.  Using
            // memcpy (not a packed struct literal) so the test is not
            // itself dependent on the host compiler's struct layout.
            std::vector<std::byte> inbox_buf(inbox_sz, std::byte{0});
            const double  ts     = 77.0;
            const uint8_t flag   = 3;
            const int32_t count  = -42;
            const float   vals[3] = {1.5f, 2.5f, 3.5f};
            const char    tag[8] = {'A','B','C','D','E','F','G','H'};
            std::memcpy(inbox_buf.data() +  0, &ts,    sizeof(ts));
            std::memcpy(inbox_buf.data() +  8, &flag,  sizeof(flag));
            std::memcpy(inbox_buf.data() + 12, &count, sizeof(count));
            std::memcpy(inbox_buf.data() + 16, vals,   sizeof(vals));
            std::memcpy(inbox_buf.data() + 28, tag,    sizeof(tag));

            engine.invoke_on_inbox({inbox_buf.data(), inbox_buf.size(),
                                     "prod.sender.uid00000001", 7});
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "Python-side asserts (ctypes.Structure, each field "
                   "at its padded offset, sender_uid, seq, read-only "
                   "enforcement per field) must all have passed — a "
                   "non-zero count pins which contract regressed";

            engine.finalize();
        },
        "python_engine::invoke_on_inbox_typed_data",
        Logger::GetLifecycleModule());
}

int type_sizeof_inbox_frame_returns_correct_size(const std::string &dir)
{
    // Corrected + Python-specific-strengthened over V2.
    //
    // V2 docstring said "28 bytes aligned" but asserted 32 (the value
    // was right, the comment was wrong).  V2 also asserted ONLY
    // equality between OutSlotFrame and InboxFrame (both registered
    // from the same spec — the check was trivially satisfied by any
    // implementation that stored sizes at all).
    //
    // This body pins THREE distinct properties:
    //
    //   (1) Canonical agreement: engine.type_sizeof("InboxFrame") ==
    //       pylabhub::hub::compute_schema_size(spec, "aligned").
    //       Ties the engine's ctypes Structure layout to the role
    //       host's sizing helper.  Python-specific value: a ctypes
    //       Structure's sizeof is driven by _fields_ + _pack_; if the
    //       engine built the wrong _fields_ (e.g., swapped alignment
    //       rules with Lua's ffi cdef) this would diverge.
    //
    //   (2) Exact layout value: 32 bytes for the specific field
    //       sequence below (1 + 7pad + 8 + 2 + 2pad + 4 + 5 + 3pad).
    //       Pins the concrete answer — compute_schema_size could
    //       agree with type_sizeof while BOTH were wrong.
    //
    //   (3) Name-keyed storage (Python-specific): register InboxFrame
    //       with a DIFFERENT spec than OutSlotFrame.  A "last type
    //       wins" or "single-slot storage" regression (plausible for
    //       an engine that accidentally shared one ctypes Structure
    //       for all registrations) would return the SAME size for
    //       both.  Using distinct specs + distinct expected sizes
    //       turns a false-positive test into a real one.
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir,
                         "def on_produce(tx, msgs, api):\n"
                         "    return False\n");

            RoleHostCore core;
            PythonEngine engine;
            engine.set_python_venv("");
            ASSERT_TRUE(engine.initialize("test", &core));
            ASSERT_TRUE(engine.load_script(
                script_dir / "script" / "python",
                "__init__.py", "on_produce"));

            // Three canonical type buckets, each with a distinct
            // adversarial-padding spec from test_schema_helpers.h.
            // The point is to exercise the engine's ctypes Structure
            // builder across three DIFFERENT padding patterns and
            // verify that (a) each registered type's sizeof matches
            // compute_schema_size under that pattern, (b) all three
            // sizes are distinct (name-keying), and (c) the specific
            // padding math matches the helpers' documented totals.
            //
            //   OutSlotFrame ← padding_schema      (16 bytes)
            //     float64 ts + uint8 flag + 3-byte pad + int32 count
            //     + 0 trailing pad = 16 bytes aligned.
            //
            //   InboxFrame   ← complex_mixed_schema (56 bytes)
            //     float64 + float32[3] + uint16 + bytes[5] + string[16]
            //     + 5-byte pad + int64 = 56 bytes aligned.
            //
            //   OutFlexFrame ← multifield_schema   (40 bytes)
            //     float64 + uint8 + 3-byte pad + int32 + float32[3]
            //     + bytes[8] + 4-byte trailing pad = 40 bytes aligned.
            auto slot_spec  = padding_schema();
            auto inbox_spec = complex_mixed_schema();
            auto flex_spec  = multifield_schema();
            ASSERT_TRUE(engine.register_slot_type(slot_spec,  "OutSlotFrame",
                                                   "aligned"));
            ASSERT_TRUE(engine.register_slot_type(inbox_spec, "InboxFrame",
                                                   "aligned"));
            ASSERT_TRUE(engine.register_slot_type(flex_spec,  "OutFlexFrame",
                                                   "aligned"));

            auto api = make_api(core);
            ASSERT_TRUE(engine.build_api(*api));

            const size_t slot_expected =
                pylabhub::hub::compute_schema_size(slot_spec,  "aligned");
            const size_t inbox_expected =
                pylabhub::hub::compute_schema_size(inbox_spec, "aligned");
            const size_t flex_expected =
                pylabhub::hub::compute_schema_size(flex_spec,  "aligned");

            // (1) Canonical agreement — engine ctypes sizeof must
            //     match compute_schema_size under adversarial padding
            //     for all three canonical type buckets.  A ctypes
            //     Structure builder that dropped interior padding or
            //     missed the trailing struct-alignment pad would
            //     diverge on at least one of these three.
            EXPECT_EQ(engine.type_sizeof("OutSlotFrame"), slot_expected);
            EXPECT_EQ(engine.type_sizeof("InboxFrame"),   inbox_expected);
            EXPECT_EQ(engine.type_sizeof("OutFlexFrame"), flex_expected);

            // (2) Exact layout values — pin the multi-field padding
            //     math so a regression in compute_schema_size itself
            //     would fail here (defence in depth: (1) agrees the
            //     two sources, (2) grounds both against a constant).
            EXPECT_EQ(slot_expected,  16u)
                << "padding_schema: float64 + uint8 + 3pad + int32 = 16 bytes";
            EXPECT_EQ(inbox_expected, 56u)
                << "complex_mixed_schema: f64 + f32[3] + u16 + bytes[5] + "
                   "string[16] + 5pad + i64 = 56 bytes";
            EXPECT_EQ(flex_expected,  40u)
                << "multifield_schema: f64 + u8 + 3pad + i32 + f32[3] + "
                   "bytes[8] + 4pad = 40 bytes";

            // (3) Name-keyed storage — ALL three sizes must be
            //     pairwise distinct (16 / 56 / 40).  A "last type
            //     wins" or shared-Structure regression would fold two
            //     or more to the same value.
            EXPECT_NE(engine.type_sizeof("OutSlotFrame"),
                      engine.type_sizeof("InboxFrame"));
            EXPECT_NE(engine.type_sizeof("OutSlotFrame"),
                      engine.type_sizeof("OutFlexFrame"));
            EXPECT_NE(engine.type_sizeof("InboxFrame"),
                      engine.type_sizeof("OutFlexFrame"))
                << "all three type_sizeof values must be distinct — "
                   "any collapse means the engine's type map is "
                   "storing a shared Structure across names";

            // (4) Packed-vs-aligned discriminator (Python-specific).
            //     Register InFlexFrame (a distinct canonical name —
            //     register_slot_type accepts all five canonical
            //     names regardless of role_tag) under "packed"
            //     packing, using the SAME multifield_schema.  Under
            //     aligned packing the schema is 40 bytes; packed
            //     drops the interior 3-byte pad (after uint8 flag)
            //     plus the 4-byte trailing struct-alignment pad —
            //     packed size = 33 bytes.
            //     A regression where "packed" is silently ignored
            //     and the builder falls back to aligned would
            //     return 40.
            ASSERT_TRUE(engine.register_slot_type(flex_spec, "InFlexFrame",
                                                   "packed"));
            const size_t flex_packed =
                pylabhub::hub::compute_schema_size(flex_spec, "packed");
            EXPECT_EQ(flex_packed, 33u)
                << "packed multifield_schema drops 3+4 pad bytes = 33";
            EXPECT_EQ(engine.type_sizeof("InFlexFrame"), flex_packed)
                << "engine must honour packing arg — sizeof(packed) "
                   "must equal compute_schema_size(\"packed\")";
            EXPECT_LT(engine.type_sizeof("InFlexFrame"),
                      engine.type_sizeof("OutFlexFrame"))
                << "packed layout must be strictly smaller than "
                   "aligned for multifield_schema — pins that the "
                   "packing arg is honoured end-to-end";

            engine.finalize();
        },
        "python_engine::type_sizeof_inbox_frame_returns_correct_size",
        Logger::GetLifecycleModule());
}

int invoke_on_inbox_missing_type_reports_error(const std::string &dir)
{
    // Strengthened over V2 (EXPECT_GE → EXPECT_EQ == 1u) + ERROR text
    // pinning via expected_error_substrings at the parent level.  If
    // the missing-type guard ever started cascading (e.g. invoking the
    // script anyway with a None data, which would raise in the
    // assertion-free script body below), script_error_count would be
    // >= 2 and the EQ assertion would fail loudly.
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir, R"PY(
def on_produce(tx, msgs, api):
    return False

def on_inbox(msg, api):
    return True
)PY");

            RoleHostCore core;
            PythonEngine engine;
            engine.set_python_venv("");
            ASSERT_TRUE(engine.initialize("test", &core));
            ASSERT_TRUE(engine.load_script(
                script_dir / "script" / "python",
                "__init__.py", "on_produce"));

            auto spec = simple_schema();
            ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame",
                                                   "aligned"));
            // Deliberately NO InboxFrame registration.

            auto api = make_api(core);
            ASSERT_TRUE(engine.build_api(*api));

            float raw = 1.0f;
            engine.invoke_on_inbox({&raw, sizeof(raw),
                                     "cons.sender.uid00000001", 1});
            EXPECT_EQ(engine.script_error_count(), 1u)
                << "missing InboxFrame must increment script_error_count "
                   "by EXACTLY 1 — more would mean the guard cascaded or "
                   "the callback was invoked anyway";

            engine.finalize();
        },
        "python_engine::invoke_on_inbox_missing_type_reports_error",
        Logger::GetLifecycleModule());
}

int invoke_produce_discard_on_false_but_python_wrote_slot(const std::string &dir)
{
    // NEW test — pins the production contract that the engine does NOT
    // roll back Python-side writes to tx.slot when the callback
    // returns False.  The slot buffer is caller-owned; the return
    // value only tells the caller whether to publish the slot
    // downstream.  A user who writes `tx.slot.value = 42; return
    // False` gets:
    //   result == Discard  (caller drops the slot)
    //   buf    == 42.0     (caller's buffer was mutated in place)
    //
    // This subtlety matters because a role host implementing
    // "conditionally publish" may reuse the same buffer for the next
    // iteration; if they expected the engine to clear it on Discard
    // they'd get stale data.  Mirrors the Lua gap-fill
    // invoke_produce_discard_on_false_but_lua_wrote_slot.
    return produce_worker_with_script(
        dir,
        "python_engine::invoke_produce_discard_on_false_but_python_wrote_slot",
        R"PY(
def on_produce(tx, msgs, api):
    if tx.slot is not None:
        tx.slot.value = 42.0
    return False
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            float buf = 0.0f;
            std::vector<IncomingMessage> msgs;
            auto result = engine.invoke_produce(InvokeTx{&buf, sizeof(buf)},
                                                 msgs);
            EXPECT_EQ(result, InvokeResult::Discard)
                << "explicit `return False` must yield Discard";
            EXPECT_FLOAT_EQ(buf, 42.0f)
                << "Python write to tx.slot.value must propagate to the "
                   "caller's buffer regardless of return value — the engine "
                   "does NOT roll back on Discard.";
            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

// ── Generic invoke + threading (chunk 10) ──────────────────────────────────
//
// Python's generic invoke is dispatch-queued because
// supports_multi_state() == false.  Owner-thread calls execute
// synchronously via execute_direct_; non-owner calls push a
// PendingRequest onto request_queue_ and block on a std::future.
// process_pending_() drains the queue FIFO — it is called at the end
// of every hot-path invoke (invoke_produce, invoke_consume, etc.) and
// at the start of invoke_on_inbox.  finalize() drains + cancels
// pending promises with InvokeStatus::EngineShutdown.
//
// This chunk's workers pin the observable behaviour of each path.

int invoke_existing_function_returns_true(const std::string &dir)
{
    // Strengthened over V2.  V2 only checked the bool return — a
    // regression where invoke() accidentally returned true without
    // calling the function (e.g., a short-circuit in execute_direct_)
    // would pass silently.  This body pins that the callback
    // ACTUALLY RAN by setting a module-level marker and verifying it
    // via engine.eval() — eval is also admin-queued so it composes
    // cleanly with invoke().
    return produce_worker_with_script(
        dir, "python_engine::invoke_existing_function_returns_true",
        R"PY(
heartbeat_ran = False

def on_produce(tx, msgs, api):
    return True

def on_heartbeat():
    global heartbeat_ran
    heartbeat_ran = True
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            EXPECT_TRUE(engine.invoke("on_heartbeat"))
                << "invoke of an existing no-arg function must return true";

            // Verify the function actually ran by reading back the
            // module-level marker via eval().  eval returns an
            // InvokeResponse whose .value is nlohmann::json — we
            // compare against a JSON bool directly.
            auto r = engine.eval("heartbeat_ran");
            EXPECT_EQ(r.status, pylabhub::scripting::InvokeStatus::Ok);
            EXPECT_EQ(r.value, true)
                << "on_heartbeat must have set heartbeat_ran=True — "
                   "invoke() returned true but dispatch did not run "
                   "the function (got eval result: " << r.value << ")";
            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

int invoke_non_existent_function_returns_false(const std::string &dir)
{
    // Strengthened over V2.  V2 only checked false return.  The
    // important distinction here — which a regression could flip —
    // is that a NotFound dispatch must NOT increment
    // script_error_count.  "no such function" is a lookup failure
    // (execute_direct_ returns InvokeStatus::NotFound with no call
    // attempted), not a script runtime failure.  Conflating the two
    // would pollute script_error_count with admin-side lookup misses
    // and defeat its signal value.
    return produce_worker_with_script(
        dir, "python_engine::invoke_non_existent_function_returns_false",
        "def on_produce(tx, msgs, api):\n    return True\n",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            EXPECT_FALSE(engine.invoke("no_such_function"));
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "NotFound dispatch must NOT increment "
                   "script_error_count — that counter is reserved for "
                   "actual script runtime errors (raised exceptions), "
                   "not lookup failures";
        });
}

int invoke_empty_name_returns_false(const std::string &dir)
{
    // Strengthened over V2 — same no-error-count pin as
    // non_existent_function.  Also pins that the guard happens
    // before py::getattr (the empty-string check in execute_direct_
    // at python_engine.cpp:626) rather than relying on
    // getattr("") throwing — both paths return false, but the
    // former costs no GIL acquire.
    return produce_worker_with_script(
        dir, "python_engine::invoke_empty_name_returns_false",
        "def on_produce(tx, msgs, api):\n    return True\n",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            EXPECT_FALSE(engine.invoke(""));
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "empty-name dispatch must NOT increment "
                   "script_error_count — early guard, not a script "
                   "runtime failure";
        });
}

int invoke_script_error_returns_false_and_increments_errors(
    const std::string &dir)
{
    // Directly maps to V2 — strengthened only by pinning the
    // ERROR-log substring at the parent level (caller passes
    // expected_error_substrings = {"intentional test error"}).  V2
    // already checked false return + count == 1.
    return produce_worker_with_script(
        dir,
        "python_engine::invoke_script_error_returns_false_and_increments_errors",
        R"PY(
def on_produce(tx, msgs, api):
    return True

def bad_func():
    raise RuntimeError("intentional test error")
)PY",
        [](PythonEngine &engine, RoleHostCore &core) {
            EXPECT_FALSE(engine.invoke("bad_func"))
                << "invoke of a function that raises must return false";
            EXPECT_EQ(core.script_error_count(), 1u)
                << "RuntimeError from invoked function must increment "
                   "script_error_count exactly once";
            EXPECT_EQ(engine.script_error_count(), 1u);
        });
}

int invoke_from_non_owner_thread_queued(const std::string &dir)
{
    // Strengthened over V2.  V2 spun a non-owner thread + drove
    // process_pending_ via invoke_produce in a bounded loop, then
    // asserted invoke returned true.  Missing: proof the callback
    // ACTUALLY RAN in the owner thread (via the expected queue
    // path), not accidentally by the non-owner thread itself
    // (which would violate GIL ownership).
    //
    // This body adds:
    //   (1) A module-level counter incremented by on_heartbeat.
    //       After drain, eval("call_count") reads the final value.
    //       Mismatch pins a dispatch-count discrepancy.
    //   (2) The non-owner thread's invoke MUST block until drained
    //       — verified by observing that "done" is still false
    //       immediately after the thread is spawned (before any
    //       invoke_produce call on the owner side).
    return produce_worker_with_script(
        dir, "python_engine::invoke_from_non_owner_thread_queued",
        R"PY(
call_count = 0

def on_produce(tx, msgs, api):
    return True

def on_heartbeat():
    global call_count
    call_count += 1
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            using pylabhub::tests::helper::poll_until;
            std::atomic<bool> done{false};
            bool result = false;

            std::thread t([&] {
                result = engine.invoke("on_heartbeat");
                done.store(true, std::memory_order_release);
            });

            // Queue-dispatch pin — wait for the thread to push its
            // request onto the dispatch queue (observable via the
            // status probe), then assert `done` is STILL false.
            // Together these prove the thread has entered invoke()
            // AND is currently blocked on future.get() (not yet
            // drained) — the queue-blocking contract.  If a
            // regression made invoke() execute synchronously from
            // the non-owner thread, the thread would complete before
            // ever enqueueing and the poll would time out.
            ASSERT_TRUE(poll_until(
                [&] { return engine.pending_script_engine_request_count() == 1; },
                std::chrono::seconds{1}))
                << "non-owner invoke was expected to enqueue a "
                   "request; queue depth stayed at 0 past timeout";
            EXPECT_FALSE(done.load(std::memory_order_acquire))
                << "thread has enqueued but must still be blocked on "
                   "future.get() (not yet drained) — pins the queued-"
                   "dispatch contract";

            // Owner drains via hot-path invokes.  Bounded loop with
            // timeout to detect deadlock.
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!done.load(std::memory_order_acquire))
            {
                ASSERT_LT(std::chrono::steady_clock::now(), deadline)
                    << "Timeout: non-owner invoke was never drained "
                       "by process_pending_()";
                std::vector<IncomingMessage> msgs;
                engine.invoke_produce({nullptr, 0}, msgs);
                std::this_thread::yield();
            }
            t.join();

            EXPECT_TRUE(result) << "non-owner invoke must ultimately "
                                   "succeed after drain";

            // Verify the callback actually ran in the owner thread
            // (eval reads module-level state).  r.value is
            // nlohmann::json — compare against a JSON integer.
            auto r = engine.eval("call_count");
            EXPECT_EQ(r.status, pylabhub::scripting::InvokeStatus::Ok);
            EXPECT_EQ(r.value, 1)
                << "on_heartbeat must have run exactly once — got "
                   "call_count=" << r.value;
            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

int invoke_from_non_owner_thread_finalize_unblocks(const std::string &dir)
{
    // Strengthened over V2.  V2 spawned ONE non-owner thread, called
    // finalize, verified invoke returned false.  A regression where
    // finalize only cancelled the FIRST pending request (e.g., a
    // for-loop that breaks after one .set_value) would pass V2.
    //
    // This body queues THREE non-owner invokes, then calls finalize
    // from the owner thread, and verifies ALL three unblock with
    // false return.  The count pins the "drain all" contract of
    // finalize_engine_ at python_engine.cpp:537-540.
    return produce_worker_with_script(
        dir,
        "python_engine::invoke_from_non_owner_thread_finalize_unblocks",
        R"PY(
def on_produce(tx, msgs, api):
    return True

def slow_func():
    pass
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            using pylabhub::tests::helper::poll_until;
            constexpr int kPending = 3;
            std::atomic<int> false_returns{0};

            std::vector<std::thread> threads;
            threads.reserve(kPending);
            for (int i = 0; i < kPending; ++i)
            {
                threads.emplace_back([&] {
                    // Must block (queue not drained) — then return
                    // false when finalize cancels.
                    bool r = engine.invoke("slow_func");
                    if (!r)
                        false_returns.fetch_add(1,
                                                std::memory_order_acq_rel);
                });
            }

            // Use the engine's status probe to wait for ALL 3
            // requests to be on the queue — the poll observes the
            // exact condition we need ("all 3 pushed, none drained")
            // rather than guessing via time.  If only 2 make it onto
            // the queue and the 3rd hits the pre-queue early-return
            // path (e.g., is_accepting() flipped), the poll times
            // out and the test fails loudly instead of silently
            // passing via the wrong path.
            ASSERT_TRUE(poll_until(
                [&] { return engine.pending_script_engine_request_count()
                         == static_cast<size_t>(kPending); },
                std::chrono::seconds{2}))
                << "expected " << kPending << " queued invokes, got "
                << engine.pending_script_engine_request_count();

            engine.finalize();
            for (auto &t : threads) t.join();

            EXPECT_EQ(false_returns.load(), kPending)
                << "finalize must cancel ALL pending invokes, not "
                   "just one — got " << false_returns.load()
                << " false returns of " << kPending << " expected";
        });
}

int invoke_concurrent_non_owner_threads(const std::string &dir)
{
    // Strengthened over V2.  V2 ran 4 threads × 10 calls and
    // verified the total success count — pinned that no invocations
    // got lost in concurrent enqueue, but did NOT verify the
    // callbacks were actually executed by the owner (would have
    // passed even if every "invoke" short-circuited to return true).
    //
    // This body adds:
    //   (a) Module-level counter incremented per call — eval reads
    //       the final total.  Must match kThreads * kCallsPerThread.
    //   (b) script_error_count MUST remain 0 — a regression that
    //       treated queue cancellation as an error would show up here.
    return produce_worker_with_script(
        dir, "python_engine::invoke_concurrent_non_owner_threads",
        R"PY(
call_count = 0

def on_produce(tx, msgs, api):
    return True

def on_heartbeat():
    global call_count
    call_count += 1
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            constexpr int kThreads        = 4;
            constexpr int kCallsPerThread = 10;
            constexpr int kExpected       = kThreads * kCallsPerThread;

            std::atomic<int> success_count{0};
            std::vector<std::thread> threads;
            for (int i = 0; i < kThreads; ++i)
            {
                threads.emplace_back([&] {
                    for (int j = 0; j < kCallsPerThread; ++j)
                        if (engine.invoke("on_heartbeat"))
                            success_count.fetch_add(
                                1, std::memory_order_relaxed);
                });
            }

            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (success_count.load(std::memory_order_relaxed) < kExpected)
            {
                ASSERT_LT(std::chrono::steady_clock::now(), deadline)
                    << "Timeout: only " << success_count.load()
                    << " of " << kExpected << " invokes completed";
                std::vector<IncomingMessage> msgs;
                engine.invoke_produce({nullptr, 0}, msgs);
                std::this_thread::yield();
            }
            for (auto &t : threads) t.join();

            EXPECT_EQ(success_count.load(), kExpected);

            // Side-effect verification: the owner thread MUST have
            // actually executed all 40 callbacks.  This pins that
            // every queued request was drained to execute_direct_,
            // not short-circuited somehow.
            auto r = engine.eval("call_count");
            EXPECT_EQ(r.status, pylabhub::scripting::InvokeStatus::Ok);
            EXPECT_EQ(r.value, kExpected)
                << "on_heartbeat must have run exactly " << kExpected
                << " times — got call_count=" << r.value;
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "concurrent queued invokes must not increment "
                   "script_error_count";
        });
}

int invoke_after_finalize_returns_false(const std::string &dir)
{
    // Strengthened over V2 — coverage gap fill.  V2 called finalize
    // then invoked from the SAME (owner) thread and asserted false.
    // The guard at python_engine.cpp:569 (is_accepting()) covers
    // both owner and non-owner paths, but V2 only exercised one.
    // A regression that guarded only the queued path (e.g., the
    // accepting_ check inside the queue_mu_ section only) would
    // pass V2 but fail here — owner-path direct dispatch could
    // accidentally run after finalize.
    //
    // This body tests BOTH paths:
    //   (1) Owner thread: invoke after finalize → false.
    //   (2) Non-owner thread: invoke after finalize → false,
    //       completes immediately (is_accepting() returns false
    //       before ever touching the queue).
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir, R"PY(
def on_produce(tx, msgs, api):
    return True

def on_heartbeat():
    pass
)PY");

            RoleHostCore core;
            PythonEngine engine;
            std::unique_ptr<RoleAPIBase> api;
            ASSERT_TRUE(setup_role_engine(engine, core, script_dir, api,
                                            RoleKind::Producer));
            engine.finalize();

            // (1) Owner-thread path.
            EXPECT_FALSE(engine.invoke("on_heartbeat"))
                << "owner-thread invoke after finalize must return false";

            // (2) Non-owner-thread path.
            bool non_owner_result = true; // expect false
            std::thread t([&] {
                non_owner_result = engine.invoke("on_heartbeat");
            });
            t.join();
            EXPECT_FALSE(non_owner_result)
                << "non-owner-thread invoke after finalize must "
                   "also return false — the is_accepting() guard at "
                   "python_engine.cpp:569 covers both paths";
        },
        "python_engine::invoke_after_finalize_returns_false",
        Logger::GetLifecycleModule());
}

int invoke_with_args_calls_function(const std::string &dir)
{
    // Strengthened over V2.  V2 called invoke("greet", {"name":
    // "test"}) and only verified true return.  Two gaps:
    //
    //   (a) Args were never verified to reach the callee — the
    //       function accepted **kwargs and ignored them.  A
    //       regression that dropped args (e.g. execute_direct_
    //       overload calling fn() instead of fn(**args)) would
    //       pass V2 silently.
    //
    //   (b) The no-args branch (args.is_null() || args.empty())
    //       was not exercised on the args-overload path — that
    //       branch is what execute_direct_ uses when the json arg
    //       is empty dict.  Regression: the branch could silently
    //       call fn() without kwargs expansion.
    //
    // This body pins both:
    //   (1) With-args: kwargs reach the callee (module-level
    //       tracker stores kwargs["name"]).
    //   (2) Empty-dict args: the EMPTY-branch of execute_direct_
    //       still dispatches and the function sees no kwargs.
    //   (3) Multi-type args: int + string + bool — ensures JSON→
    //       Python conversion handles the common types.
    return produce_worker_with_script(
        dir, "python_engine::invoke_with_args_calls_function",
        R"PY(
last_kwargs_name  = ""
last_kwargs_count = 0
last_kwargs_flag  = None
empty_call_count  = 0

def on_produce(tx, msgs, api):
    return True

def greet(**kwargs):
    global last_kwargs_name, last_kwargs_count, last_kwargs_flag
    last_kwargs_name  = kwargs.get("name",  "")
    last_kwargs_count = kwargs.get("count", 0)
    last_kwargs_flag  = kwargs.get("flag",  None)

def no_arg_fn(**kwargs):
    global empty_call_count
    # Empty args path MUST dispatch to this — and kwargs must be
    # empty (not a residual dict from the previous call).
    assert len(kwargs) == 0, f"expected no kwargs, got {kwargs}"
    empty_call_count += 1
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            // (1) + (3) multi-type args reach the callee.
            nlohmann::json args = {
                {"name",  "test"},
                {"count", 42},
                {"flag",  true},
            };
            EXPECT_TRUE(engine.invoke("greet", args));
            EXPECT_EQ(engine.script_error_count(), 0u);

            auto r_name = engine.eval("last_kwargs_name");
            EXPECT_EQ(r_name.status,
                      pylabhub::scripting::InvokeStatus::Ok);
            EXPECT_EQ(r_name.value, "test")
                << "kwargs['name'] must reach the callee — pins "
                   "fn(**args) expansion on the args-overload path";
            auto r_count = engine.eval("last_kwargs_count");
            EXPECT_EQ(r_count.value, 42);
            auto r_flag = engine.eval("last_kwargs_flag");
            EXPECT_EQ(r_flag.value, true);

            // (2) Empty-args dispatch must still call the function.
            EXPECT_TRUE(engine.invoke("no_arg_fn", nlohmann::json::object()));
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "empty-dict args → in-script assert on "
                   "len(kwargs)==0 must have passed";

            auto r_count2 = engine.eval("empty_call_count");
            EXPECT_EQ(r_count2.value, 1)
                << "no_arg_fn must have run exactly once under the "
                   "empty-args branch (execute_direct_'s "
                   "args.empty() path)";
        });
}

int invoke_with_args_from_non_owner_thread(const std::string &dir)
{
    // Coverage gap fill (static-review I2).  Both
    // invoke_from_non_owner_thread_queued (no args) and
    // invoke_with_args_calls_function (owner + args) exercise parts
    // of the argument-carrying dispatch path but not the combined
    // case.  A regression in the queued args path — e.g., the
    // PendingRequest's json args getting dropped or corrupted during
    // the queue_mu_-protected push_back at python_engine.cpp:594-602
    // — would pass both of the existing tests silently.
    //
    // This body runs greet(**args) from a non-owner thread, drives
    // the owner-side drain via invoke_produce in a bounded loop, and
    // verifies the kwargs arrived intact via eval().
    return produce_worker_with_script(
        dir, "python_engine::invoke_with_args_from_non_owner_thread",
        R"PY(
last_kwargs_name  = ""
last_kwargs_count = 0

def on_produce(tx, msgs, api):
    return True

def greet(**kwargs):
    global last_kwargs_name, last_kwargs_count
    last_kwargs_name  = kwargs.get("name",  "")
    last_kwargs_count = kwargs.get("count", 0)
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            using pylabhub::tests::helper::poll_until;
            std::atomic<bool> done{false};
            bool              result = false;

            std::thread t([&] {
                nlohmann::json args = {{"name", "queued"}, {"count", 99}};
                result = engine.invoke("greet", args);
                done.store(true, std::memory_order_release);
            });

            // Queue the request, verify it enqueued, then drain.
            ASSERT_TRUE(poll_until(
                [&] { return engine.pending_script_engine_request_count() == 1; },
                std::chrono::seconds{1}))
                << "non-owner invoke(name, args) did not enqueue";

            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!done.load(std::memory_order_acquire))
            {
                ASSERT_LT(std::chrono::steady_clock::now(), deadline);
                std::vector<IncomingMessage> msgs;
                engine.invoke_produce({nullptr, 0}, msgs);
                std::this_thread::yield();
            }
            t.join();

            EXPECT_TRUE(result) << "queued invoke(name, args) must "
                                   "succeed after drain";

            // Verify kwargs survived the queue round-trip intact.
            auto r_name = engine.eval("last_kwargs_name");
            EXPECT_EQ(r_name.value, "queued")
                << "kwargs['name'] must survive the queue_mu_-protected "
                   "push_back of PendingRequest — pins args not dropped "
                   "on the non-owner-thread path";
            auto r_count = engine.eval("last_kwargs_count");
            EXPECT_EQ(r_count.value, 99);
            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

// ── Eval + shared data (chunk 11) ──────────────────────────────────────────

int eval_returns_scalar_result(const std::string &dir)
{
    // Strengthened over V2.  V2 tested 3 scalar types (int, str,
    // bool).  Missing:
    //   - float (py_to_json path for py::float_ → json double)
    //   - None (py_to_json path for py::none → json null)
    //   - list (recursive container conversion)
    //   - dict (recursive container conversion)
    //   - expression evaluation in module namespace (eval_direct_
    //     uses module_.attr("__dict__") as globals — module-level
    //     names must be reachable)
    //
    // This body pins all six paths, plus one negative test (empty
    // code → InvokeStatus::NotFound, distinct from ScriptError).
    return produce_worker_with_script(
        dir, "python_engine::eval_returns_scalar_result",
        R"PY(
# Module-level names for the namespace-reachability eval case.
my_magic_number = 2026

def on_produce(tx, msgs, api):
    return True
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            using pylabhub::scripting::InvokeStatus;

            // (1) int → json number
            auto r_int = engine.eval("42");
            EXPECT_EQ(r_int.status, InvokeStatus::Ok);
            EXPECT_EQ(r_int.value, 42);

            // (2) string → json string
            auto r_str = engine.eval("'hello'");
            EXPECT_EQ(r_str.status, InvokeStatus::Ok);
            EXPECT_EQ(r_str.value, "hello");

            // (3) bool → json bool
            auto r_bool = engine.eval("True");
            EXPECT_EQ(r_bool.status, InvokeStatus::Ok);
            EXPECT_EQ(r_bool.value, true);

            // (4) float → json number (double)
            auto r_float = engine.eval("3.25");
            EXPECT_EQ(r_float.status, InvokeStatus::Ok);
            EXPECT_EQ(r_float.value, 3.25);

            // (5) None → json null — nlohmann::json null compares
            //     equal to nullptr.
            auto r_none = engine.eval("None");
            EXPECT_EQ(r_none.status, InvokeStatus::Ok);
            EXPECT_TRUE(r_none.value.is_null())
                << "eval('None') must produce json null; got "
                << r_none.value;

            // (6) list → json array (recursive py_to_json path).
            auto r_list = engine.eval("[1, 2, 3]");
            EXPECT_EQ(r_list.status, InvokeStatus::Ok);
            ASSERT_TRUE(r_list.value.is_array());
            EXPECT_EQ(r_list.value.size(), 3u);
            EXPECT_EQ(r_list.value[0], 1);
            EXPECT_EQ(r_list.value[2], 3);

            // (7) dict → json object.
            auto r_dict = engine.eval("{'a': 1, 'b': 'x'}");
            EXPECT_EQ(r_dict.status, InvokeStatus::Ok);
            ASSERT_TRUE(r_dict.value.is_object());
            EXPECT_EQ(r_dict.value["a"], 1);
            EXPECT_EQ(r_dict.value["b"], "x");

            // (8) Module namespace reachability — my_magic_number
            //     is defined in the script.  eval_direct_ uses
            //     module_.attr("__dict__") as globals, so module-
            //     level names MUST resolve.  A regression that
            //     swapped to a fresh dict would raise NameError.
            auto r_mod = engine.eval("my_magic_number");
            EXPECT_EQ(r_mod.status, InvokeStatus::Ok);
            EXPECT_EQ(r_mod.value, 2026);

            // (9) Empty code → NotFound (distinct from ScriptError).
            //     Pins eval_direct_'s early empty check at
            //     python_engine.cpp:679-680.
            auto r_empty = engine.eval("");
            EXPECT_EQ(r_empty.status, InvokeStatus::NotFound)
                << "eval('') must return NotFound (not ScriptError, "
                   "not Ok) — pins the early empty-code guard";
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "scalar evals + empty-code NotFound must not "
                   "increment script_error_count";
        });
}

int eval_error_returns_empty(const std::string &dir)
{
    // Strengthened over V2.  V2 tested one error case (undefined
    // variable → ScriptError).  Missing:
    //   - Syntax-error case (the py::eval path surfaces these as
    //     py::error_already_set too — verifies the shared
    //     on_python_error_ funnel handles both runtime and syntax).
    //   - Each distinct error path must increment script_error_count
    //     by exactly 1 (no double-counting, no silent swallow).
    //   - Each error must log the Python exception message to ERROR
    //     (pinned via parent expected_error_substrings on at least
    //     one distinctive substring per case).
    //
    // Parent substrings ("NameError" and "SyntaxError") pin the
    // translator at python_engine.cpp:693 (on_python_error_) is
    // producing the Python exception class name in the ERROR line
    // — not just a generic "eval failed" placeholder.
    return produce_worker_with_script(
        dir, "python_engine::eval_error_returns_empty",
        "def on_produce(tx, msgs, api):\n    return True\n",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            using pylabhub::scripting::InvokeStatus;

            // (1) NameError — undefined variable.
            auto r1 = engine.eval("undefined_variable_xyz");
            EXPECT_EQ(r1.status, InvokeStatus::ScriptError);
            EXPECT_EQ(engine.script_error_count(), 1u)
                << "NameError in eval must increment by exactly 1";

            // (2) SyntaxError — malformed expression.  py::eval uses
            //     Py_eval_input so statements (like `x = 1`) also
            //     parse as syntax errors, which is what we want here.
            auto r2 = engine.eval("1 2 3 invalid");
            EXPECT_EQ(r2.status, InvokeStatus::ScriptError);
            EXPECT_EQ(engine.script_error_count(), 2u)
                << "second eval error must increment to 2 — no "
                   "accumulation bug or double-count";

            // (3) ZeroDivisionError — distinct runtime error path.
            auto r3 = engine.eval("1 / 0");
            EXPECT_EQ(r3.status, InvokeStatus::ScriptError);
            EXPECT_EQ(engine.script_error_count(), 3u);
        });
}

int shared_data_persists_across_callbacks(const std::string &dir)
{
    // Strengthened over V2.  V2 stored an int counter + incremented
    // from on_produce + read via eval("get_counter()").  That shape
    // misses three real contracts:
    //
    //   (a) api.shared_data is a Python dict held by reference in
    //       ProducerAPI::shared_data_ (producer_api.cpp: readwrite
    //       binding).  The SAME dict object must be seen by every
    //       callback on every invocation — not a fresh dict per
    //       call.  V2's counter test would pass even if a fresh
    //       dict were supplied IF the counter re-incremented each
    //       time (e.g., if the engine deep-copied the dict back).
    //
    //   (b) Mutable container updates (list.append, dict[key] = ...)
    //       must propagate — the pybind11 binding exposes a live
    //       py::dict, not a JSON-round-tripped copy.
    //
    //   (c) Cross-callback persistence spans on_init, on_produce,
    //       on_stop (and eval) — V2 only covered on_init →
    //       on_produce.  A regression that cleared shared_data on
    //       finalize's cleanup path (before on_stop) would pass V2
    //       but fail here.
    return produce_worker_with_script(
        dir, "python_engine::shared_data_persists_across_callbacks",
        R"PY(
# Module-level reference so eval() can read shared_data without an
# api param (eval-called functions don't receive api).
_api_ref = None

def on_init(api):
    global _api_ref
    _api_ref = api
    # Identity anchor: remember the dict's id so we can detect a
    # fresh-dict regression (id() must stay constant across callbacks).
    api.shared_data["_id_at_init"] = id(api.shared_data)
    api.shared_data["counter"]     = 0
    api.shared_data["history"]     = []            # mutable list
    api.shared_data["meta"]        = {"seen": 0}   # nested dict
    api.shared_data["last_stage"]  = "init"

def on_produce(tx, msgs, api):
    # Identity check: dict instance must be the SAME one on_init
    # populated — a regression that rebuilt it per-call (or
    # copy-back after on_init) would fail here.
    assert id(api.shared_data) == api.shared_data["_id_at_init"], (
        f"api.shared_data identity changed: expected "
        f"{api.shared_data['_id_at_init']}, got {id(api.shared_data)}")
    api.shared_data["counter"] += 1
    api.shared_data["history"].append(api.shared_data["counter"])
    api.shared_data["meta"]["seen"] += 1
    api.shared_data["last_stage"] = "produce"
    return True

def on_stop(api):
    # Must run BEFORE shared_data gets cleared.  Pin that on_stop
    # still sees the accumulated state (V2 never covered this).
    assert api.shared_data["counter"] == 5, (
        f"on_stop expected counter=5, got {api.shared_data['counter']}")
    api.shared_data["last_stage"] = "stop"

def get_counter():
    return _api_ref.shared_data["counter"]

def get_history():
    return _api_ref.shared_data["history"]

def get_last_stage():
    return _api_ref.shared_data["last_stage"]

def get_meta_seen():
    return _api_ref.shared_data["meta"]["seen"]

def get_id_matches():
    return id(_api_ref.shared_data) == _api_ref.shared_data["_id_at_init"]
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            using pylabhub::scripting::InvokeStatus;

            engine.invoke_on_init();
            ASSERT_EQ(engine.script_error_count(), 0u) << "on_init failed";

            // 5 on_produce calls — each must see the same dict.
            std::vector<IncomingMessage> msgs;
            for (int i = 0; i < 5; ++i)
                engine.invoke_produce({nullptr, 0}, msgs);
            ASSERT_EQ(engine.script_error_count(), 0u)
                << "on_produce sequence failed — identity check or "
                   "mutation propagation broken";

            // (1) Scalar persistence (V2 parity).
            auto r_count = engine.eval("get_counter()");
            EXPECT_EQ(r_count.status, InvokeStatus::Ok);
            EXPECT_EQ(r_count.value, 5);

            // (2) Mutable-list append propagates through the binding
            //     — the py::dict returned to Python is the SAME
            //     dict C++ stores, not a JSON copy.
            auto r_hist = engine.eval("get_history()");
            EXPECT_EQ(r_hist.status, InvokeStatus::Ok);
            ASSERT_TRUE(r_hist.value.is_array());
            ASSERT_EQ(r_hist.value.size(), 5u);
            EXPECT_EQ(r_hist.value[0], 1);
            EXPECT_EQ(r_hist.value[4], 5);

            // (3) Nested-dict update propagates.
            auto r_meta = engine.eval("get_meta_seen()");
            EXPECT_EQ(r_meta.status, InvokeStatus::Ok);
            EXPECT_EQ(r_meta.value, 5);

            // (4) Dict identity across callbacks — the id() captured
            //     at on_init time still matches AFTER 5 on_produce
            //     calls.  Pins that the binding is reference-based,
            //     not copy-on-read.
            auto r_id = engine.eval("get_id_matches()");
            EXPECT_EQ(r_id.status, InvokeStatus::Ok);
            EXPECT_EQ(r_id.value, true)
                << "api.shared_data must be the SAME dict across "
                   "callbacks — the id(dict) captured in on_init "
                   "must match id(dict) observed post-on_produce";

            // (5) on_stop sees accumulated state — pins that
            //     shared_data is NOT cleared before on_stop runs.
            engine.invoke_on_stop();
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "on_stop assertion on counter==5 must have passed";

            auto r_stage = engine.eval("get_last_stage()");
            EXPECT_EQ(r_stage.status, InvokeStatus::Ok);
            EXPECT_EQ(r_stage.value, "stop")
                << "on_stage must have been able to update shared_data "
                   "(last_stage='stop') — shared_data remained live "
                   "at on_stop time";
        });
}

// ── API diagnostics + identity (chunk 12) ──────────────────────────────────

namespace
{
/// Shared body for live-read pinning tests.  `api_accessor` is the name
/// of the Python-visible accessor (e.g. "loop_overrun_count").
/// `expected_sequence` is the value sequence the core will drive across
/// successive invokes; `drive` is a C++ callable that mutates the core
/// to produce the i-th expected value.  Verifies (a) each invoke sees
/// the live value, (b) total invoke count matches sequence length.
template <typename Drive>
void run_core_live_read_test(PythonEngine &engine, RoleHostCore &core,
                             const std::vector<int64_t> &expected_sequence,
                             Drive &&drive)
{
    std::vector<IncomingMessage> msgs;
    float buf = 0.0f;
    for (size_t i = 0; i < expected_sequence.size(); ++i)
    {
        drive(core, i, expected_sequence[i]);
        engine.invoke_produce({&buf, sizeof(buf)}, msgs);
        EXPECT_EQ(engine.script_error_count(), 0u)
            << "call " << i
            << ": accessor must read LIVE from RoleHostCore per call, "
               "not a build_api-time snapshot";
    }
    // S4 pin: script saw exactly N invocations.
    auto r = engine.eval("_call");
    EXPECT_EQ(r.value, static_cast<int64_t>(expected_sequence.size()))
        << "script must have been invoked "
        << expected_sequence.size() << " times";
}

/// Shared script source for the live-read pattern.  The placeholders
/// use `@NAME@` delimiters (not `{NAME}`) specifically so they don't
/// collide with Python f-string braces — the f-string in the assert
/// message needs single `{` to interpolate variables, and double
/// `{{` in an f-string means a literal `{` (not interpolation).
constexpr const char *kLiveReadScriptTemplate = R"PY(
_call = 0
_expected = [@SEQUENCE@]

def on_produce(tx, msgs, api):
    global _call
    v = api.@ACCESSOR@()
    assert v == _expected[_call], (
        f"call {_call}: expected {_expected[_call]}, got {v} — "
        f"accessor must re-read RoleHostCore per call")
    _call += 1
    return False
)PY";

/// Build the live-read script body with the given accessor + sequence.
std::string build_live_read_script(const char *accessor,
                                   const std::vector<int64_t> &seq)
{
    std::string seq_str;
    for (size_t i = 0; i < seq.size(); ++i)
    {
        if (i > 0) seq_str += ", ";
        seq_str += std::to_string(seq[i]);
    }
    std::string src = kLiveReadScriptTemplate;
    auto replace_first = [&src](const std::string &needle,
                                 const std::string &value) {
        size_t pos = src.find(needle);
        if (pos != std::string::npos)
            src.replace(pos, needle.size(), value);
    };
    replace_first("@SEQUENCE@", seq_str);
    replace_first("@ACCESSOR@", accessor);
    return src;
}
} // namespace

int api_loop_overrun_count_reads_from_core(const std::string &dir)
{
    // Strengthened over V2 — live-read pin via two-invoke sequence
    // across distinct core state (3 overruns then 7).  The second
    // invoke must see LIVE count from RoleHostCore, not a cached
    // build_api-time snapshot.  Script owns its expected sequence;
    // C++ drives core-state progression.
    return produce_worker_with_script(
        dir, "python_engine::api_loop_overrun_count_reads_from_core",
        build_live_read_script("loop_overrun_count", {3, 7}),
        [](PythonEngine &engine, RoleHostCore &core) {
            run_core_live_read_test(
                engine, core, {3, 7},
                [](RoleHostCore &c, size_t i, int64_t target) {
                    // Increment until we reach `target`.  For i==0 this
                    // starts from 0 (3 increments → 3); for i==1 this
                    // continues from 3 (4 increments → 7).
                    const int64_t current =
                        static_cast<int64_t>(c.loop_overrun_count());
                    for (int64_t k = current; k < target; ++k)
                        c.inc_loop_overrun();
                    (void)i;
                });
        });
}

int api_last_cycle_work_us_reads_from_core(const std::string &dir)
{
    // Strengthened over V2 — same live-read pin.  V2 set 12345 once
    // and verified once; this tests TWO distinct values in sequence,
    // proving set_last_cycle_work_us propagates per-invoke.
    return produce_worker_with_script(
        dir, "python_engine::api_last_cycle_work_us_reads_from_core",
        build_live_read_script("last_cycle_work_us", {12345, 99999}),
        [](PythonEngine &engine, RoleHostCore &core) {
            run_core_live_read_test(
                engine, core, {12345, 99999},
                [](RoleHostCore &c, size_t, int64_t target) {
                    c.set_last_cycle_work_us(
                        static_cast<uint64_t>(target));
                });
        });
}

// NOTE: V2's Api_CriticalError_DefaultIsFalse was removed — chunk 6's
// api_critical_error_set_and_read_and_stop_reason already exercises the
// same transition.  Chunk 6 calls api.set_critical_error() (pybind11
// wrapper); this V2 test called core.set_critical_error() (direct) but
// both go through the same underlying function — there is no distinct
// code path to cover.  Chunk 6 additionally pins is_shutdown_requested
// (the third side-effect of the bundled operation).

int api_identity_accessors_return_correct_values(const std::string &dir)
{
    // Strengthened over V2 — pins the full identity surface AND
    // verifies logs_dir / run_dir derivation from role_dir (V2 only
    // checked isinstance).
    //
    // role_dir is set to a known path; logs_dir and run_dir MUST
    // derive as role_dir + "/logs" and role_dir + "/run" per
    // role_api_base.cpp:678-686.  Empty-role_dir fall-through case
    // is covered by api_environment_strings_logs_dir_run_dir.
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir, R"PY(
def on_produce(tx, msgs, api):
    # Identity set by make_api (tag="prod").
    assert api.uid() == "prod.testengine.uid00000001", (
        f"uid: {api.uid()!r}")
    assert api.name() == "TestEngine", f"name: {api.name()!r}"
    assert api.channel() == "test.channel", f"channel: {api.channel()!r}"
    assert api.log_level() == "error", f"log_level: {api.log_level()!r}"

    # script_dir / role_dir / logs_dir / run_dir — all strings,
    # role_dir explicitly set below; logs/run derive.
    assert isinstance(api.script_dir(), str), "script_dir must be str"
    assert isinstance(api.role_dir(),   str), "role_dir must be str"
    assert api.role_dir() == "/tmp/role_dir_test", (
        f"role_dir: {api.role_dir()!r}")
    assert api.logs_dir() == "/tmp/role_dir_test/logs", (
        f"logs_dir must derive as role_dir + '/logs', got {api.logs_dir()!r}")
    assert api.run_dir()  == "/tmp/role_dir_test/run", (
        f"run_dir must derive as role_dir + '/run', got {api.run_dir()!r}")
    return False
)PY");

            RoleHostCore core;
            PythonEngine engine;
            std::unique_ptr<RoleAPIBase> api;
            ASSERT_TRUE(setup_role_engine(engine, core, script_dir, api,
                                            RoleKind::Producer));
            // Set role_dir BEFORE invoking (build_api already done by
            // setup_role_engine — role_dir is read live per-call).
            api->set_role_dir("/tmp/role_dir_test");

            std::vector<IncomingMessage> msgs;
            float buf = 0.0f;
            engine.invoke_produce({&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(engine.script_error_count(), 0u);

            engine.finalize();
        },
        "python_engine::api_identity_accessors_return_correct_values",
        Logger::GetLifecycleModule());
}

// NOTE: V2's Api_StopReason_AfterCriticalError was absorbed into
// chunk 6's api_stop_reason_reflects_all_enum_values — that test
// already exercises all four StopReason values (Normal, PeerDead,
// HubDead, CriticalError) via core.set_stop_reason() + api.stop_reason()
// round-trip.  Keeping a chunk-12 duplicate would be pure redundancy.
// The unique V2 intent (bundled-atomicity of api.set_critical_error()
// flipping critical_error + stop_reason + shutdown_requested together)
// is already covered by chunk 6's api_critical_error_set_and_read_and_stop_reason.

int api_environment_strings_logs_dir_run_dir(const std::string &dir)
{
    // Strengthened over V2.  V2 only asserted isinstance(str).  This
    // body pins both derivation paths:
    //   (1) role_dir empty  → logs_dir == "" AND run_dir == ""
    //       (role_api_base.cpp:678-686 early-return branch)
    //   (2) role_dir set    → logs_dir == role_dir + "/logs" AND
    //                         run_dir  == role_dir + "/run"
    // Both branches re-read role_dir per call — no caching.
    //
    // The script computes its expected paths from api.role_dir()
    // itself rather than from a hardcoded constant — so whatever
    // the C++ side sets for role_dir, the expected derivation
    // follows.  No cross-language magic string to keep in sync.
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir, R"PY(
_call = 0

def on_produce(tx, msgs, api):
    global _call
    rd   = api.role_dir()
    logs = api.logs_dir()
    run  = api.run_dir()
    assert isinstance(rd, str) and isinstance(logs, str) and isinstance(run, str), (
        f"role_dir={type(rd)}, logs_dir={type(logs)}, run_dir={type(run)}")
    if rd == "":
        # Empty-role_dir branch — both strings must be empty.
        assert logs == "", f"empty role_dir must yield empty logs_dir, got {logs!r}"
        assert run  == "", f"empty role_dir must yield empty run_dir, got {run!r}"
    else:
        # Set-role_dir branch — derived paths computed from the
        # live role_dir value (no cross-language hardcoded constant).
        assert logs == rd + "/logs", (
            f"logs_dir expected {rd + '/logs'!r}, got {logs!r}")
        assert run  == rd + "/run", (
            f"run_dir expected {rd + '/run'!r}, got {run!r}")
    _call += 1
    return False
)PY");

            RoleHostCore core;
            PythonEngine engine;
            std::unique_ptr<RoleAPIBase> api;
            ASSERT_TRUE(setup_role_engine(engine, core, script_dir, api,
                                            RoleKind::Producer));

            std::vector<IncomingMessage> msgs;
            float buf = 0.0f;

            // Call 0: role_dir unset — empty-branch.
            engine.invoke_produce({&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(engine.script_error_count(), 0u);

            // Call 1: set role_dir to an arbitrary non-empty path;
            // script derives expected logs/run from the live value.
            api->set_role_dir("/var/run/plh-test");
            engine.invoke_produce({&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "after set_role_dir, logs_dir/run_dir must derive "
                   "from role_dir suffixed by /logs and /run";

            // S4 pin: script saw exactly 2 invocations.  A refactor that
            // dropped a call would be caught here even if script_error_count
            // stayed at 0.
            auto r = engine.eval("_call");
            EXPECT_EQ(r.value, 2) << "script must have been invoked twice";

            engine.finalize();
        },
        "python_engine::api_environment_strings_logs_dir_run_dir",
        Logger::GetLifecycleModule());
}

int api_report_metrics_non_dict_arg_is_error(const std::string &dir)
{
    // Strengthened over V2.  V2 used EXPECT_GE(count, 1u) which
    // passes even if the regression produced a cascade of errors.
    // Strict EQ to 1 pins "exactly one error from the type mismatch,
    // no cascading follow-on errors."  Parent test also pins the
    // ERROR log substring at the ExpectWorkerOk level.
    return produce_worker_with_script(
        dir,
        "python_engine::api_report_metrics_non_dict_arg_is_error",
        R"PY(
def on_produce(tx, msgs, api):
    api.report_metrics(42)  # int, not dict — must raise TypeError
    return False
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            std::vector<IncomingMessage> msgs;
            float buf = 0.0f;
            engine.invoke_produce({&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(engine.script_error_count(), 1u)
                << "report_metrics(non-dict) must increment error "
                   "count by EXACTLY 1 — any other value indicates "
                   "a cascade or silent-swallow regression";
        });
}

// ── Custom metrics (chunk 13) ──────────────────────────────────────────────

int api_custom_metrics_report_and_read_in_metrics(const std::string &dir)
{
    // Strengthened over V2.  V2 reported two values and verified via
    // api.metrics() only.  This body adds:
    //   (a) Pre-report state: api.metrics() must NOT contain "custom"
    //       group before any report — proves the group is created
    //       on first report, not always present.
    //   (b) C++-side cross-check: core.custom_metrics_snapshot() must
    //       reflect the same values — pins the round-trip through the
    //       pybind11 → RoleHostCore → snapshot path, not just the
    //       Python-side accessor echoing its own storage.
    //   (c) Cross-invoke persistence: values reported in invoke 1 are
    //       visible in invoke 2 — pins that custom metrics live in
    //       RoleHostCore, not a per-call local.
    // Script owns its phase via a call counter:
    //   call 0 — pre-report: "custom" group absent.
    //   call 1 — report + verify via Python-side api.metrics().
    //   call 2 — cross-invoke persistence: values from call 1 still visible.
    return produce_worker_with_script(
        dir,
        "python_engine::api_custom_metrics_report_and_read_in_metrics",
        R"PY(
_call = 0

def on_produce(tx, msgs, api):
    global _call
    if _call == 0:
        # Pre-report state — "custom" group must NOT be present yet.
        m = api.metrics()
        assert "custom" not in m, (
            f"'custom' must not be present before any report_metric, "
            f"got keys: {list(m.keys())}")
    elif _call == 1:
        # Report + verify via Python-side accessor.
        api.report_metric("latency_ms", 42.5)
        api.report_metric("throughput", 100)    # int → double
        m = api.metrics()
        assert "custom" in m, "custom metrics group must exist after report"
        assert m["custom"]["latency_ms"] == 42.5, (
            f"got {m['custom']['latency_ms']}")
        assert m["custom"]["throughput"] == 100, (
            f"got {m['custom']['throughput']}")
    else:  # call 2
        # Cross-invoke persistence: values from call 1 must still be visible.
        m = api.metrics()
        assert m["custom"]["latency_ms"] == 42.5, (
            f"cross-invoke: latency_ms lost, got "
            f"{m.get('custom', {}).get('latency_ms')}")
        assert m["custom"]["throughput"] == 100, (
            "cross-invoke: throughput lost")
    _call += 1
    return False
)PY",
        [](PythonEngine &engine, RoleHostCore &core) {
            std::vector<IncomingMessage> msgs;
            float buf = 0.0f;

            // Call 0: pre-report.
            engine.invoke_produce({&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "'custom' group must be absent before any report_metric";
            EXPECT_EQ(core.custom_metrics_snapshot().size(), 0u)
                << "core snapshot must also be empty pre-report";

            // Call 1: report two scalars.
            engine.invoke_produce({&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(engine.script_error_count(), 0u);

            // C++-side cross-check — pins the round-trip.
            const auto snap = core.custom_metrics_snapshot();
            EXPECT_EQ(snap.size(), 2u);
            auto it_lat = snap.find("latency_ms");
            ASSERT_NE(it_lat, snap.end()) << "core snapshot missing 'latency_ms'";
            EXPECT_DOUBLE_EQ(it_lat->second, 42.5);
            auto it_thr = snap.find("throughput");
            ASSERT_NE(it_thr, snap.end()) << "core snapshot missing 'throughput'";
            EXPECT_DOUBLE_EQ(it_thr->second, 100.0);

            // Call 2: cross-invoke persistence.
            engine.invoke_produce({&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "custom metrics must persist across invokes — they live "
                   "in RoleHostCore, not in per-call local state";

            // S4 pin: exactly 3 phases reached.  A refactor that
            // dropped an invoke would be caught here.
            auto r = engine.eval("_call");
            EXPECT_EQ(r.value, 3) << "script must have been invoked 3 times";
        });
}

int api_custom_metrics_batch_and_clear(const std::string &dir)
{
    // Strengthened over V2.  V2 reported via report_metrics(dict) +
    // clear_custom_metrics + checked "custom not in m".  Additions:
    //   (a) Script cycle: report → verify present → clear → verify
    //       absent → RE-report → verify present again.  Pins that
    //       clear is an empty-the-map operation, not a lock-out.
    //   (b) C++-side snapshot cross-checked at each step.
    //   (c) Mixed-magnitude values (very small float, large int)
    //       verified round-trip through double storage.
    return produce_worker_with_script(
        dir, "python_engine::api_custom_metrics_batch_and_clear",
        R"PY(
def on_produce(tx, msgs, api):
    # Phase 1: batch report via dict.
    api.report_metrics({
        "a": 1.0,
        "b": 2.5e-6,     # tiny
        "c": 1.0e9,      # large
    })
    m = api.metrics()
    assert m["custom"]["a"] == 1.0
    assert m["custom"]["b"] == 2.5e-6
    assert m["custom"]["c"] == 1.0e9

    # Phase 2: clear.
    api.clear_custom_metrics()
    m2 = api.metrics()
    assert "custom" not in m2, (
        f"'custom' must be absent after clear, got: {list(m2.keys())}")

    # Phase 3: re-report after clear — clear must not disable the facility.
    api.report_metric("revived", 7.0)
    m3 = api.metrics()
    assert "custom" in m3, "'custom' must reappear after post-clear report"
    assert m3["custom"]["revived"] == 7.0
    assert "a" not in m3["custom"], (
        f"old keys must not survive clear, got: {list(m3['custom'].keys())}")
    return False
)PY",
        [](PythonEngine &engine, RoleHostCore &core) {
            std::vector<IncomingMessage> msgs;
            float buf = 0.0f;
            engine.invoke_produce({&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(engine.script_error_count(), 0u);

            // C++-side verification: after all three phases, only the
            // "revived" key survives.
            const auto snap = core.custom_metrics_snapshot();
            EXPECT_EQ(snap.size(), 1u)
                << "core snapshot must have exactly 1 entry (revived) "
                   "after the report → clear → re-report cycle";
            auto it = snap.find("revived");
            ASSERT_NE(it, snap.end()) << "core snapshot missing 'revived'";
            EXPECT_DOUBLE_EQ(it->second, 7.0);
        });
}

int api_custom_metrics_overwrite_same_key(const std::string &dir)
{
    // Strengthened over V2.  V2 reported x=1 then x=2 and verified x==2.
    // This body tests a 4-value chain (1 → 2 → 3 → 999) through
    // int/float mix, proving each call overwrites cleanly.  Also
    // verifies the key count stays at 1 throughout (no duplicate
    // storage on reassign).
    return produce_worker_with_script(
        dir, "python_engine::api_custom_metrics_overwrite_same_key",
        R"PY(
def on_produce(tx, msgs, api):
    api.report_metric("x", 1)         # int
    api.report_metric("x", 2)         # int
    api.report_metric("x", 3.5)       # float (type change; stored as double)
    api.report_metric("x", 999)       # int again
    m = api.metrics()
    assert "custom" in m, "custom group must exist"
    assert m["custom"]["x"] == 999, (
        f"expected x==999 after 4 overwrites, got {m['custom']['x']}")
    assert len(m["custom"]) == 1, (
        f"overwrite must not duplicate keys; got {len(m['custom'])} entries: "
        f"{list(m['custom'].keys())}")
    return False
)PY",
        [](PythonEngine &engine, RoleHostCore &core) {
            std::vector<IncomingMessage> msgs;
            float buf = 0.0f;
            engine.invoke_produce({&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(engine.script_error_count(), 0u);

            // C++-side: snapshot must contain exactly the final value.
            const auto snap = core.custom_metrics_snapshot();
            EXPECT_EQ(snap.size(), 1u)
                << "core snapshot must have exactly 1 entry — 4 reports "
                   "of the same key must overwrite, not accumulate";
            auto it = snap.find("x");
            ASSERT_NE(it, snap.end());
            EXPECT_DOUBLE_EQ(it->second, 999.0)
                << "final value must be 999.0 (last report wins)";
        });
}

int api_custom_metrics_zero_value(const std::string &dir)
{
    // Strengthened over V2.  V2 reported 0.0 and verified present.
    // This body pins three edge-value cases:
    //   (a) 0.0     — positive zero
    //   (b) -0.0    — negative zero.  IEEE 754 makes +0.0 == -0.0, so
    //                 `==` alone doesn't distinguish them — we check
    //                 the sign bit via math.copysign / std::signbit
    //                 to pin that the storage preserves sign-of-zero.
    //   (c) negative finite — pins the storage isn't truncating to
    //                          unsigned somewhere.
    // All three must be storable AND retrievable (presence check
    // must not confuse "zero" with "absent").
    return produce_worker_with_script(
        dir, "python_engine::api_custom_metrics_zero_value",
        R"PY(
import math

def on_produce(tx, msgs, api):
    api.report_metric("pos_zero", 0.0)
    api.report_metric("neg_zero", -0.0)
    api.report_metric("negative", -3.14)

    m = api.metrics()
    assert "custom" in m, "'custom' must exist even with zero-valued entries"
    # Presence (distinct from "absent"): key IS in the dict.
    assert "pos_zero" in m["custom"], "0.0 key must be present, not filtered"
    assert "neg_zero" in m["custom"], "-0.0 key must be present"
    assert "negative" in m["custom"], "negative-value key must be present"

    # Values — equality holds for both zeros by IEEE 754, so these
    # only pin "stored as some zero".
    assert m["custom"]["pos_zero"] == 0.0
    assert m["custom"]["neg_zero"] == 0.0
    assert m["custom"]["negative"] == -3.14

    # Sign-preservation pin: copysign(1.0, x) returns +1.0 for +0.0
    # and -1.0 for -0.0, so this distinguishes the two.  A storage
    # regression that normalised to +0.0 would flip the neg_zero sign
    # check here.
    pos_sign = math.copysign(1.0, m["custom"]["pos_zero"])
    neg_sign = math.copysign(1.0, m["custom"]["neg_zero"])
    assert pos_sign == 1.0,  f"pos_zero sign lost, got copysign result {pos_sign}"
    assert neg_sign == -1.0, (
        f"neg_zero sign lost, got copysign result {neg_sign} — "
        f"storage normalised -0.0 to +0.0")
    return False
)PY",
        [](PythonEngine &engine, RoleHostCore &core) {
            std::vector<IncomingMessage> msgs;
            float buf = 0.0f;
            engine.invoke_produce({&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(engine.script_error_count(), 0u);

            // C++-side: snapshot must contain all 3 entries with
            // sign-of-zero preserved (std::signbit: true for -0.0).
            const auto snap = core.custom_metrics_snapshot();
            EXPECT_EQ(snap.size(), 3u);
            auto it_pos = snap.find("pos_zero");
            ASSERT_NE(it_pos, snap.end()) << "core snapshot missing 'pos_zero'";
            EXPECT_EQ(it_pos->second, 0.0);
            EXPECT_FALSE(std::signbit(it_pos->second))
                << "+0.0 sign bit must be clear";
            auto it_neg = snap.find("neg_zero");
            ASSERT_NE(it_neg, snap.end()) << "core snapshot missing 'neg_zero'";
            EXPECT_EQ(it_neg->second, 0.0);
            EXPECT_TRUE(std::signbit(it_neg->second))
                << "-0.0 sign bit must be set — storage must preserve "
                   "negative-zero bit pattern through the pybind11 → "
                   "core double path";
            auto it_neg_val = snap.find("negative");
            ASSERT_NE(it_neg_val, snap.end()) << "core snapshot missing 'negative'";
            EXPECT_DOUBLE_EQ(it_neg_val->second, -3.14);
        });
}

int api_custom_metrics_report_type_errors(const std::string &dir)
{
    // Error-path coverage for the pybind11-bound `report_metric` /
    // `report_metrics` signatures:
    //   report_metric(const std::string &key, double value)
    //   report_metrics(const std::unordered_map<std::string, double> &kv)
    //
    // Each case below passes a deliberately wrong type to pybind11's
    // argument conversion; pybind11 raises TypeError at conversion
    // time BEFORE entering the C++ function, so RoleHostCore is
    // untouched and script_error_count stays 0 (all TypeErrors
    // are caught by the script's try/except).
    //
    // Validates:
    //   (a) Wrong key type (non-str) for report_metric.
    //   (b) Wrong value type (non-numeric) for report_metric.
    //   (c) Wrong key type in report_metrics dict.
    //   (d) Wrong value type in report_metrics dict.
    //   (e) Side-effect containment — no partial update: after a
    //       rejected report_metrics, the core's snapshot must be
    //       unchanged (pybind11 converts the whole map or nothing).
    return produce_worker_with_script(
        dir, "python_engine::api_custom_metrics_report_type_errors",
        R"PY(
def on_produce(tx, msgs, api):
    # (a) report_metric(int_key, valid_val) — key must be str.
    try:
        api.report_metric(42, 1.0)
        assert False, "int key must raise TypeError"
    except TypeError:
        pass

    # (b) report_metric(valid_key, str_val) — value must be numeric.
    try:
        api.report_metric("k", "not_numeric")
        assert False, "str value must raise TypeError"
    except TypeError:
        pass

    # (c) report_metrics({int_key: valid_val}) — dict keys must be str.
    try:
        api.report_metrics({42: 1.0})
        assert False, "int key in report_metrics dict must raise TypeError"
    except TypeError:
        pass

    # (d) report_metrics({valid_key: str_val}) — dict values must be numeric.
    try:
        api.report_metrics({"k": "not_numeric"})
        assert False, "str value in report_metrics dict must raise TypeError"
    except TypeError:
        pass

    # (e) After all rejected calls, write ONE valid entry so the C++
    # side can pin "only the valid entry made it through — nothing
    # leaked from the failed attempts."
    api.report_metric("sentinel", 42.0)
    return False
)PY",
        [](PythonEngine &engine, RoleHostCore &core) {
            std::vector<IncomingMessage> msgs;
            float buf = 0.0f;
            engine.invoke_produce({&buf, sizeof(buf)}, msgs);
            // All TypeErrors were caught by the script — no leak into
            // script_error_count.
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "TypeError from pybind11 arg conversion must be "
                   "catchable in-script without polluting error count";

            // Side-effect containment: only the final valid call
            // produced a metric.  A pybind11 bug that partially
            // applied a rejected dict would show > 1 entry here.
            const auto snap = core.custom_metrics_snapshot();
            EXPECT_EQ(snap.size(), 1u)
                << "exactly 1 entry expected (the 'sentinel' from the "
                   "final valid call) — any more means a rejected "
                   "report_metrics partially updated the core";
            auto it = snap.find("sentinel");
            ASSERT_NE(it, snap.end()) << "sentinel entry missing";
            EXPECT_DOUBLE_EQ(it->second, 42.0);
        });
}

// ── Queue state / spinlock / channels / open_inbox / numpy (chunk 14) ─────

int api_producer_queue_state_without_queue(const std::string &dir)
{
    // Strengthened over V2 with type-assertions (int + str) to catch
    // a pybind11 binding that returned None or the wrong type.
    //
    // Note: last_seq() is NOT bound on ProducerAPI — producers write
    // sequences, they do not track a read cursor.  The method lives
    // on ConsumerAPI (consumer test) and ProcessorAPI (processor
    // dual-defaults test) only.
    return produce_worker_with_script(
        dir, "python_engine::api_producer_queue_state_without_queue",
        R"PY(
def on_produce(tx, msgs, api):
    cap = api.out_capacity()
    pol = api.out_policy()
    assert isinstance(cap, int), f"out_capacity must be int, got {type(cap)}"
    assert isinstance(pol, str), f"out_policy must be str, got {type(pol)}"
    assert cap == 0, f"expected out_capacity==0, got {cap}"
    assert pol == "", f"expected out_policy=='', got {pol!r}"
    return False
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            std::vector<IncomingMessage> msgs;
            float buf = 0.0f;
            engine.invoke_produce({&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

int api_consumer_queue_state_without_queue(const std::string &dir)
{
    // V2 parity with same type-assertion strengthening as the
    // producer test.  Consumer surface: in_capacity, in_policy,
    // last_seq.
    return consume_worker_with_script(
        dir, "python_engine::api_consumer_queue_state_without_queue",
        R"PY(
def on_consume(rx, msgs, api):
    cap = api.in_capacity()
    pol = api.in_policy()
    seq = api.last_seq()
    assert isinstance(cap, int),   f"in_capacity must be int, got {type(cap)}"
    assert isinstance(pol, str),   f"in_policy must be str, got {type(pol)}"
    assert isinstance(seq, int),   f"last_seq must be int, got {type(seq)}"
    assert cap == 0, f"in_capacity: {cap}"
    assert pol == "", f"in_policy: {pol!r}"
    assert seq == 0, f"last_seq: {seq}"
    return True
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            std::vector<IncomingMessage> msgs;
            float buf = 1.0f;
            engine.invoke_consume(InvokeRx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

int api_processor_queue_state_dual_defaults(const std::string &dir)
{
    // V2 parity: processor exposes BOTH in_ and out_ queue accessors.
    // All 5 values (in_capacity, in_policy, out_capacity, out_policy,
    // last_seq) must default to zero/empty when no queue is wired.
    return process_worker_with_script(
        dir, "python_engine::api_processor_queue_state_dual_defaults",
        R"PY(
def on_process(rx, tx, msgs, api):
    assert api.in_capacity()  == 0, f"in_capacity={api.in_capacity()}"
    assert api.in_policy()    == "", f"in_policy={api.in_policy()!r}"
    assert api.out_capacity() == 0, f"out_capacity={api.out_capacity()}"
    assert api.out_policy()   == "", f"out_policy={api.out_policy()!r}"
    assert api.last_seq()     == 0, f"last_seq={api.last_seq()}"
    return False
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            std::vector<IncomingMessage> msgs;
            auto result = engine.invoke_process(
                InvokeRx{nullptr, 0}, InvokeTx{nullptr, 0}, msgs);
            EXPECT_EQ(result, InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

int api_processor_channels_in_out(const std::string &dir)
{
    // V2 parity + type-assertions (both channels must be str).
    //
    // Note: ProcessorAPI does NOT expose a single-direction `channel()`
    // method — only `in_channel()` + `out_channel()`.  The unified
    // `channel()` accessor is specific to producer/consumer roles
    // (single-direction).  A regression that added `channel()` to
    // processor would be a semantic error the hasattr check below
    // catches.
    //
    // Inline setup (run_gtest_worker) needed because make_api does
    // not set out_channel by default, and the test-channel defaults
    // do not match the values the script asserts against.
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir, R"PY(
def on_process(rx, tx, msgs, api):
    ic = api.in_channel()
    oc = api.out_channel()
    assert isinstance(ic, str), f"in_channel must be str, got {type(ic)}"
    assert isinstance(oc, str), f"out_channel must be str, got {type(oc)}"
    assert ic == "sensor.input",  f"in_channel mismatch: {ic!r}"
    assert oc == "sensor.output", f"out_channel mismatch: {oc!r}"
    # Processor is dual-channel — the single-direction channel()
    # accessor from ProducerAPI/ConsumerAPI must NOT be exposed.
    assert not hasattr(api, "channel"), (
        "ProcessorAPI must NOT expose the unified channel() method "
        "(use in_channel()/out_channel() instead) — "
        f"found attrs: {dir(api)}")
    return False
)PY");

            RoleHostCore core;
            PythonEngine engine;
            engine.set_python_venv("");
            ASSERT_TRUE(engine.initialize("test", &core));
            ASSERT_TRUE(engine.load_script(
                script_dir / "script" / "python",
                "__init__.py", "on_process"));

            auto spec = simple_schema();
            ASSERT_TRUE(engine.register_slot_type(spec, "InSlotFrame",  "aligned"));
            ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame", "aligned"));

            auto api = make_api(core, "proc");
            api->set_channel("sensor.input");
            api->set_out_channel("sensor.output");
            ASSERT_TRUE(engine.build_api(*api));

            float in_data  = 1.0f;
            float out_data = 0.0f;
            std::vector<IncomingMessage> msgs;
            auto result = engine.invoke_process(
                InvokeRx{&in_data,  sizeof(in_data)},
                InvokeTx{&out_data, sizeof(out_data)}, msgs);
            EXPECT_EQ(result, InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 0u);

            engine.finalize();
        },
        "python_engine::api_processor_channels_in_out",
        Logger::GetLifecycleModule());
}

int api_open_inbox_without_broker(const std::string &dir)
{
    // Strengthened over V2.  V2 tested one uid ("some-uid").  This
    // body tests three arg shapes (normal uid, empty string, unicode)
    // — all must return None without raising.  A regression where
    // empty-string uid crashed or threw would surface here instead
    // of silently later.
    return produce_worker_with_script(
        dir, "python_engine::api_open_inbox_without_broker",
        R"PY(
def on_produce(tx, msgs, api):
    for uid in ("prod.someone.uid12345678", "", "PROD-δΣ-99999999"):
        r = api.open_inbox(uid)
        assert r is None, (
            f"api.open_inbox({uid!r}) without broker expected None, got {r!r}")
    return False
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            std::vector<IncomingMessage> msgs;
            float buf = 0.0f;
            auto result = engine.invoke_produce({&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(result, InvokeResult::Discard);
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "api.open_inbox without broker must return None for "
                   "any uid shape (normal, empty, unicode) — not raise";
        });
}

int api_spinlock_count_without_shm(const std::string &dir)
{
    // V2 parity.  Without SHM wired, spinlock_count() must be 0.
    return produce_worker_with_script(
        dir, "python_engine::api_spinlock_count_without_shm",
        R"PY(
def on_produce(tx, msgs, api):
    c = api.spinlock_count()
    assert isinstance(c, int), f"spinlock_count must be int, got {type(c)}"
    assert c == 0, f"spinlock_count: {c}"
    return False
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            std::vector<IncomingMessage> msgs;
            float buf = 0.0f;
            engine.invoke_produce({&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

int api_spinlock_without_shm_is_error(const std::string &dir)
{
    // Strengthened over V2.  V2 caught a bare ValueError.  This body
    // also pins that the exception message mentions "spinlock" so a
    // regression that raised ValueError from some other path (e.g.,
    // bad type conversion) doesn't silently satisfy the except clause.
    return produce_worker_with_script(
        dir, "python_engine::api_spinlock_without_shm_is_error",
        R"PY(
def on_produce(tx, msgs, api):
    try:
        api.spinlock(0)
        assert False, "spinlock(0) should raise without SHM wired"
    except ValueError as e:
        # The ValueError text is produced by the pybind11 binding in
        # producer_api.cpp and is expected to contain the literal
        # "spinlock".  Case-sensitive check ensures the pin is tight.
        assert "spinlock" in str(e), (
            f"ValueError raised but message doesn't mention 'spinlock': {e}")
    return False
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            std::vector<IncomingMessage> msgs;
            float buf = 0.0f;
            engine.invoke_produce({&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "try/except in script must have caught the ValueError "
                   "cleanly — the assertion inside the except clause pins "
                   "the error came from the spinlock path";
        });
}

int api_as_numpy_array_field(const std::string &dir)
{
    // Strengthened over V2 — adds C++-side buffer-layout verification:
    // header @ 0 (float32, 4 bytes), values[4] @ 4-20 (float32 * 4,
    // 16 bytes, aligned packing).  Script writes values[:] via
    // numpy; C++ reads each element from its expected offset.  A
    // regression in as_numpy's buffer-view (wrong dtype, wrong
    // stride, wrong offset) would land writes in the wrong bytes
    // and the C++ reads would diverge.
    //
    // Depends on numpy being importable in the staged Python
    // environment — GTEST_SKIP if not.
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir, R"PY(
def on_produce(tx, msgs, api):
    try:
        import numpy as np
    except ImportError:
        return False   # signal "skip" via Discard

    arr = api.as_numpy(tx.slot.values)
    assert isinstance(arr, np.ndarray), f"expected ndarray, got {type(arr)}"
    assert arr.dtype == np.float32, f"expected float32, got {arr.dtype}"
    assert len(arr) == 4, f"expected length 4, got {len(arr)}"
    # Zero-copy check: arr.data.obj should reference the slot view.
    # (as_numpy takes a ctypes array; the ndarray shares its memory.)
    arr[:] = [1.0, 2.0, 3.0, 4.0]
    # Also pin scalar field write through tx.slot directly — proves
    # the two write paths (numpy slice + direct ctypes) coexist.
    tx.slot.header = 99.0
    return True
)PY");

            // Schema: float32 header + float32[4] values (aligned: 20 bytes).
            SchemaSpec spec;
            spec.has_schema = true;
            spec.fields.push_back({"header", "float32", 1, 0});
            spec.fields.push_back({"values", "float32", 4, 0});

            RoleHostCore core;
            PythonEngine engine;
            engine.set_python_venv("");
            ASSERT_TRUE(engine.initialize("test", &core));
            ASSERT_TRUE(engine.load_script(
                script_dir / "script" / "python",
                "__init__.py", "on_produce"));
            ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame", "aligned"));

            auto api = make_api(core);
            ASSERT_TRUE(engine.build_api(*api));

            // 20-byte buffer: header (4) + values[4] (16) = 20.
            std::vector<std::byte> slot(20, std::byte{0});
            std::vector<IncomingMessage> msgs;
            auto result = engine.invoke_produce(
                InvokeTx{slot.data(), slot.size()}, msgs);
            EXPECT_EQ(engine.script_error_count(), 0u);

            if (result == InvokeResult::Discard)
            {
                GTEST_SKIP() << "numpy not available in staged Python";
            }

            EXPECT_EQ(result, InvokeResult::Commit);

            // C++-side offset verification — each float at expected
            // stride confirms as_numpy view hit the right bytes.
            float header = 0.0f;
            float values[4] = {};
            std::memcpy(&header,  slot.data() + 0,  sizeof(header));
            std::memcpy(values,   slot.data() + 4,  sizeof(values));
            EXPECT_FLOAT_EQ(header,    99.0f) << "tx.slot.header direct write";
            EXPECT_FLOAT_EQ(values[0],  1.0f) << "numpy slice values[0]";
            EXPECT_FLOAT_EQ(values[1],  2.0f);
            EXPECT_FLOAT_EQ(values[2],  3.0f);
            EXPECT_FLOAT_EQ(values[3],  4.0f) << "numpy slice values[3]";

            engine.finalize();
        },
        "python_engine::api_as_numpy_array_field",
        Logger::GetLifecycleModule());
}

int api_as_numpy_non_array_field_throws(const std::string &dir)
{
    // Strengthened over V2.  V2 used tx.slot.value as both the
    // target of the bad as_numpy call AND the signaling channel
    // (set to -1 for "numpy unavailable", 1 for "test passed").
    // That conflates two concerns.
    //
    // This body uses api.report_metric for signaling (unconditional
    // after the test), keeping tx.slot free for the actual test.
    // C++ side checks custom_metrics_snapshot() to determine
    // pass/skip/fail, and never has to poke the slot buffer.
    return produce_worker_with_script(
        dir, "python_engine::api_as_numpy_non_array_field_throws",
        R"PY(
def on_produce(tx, msgs, api):
    try:
        import numpy
    except ImportError:
        api.report_metric("numpy_available", 0)
        return False
    api.report_metric("numpy_available", 1)

    # Scalar field — as_numpy must raise TypeError.
    try:
        api.as_numpy(tx.slot.value)
        assert False, "as_numpy on scalar must raise TypeError"
    except TypeError as e:
        assert "ctypes array" in str(e), (
            f"TypeError message must mention 'ctypes array', got: {e}")
        api.report_metric("test_passed", 1)
    return False
)PY",
        [](PythonEngine &engine, RoleHostCore &core) {
            std::vector<IncomingMessage> msgs;
            float buf = 0.0f;
            engine.invoke_produce({&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(engine.script_error_count(), 0u);

            const auto snap = core.custom_metrics_snapshot();
            auto it_avail = snap.find("numpy_available");
            ASSERT_NE(it_avail, snap.end())
                << "numpy_available flag must be set by the script";
            if (it_avail->second == 0.0) {
                GTEST_SKIP() << "numpy not available in staged Python";
            }
            auto it_passed = snap.find("test_passed");
            ASSERT_NE(it_passed, snap.end())
                << "test_passed not set — the as_numpy(scalar) branch "
                   "did not reach the TypeError handler, meaning either "
                   "the call did not raise OR it raised the wrong type";
            EXPECT_DOUBLE_EQ(it_passed->second, 1.0);
        });
}

// ── FullLifecycle + FullStartup (chunk 15) ─────────────────────────────────
//
// These tests exercise the full engine lifecycle path (EngineModuleParams
// + engine_lifecycle_startup / engine_lifecycle_shutdown), which is the
// production setup path used by role hosts.  setup_role_engine used by
// earlier chunks takes a shortcut; this chunk pins the real thing.

int full_lifecycle_verifies_callback_execution(const std::string &dir)
{
    // Strengthened over V2.  V2 verified init_ran + stop_ran flags.
    // This body additionally:
    //   (a) Records the callback ORDER via a module-level list —
    //       proves on_init ran BEFORE on_stop (not just that both ran).
    //   (b) Verifies on_init received a non-None api object (api.uid()
    //       returns a non-empty str from within on_init, proving the
    //       api is wired at that entry point).
    //   (c) Asserts script_error_count == 0 between each callback —
    //       isolates which callback would be responsible if a regression
    //       raised.
    return produce_worker_with_script(
        dir, "python_engine::full_lifecycle_verifies_callback_execution",
        R"PY(
_api_ref   = None
_callback_log = []

def on_init(api):
    global _api_ref
    _api_ref = api
    assert api is not None, "on_init: api arg must not be None"
    assert isinstance(api.uid(), str) and len(api.uid()) > 0, (
        f"on_init: api.uid() must be non-empty str, got {api.uid()!r}")
    api.shared_data["init_ran"] = True
    _callback_log.append("init")

def on_produce(tx, msgs, api):
    return False

def on_stop(api):
    assert api is not None, "on_stop: api arg must not be None"
    api.shared_data["stop_ran"] = True
    _callback_log.append("stop")

def get_callback_log():
    return list(_callback_log)

def get_init_ran():
    return _api_ref.shared_data.get("init_ran", False)

def get_stop_ran():
    return _api_ref.shared_data.get("stop_ran", False)
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            using pylabhub::scripting::InvokeStatus;

            engine.invoke_on_init();
            ASSERT_EQ(engine.script_error_count(), 0u) << "on_init failed";

            auto r1 = engine.eval("get_init_ran()");
            EXPECT_EQ(r1.status, InvokeStatus::Ok);
            EXPECT_EQ(r1.value, true) << "on_init should have set init_ran=True";

            engine.invoke_on_stop();
            ASSERT_EQ(engine.script_error_count(), 0u) << "on_stop failed";

            auto r2 = engine.eval("get_stop_ran()");
            EXPECT_EQ(r2.status, InvokeStatus::Ok);
            EXPECT_EQ(r2.value, true) << "on_stop should have set stop_ran=True";

            // Callback-order pin: ['init', 'stop'] exactly, in this order.
            // A regression that invoked on_stop before on_init would fail
            // here (or would have failed earlier on the init_ran flag
            // being False when on_stop ran).
            auto r3 = engine.eval("get_callback_log()");
            EXPECT_EQ(r3.status, InvokeStatus::Ok);
            ASSERT_TRUE(r3.value.is_array());
            ASSERT_EQ(r3.value.size(), 2u);
            EXPECT_EQ(r3.value[0], "init");
            EXPECT_EQ(r3.value[1], "stop")
                << "callback order regression: expected [init, stop], "
                   "got " << r3.value;
        });
}

int full_startup_producer_slot_only(const std::string &dir)
{
    // Strengthened over V2.  V2 verified OutSlotFrame + SlotFrame
    // alias both had sizeof > 0.  This body additionally pins that
    // the SlotFrame alias resolves to the SAME size as OutSlotFrame
    // (pybind11 alias binding, not a separate independent type).
    // Also pins shutdown idempotency via two consecutive shutdowns
    // with no assertion failure.
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir,
                "def on_produce(tx, msgs, api):\n"
                "    tx.slot.value = 77.0\n"
                "    return True\n");

            PythonEngine engine;
            RoleHostCore core;
            auto api = make_api(core, "prod");

            pylabhub::scripting::EngineModuleParams params;
            fill_base_params(params, engine, api.get(),
                             script_dir, "prod", "on_produce");
            params.out_slot_spec = simple_schema();
            params.out_packing   = "aligned";

            engine.set_python_venv("");
            ASSERT_NO_THROW(
                pylabhub::scripting::engine_lifecycle_startup(nullptr, &params));

            const size_t expected =
                pylabhub::hub::compute_schema_size(params.out_slot_spec,
                                                   params.out_packing);
            EXPECT_EQ(engine.type_sizeof("OutSlotFrame"), expected);
            EXPECT_EQ(engine.type_sizeof("SlotFrame"), expected)
                << "SlotFrame alias must resolve to the same size as "
                   "OutSlotFrame on a single-direction producer — not "
                   "a separate independent type";

            float buf = 0.0f;
            std::vector<IncomingMessage> msgs;
            auto result = engine.invoke_produce(
                InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(result, InvokeResult::Commit);
            EXPECT_FLOAT_EQ(buf, 77.0f);

            // Idempotent shutdown.
            pylabhub::scripting::engine_lifecycle_shutdown(nullptr, &params);
            pylabhub::scripting::engine_lifecycle_shutdown(nullptr, &params);
        },
        "python_engine::full_startup_producer_slot_only",
        Logger::GetLifecycleModule());
}

int full_startup_producer_slot_and_flexzone(const std::string &dir)
{
    // Strengthened over V2 — adds engine-type-size vs compute_schema_size
    // cross-check for FlexFrame alias (V2 only checked > 0).
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir,
                "def on_produce(tx, msgs, api):\n"
                "    tx.slot.value = 10.0\n"
                "    return True\n");

            PythonEngine engine;
            RoleHostCore core;
            auto api = make_api(core, "prod");

            pylabhub::scripting::EngineModuleParams params;
            fill_base_params(params, engine, api.get(),
                             script_dir, "prod", "on_produce");
            params.out_slot_spec = simple_schema();
            params.out_fz_spec   = simple_schema();
            params.out_packing   = "aligned";

            // Role hosts set flexzone spec on core before engine startup
            // (size is page-aligned physical).
            core.set_out_fz_spec(
                params.out_fz_spec,
                pylabhub::hub::align_to_physical_page(
                    pylabhub::hub::compute_schema_size(
                        params.out_fz_spec, params.out_packing)));

            engine.set_python_venv("");
            ASSERT_NO_THROW(
                pylabhub::scripting::engine_lifecycle_startup(nullptr, &params));

            const size_t logical_fz = pylabhub::hub::compute_schema_size(
                params.out_fz_spec, params.out_packing);
            EXPECT_GT(engine.type_sizeof("OutSlotFrame"), 0u);
            EXPECT_EQ(engine.type_sizeof("OutFlexFrame"), logical_fz);
            EXPECT_EQ(engine.type_sizeof("FlexFrame"), logical_fz)
                << "FlexFrame alias must resolve to the same logical "
                   "size as OutFlexFrame on a producer";
            EXPECT_TRUE(core.has_tx_fz());
            EXPECT_GT(core.out_schema_fz_size(), 0u);
            EXPECT_GE(core.out_schema_fz_size(), logical_fz)
                << "core.out_schema_fz_size() is PAGE-ALIGNED physical; "
                   "must be >= logical size";

            float slot_buf = 0.0f;
            std::vector<IncomingMessage> msgs;
            auto result = engine.invoke_produce(
                InvokeTx{&slot_buf, sizeof(slot_buf)}, msgs);
            EXPECT_EQ(result, InvokeResult::Commit);
            EXPECT_FLOAT_EQ(slot_buf, 10.0f);

            pylabhub::scripting::engine_lifecycle_shutdown(nullptr, &params);
            pylabhub::scripting::engine_lifecycle_shutdown(nullptr, &params);
        },
        "python_engine::full_startup_producer_slot_and_flexzone",
        Logger::GetLifecycleModule());
}

int full_startup_consumer(const std::string &dir)
{
    // Strengthened over V2 twice:
    //   (a) SlotFrame alias size cross-check (must match InSlotFrame
    //       on a consumer) — V2 only checked > 0.
    //   (b) Actual value round-trip.  V2 only asserted
    //       `rx.slot is not None`, which passes even if rx.slot
    //       pointed at a stale or zeroed buffer.  This body writes
    //       a sentinel on the C++ side and asserts the script reads
    //       it back through rx.slot.value.  Pins that the read-side
    //       ctypes view actually maps onto the caller's buffer
    //       (from_buffer view, not from_buffer_copy or a stale
    //       pointer).
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir,
                "def on_consume(rx, msgs, api):\n"
                "    assert rx.slot is not None, 'expected slot'\n"
                "    # Round-trip pin: C++ pre-filled 42.0; script\n"
                "    # must see that exact value through rx.slot.\n"
                "    assert abs(rx.slot.value - 42.0) < 1e-6, (\n"
                "        f'rx.slot.value expected 42.0, got {rx.slot.value}')\n"
                "    return True\n");

            PythonEngine engine;
            RoleHostCore core;
            auto api = make_api(core, "cons");

            pylabhub::scripting::EngineModuleParams params;
            fill_base_params(params, engine, api.get(),
                             script_dir, "cons", "on_consume");
            params.in_slot_spec = simple_schema();
            params.in_packing   = "aligned";

            engine.set_python_venv("");
            ASSERT_NO_THROW(
                pylabhub::scripting::engine_lifecycle_startup(nullptr, &params));

            const size_t expected = pylabhub::hub::compute_schema_size(
                params.in_slot_spec, params.in_packing);
            EXPECT_EQ(engine.type_sizeof("InSlotFrame"), expected);
            EXPECT_EQ(engine.type_sizeof("SlotFrame"), expected)
                << "SlotFrame alias must resolve to InSlotFrame on a "
                   "single-direction consumer";

            // Sentinel value the script reads back through rx.slot.
            float data = 42.0f;
            std::vector<IncomingMessage> msgs;
            auto result = engine.invoke_consume(
                InvokeRx{&data, sizeof(data)}, msgs);
            EXPECT_EQ(result, InvokeResult::Commit);
            EXPECT_EQ(engine.script_error_count(), 0u)
                << "script must have read 42.0 back through rx.slot.value "
                   "— pins the read-side ctypes view maps onto the "
                   "caller's buffer";

            pylabhub::scripting::engine_lifecycle_shutdown(nullptr, &params);
        },
        "python_engine::full_startup_consumer",
        Logger::GetLifecycleModule());
}

int full_startup_processor(const std::string &dir)
{
    // Strengthened over V2 — adds data-transform round-trip (2x doubling)
    // read-back check AND verifies In/OutSlotFrame sizes match
    // compute_schema_size exactly (V2 only checked > 0).  Keeps V2's
    // key pin: SlotFrame is NOT an alias on processor (dual-slot role
    // has distinct In/OutSlotFrame, disambiguated by direction).
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir,
                "def on_process(rx, tx, msgs, api):\n"
                "    if rx.slot is not None and tx.slot is not None:\n"
                "        tx.slot.value = rx.slot.value * 2.0\n"
                "    return True\n");

            PythonEngine engine;
            RoleHostCore core;
            auto api = make_api(core, "proc");

            pylabhub::scripting::EngineModuleParams params;
            fill_base_params(params, engine, api.get(),
                             script_dir, "proc", "on_process");
            params.in_slot_spec  = simple_schema();
            params.out_slot_spec = simple_schema();
            params.in_packing    = "aligned";
            params.out_packing   = "aligned";

            engine.set_python_venv("");
            ASSERT_NO_THROW(
                pylabhub::scripting::engine_lifecycle_startup(nullptr, &params));

            const size_t expected = pylabhub::hub::compute_schema_size(
                params.in_slot_spec, params.in_packing);
            EXPECT_EQ(engine.type_sizeof("InSlotFrame"),  expected);
            EXPECT_EQ(engine.type_sizeof("OutSlotFrame"), expected);
            EXPECT_EQ(engine.type_sizeof("SlotFrame"), 0u)
                << "Processor has NO SlotFrame alias — dual-slot roles "
                   "use in/out-qualified names only (avoids ambiguity "
                   "when both slots are active)";

            float in_data = 5.0f;
            float out_data = 0.0f;
            std::vector<IncomingMessage> msgs;
            auto result = engine.invoke_process(
                InvokeRx{&in_data,  sizeof(in_data)},
                InvokeTx{&out_data, sizeof(out_data)}, msgs);
            EXPECT_EQ(result, InvokeResult::Commit);
            EXPECT_FLOAT_EQ(out_data, 10.0f)
                << "process 2x transformation: in=5 → out=10";
            EXPECT_EQ(engine.script_error_count(), 0u);

            pylabhub::scripting::engine_lifecycle_shutdown(nullptr, &params);
        },
        "python_engine::full_startup_processor",
        Logger::GetLifecycleModule());
}

// ── SlotLogicalSize + FlexzoneLogicalSize + multifield + band (chunk 16) ──

namespace
{
// Shared helper: runs a FullStartup-style test that exposes
// `api.slot_logical_size()` to Python via set_out_slot_spec on core.
// Returns after assertions; caller handles the worker harness.
template <typename Body>
void run_slot_logical_size_case(
    const fs::path &script_dir, const char *script,
    SchemaSpec spec, const std::string &packing, Body &&body)
{
    write_script(script_dir, script);

    PythonEngine engine;
    RoleHostCore core;
    auto api = make_api(core, "prod");

    spec.packing = packing;
    const size_t logical =
        pylabhub::hub::compute_schema_size(spec, packing);
    core.set_out_slot_spec(SchemaSpec{spec}, logical);

    pylabhub::scripting::EngineModuleParams params;
    fill_base_params(params, engine, api.get(),
                     script_dir, "prod", "on_produce");
    params.out_slot_spec     = spec;
    params.out_packing       = packing;

    engine.set_python_venv("");
    ASSERT_NO_THROW(
        pylabhub::scripting::engine_lifecycle_startup(nullptr, &params));

    // Canonical cross-check that applies to every case.
    EXPECT_EQ(engine.type_sizeof("OutSlotFrame"), logical);
    EXPECT_EQ(core.out_slot_logical_size(), logical);

    body(engine, core, logical);

    pylabhub::scripting::engine_lifecycle_shutdown(nullptr, &params);
}
} // namespace

int slot_logical_size_aligned_padding_sensitive(const std::string &dir)
{
    // Strengthened over V2 — adds live-read pin (call twice, value
    // must still be 16 on second call).  Also splits the canonical
    // cross-check (engine.type_sizeof == core.out_slot_logical_size
    // == 16) into the shared helper so all 3 SlotLogicalSize variants
    // enforce it consistently.
    return run_gtest_worker(
        [&]() {
            run_slot_logical_size_case(
                fs::path(dir),
                "def on_produce(tx, msgs, api):\n"
                "    sz = api.slot_logical_size()\n"
                "    assert isinstance(sz, int), f'must be int, got {type(sz)}'\n"
                "    assert sz == 16, f'expected 16, got {sz}'\n"
                "    return False\n",
                padding_schema(), "aligned",
                [](PythonEngine &engine, RoleHostCore &, size_t logical) {
                    EXPECT_EQ(logical, 16u);
                    std::vector<std::byte> buf(logical, std::byte{0});
                    std::vector<IncomingMessage> msgs;
                    // Twice — live-read pin.
                    engine.invoke_produce(InvokeTx{buf.data(), buf.size()}, msgs);
                    EXPECT_EQ(engine.script_error_count(), 0u);
                    engine.invoke_produce(InvokeTx{buf.data(), buf.size()}, msgs);
                    EXPECT_EQ(engine.script_error_count(), 0u);
                });
        },
        "python_engine::slot_logical_size_aligned_padding_sensitive",
        Logger::GetLifecycleModule());
}

int slot_logical_size_packed_no_padding(const std::string &dir)
{
    // padding_schema under "packed" — 8 (float64) + 1 (uint8) + 4
    // (int32, no alignment pad) = 13 bytes.  Compare against the
    // aligned variant (16 bytes) which has a 3-byte interior pad.
    // A regression where the "packed" flag was ignored would return
    // 16, and the in-script assertion would fire.
    return run_gtest_worker(
        [&]() {
            run_slot_logical_size_case(
                fs::path(dir),
                "def on_produce(tx, msgs, api):\n"
                "    sz = api.slot_logical_size()\n"
                "    assert sz == 13, f'expected 13 (packed), got {sz}'\n"
                "    return False\n",
                padding_schema(), "packed",
                [](PythonEngine &engine, RoleHostCore &, size_t logical) {
                    EXPECT_EQ(logical, 13u);
                    std::vector<std::byte> buf(16, std::byte{0});
                    std::vector<IncomingMessage> msgs;
                    engine.invoke_produce(InvokeTx{buf.data(), buf.size()}, msgs);
                    EXPECT_EQ(engine.script_error_count(), 0u);
                });
        },
        "python_engine::slot_logical_size_packed_no_padding",
        Logger::GetLifecycleModule());
}

int slot_logical_size_complex_mixed_aligned(const std::string &dir)
{
    // complex_mixed_schema (aligned) = 56 bytes — exercises multi-
    // boundary padding (float64 + float32[3] + uint16 + bytes[5] +
    // string[16] + 5-byte pad + int64).
    return run_gtest_worker(
        [&]() {
            run_slot_logical_size_case(
                fs::path(dir),
                "def on_produce(tx, msgs, api):\n"
                "    sz = api.slot_logical_size()\n"
                "    assert sz == 56, f'expected 56, got {sz}'\n"
                "    return False\n",
                complex_mixed_schema(), "aligned",
                [](PythonEngine &engine, RoleHostCore &, size_t logical) {
                    EXPECT_EQ(logical, 56u);
                    std::vector<std::byte> buf(logical, std::byte{0});
                    std::vector<IncomingMessage> msgs;
                    engine.invoke_produce(InvokeTx{buf.data(), buf.size()}, msgs);
                    EXPECT_EQ(engine.script_error_count(), 0u);
                });
        },
        "python_engine::slot_logical_size_complex_mixed_aligned",
        Logger::GetLifecycleModule());
}

int flexzone_logical_size_array_fields(const std::string &dir)
{
    // Slot (padding_schema, 16 bytes) + flex (fz_array_schema,
    // 24 bytes) both registered.  Two distinct sizes reachable from
    // api.slot_logical_size() and api.flexzone_logical_size().
    // Python-specific strengthening over V2: ALSO verify the two
    // are NOT equal (pins that each accessor returns its own side's
    // size, not a shared snapshot).
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir,
                "def on_produce(tx, msgs, api):\n"
                "    slot_sz = api.slot_logical_size()\n"
                "    fz_sz   = api.flexzone_logical_size()\n"
                "    assert slot_sz == 16, f'slot: expected 16, got {slot_sz}'\n"
                "    assert fz_sz   == 24, f'fz: expected 24, got {fz_sz}'\n"
                "    assert slot_sz != fz_sz, (\n"
                "        'slot and fz must report distinct sizes; if they '\n"
                "        'match, the two accessors may be reading the '\n"
                "        'same underlying field')\n"
                "    return False\n");

            PythonEngine engine;
            RoleHostCore core;
            auto api = make_api(core, "prod");

            auto slot_spec = padding_schema();   slot_spec.packing = "aligned";
            auto fz_spec   = fz_array_schema();  fz_spec.packing   = "aligned";

            core.set_out_slot_spec(
                SchemaSpec{slot_spec},
                pylabhub::hub::compute_schema_size(slot_spec, "aligned"));
            core.set_out_fz_spec(
                SchemaSpec{fz_spec},
                pylabhub::hub::align_to_physical_page(
                    pylabhub::hub::compute_schema_size(fz_spec, "aligned")));

            pylabhub::scripting::EngineModuleParams params;
            fill_base_params(params, engine, api.get(),
                             script_dir, "prod", "on_produce");
            params.out_slot_spec = slot_spec;
            params.out_fz_spec   = fz_spec;
            params.out_packing   = "aligned";

            engine.set_python_venv("");
            ASSERT_NO_THROW(
                pylabhub::scripting::engine_lifecycle_startup(nullptr, &params));

            EXPECT_EQ(core.out_slot_logical_size(), 16u);
            EXPECT_GE(core.out_schema_fz_size(), 24u)
                << "page-aligned physical size must bound logical 24";
            EXPECT_EQ(engine.type_sizeof("OutSlotFrame"), 16u);
            EXPECT_EQ(engine.type_sizeof("OutFlexFrame"), 24u);

            std::vector<std::byte> slot_buf(16, std::byte{0});
            std::vector<IncomingMessage> msgs;
            engine.invoke_produce(
                InvokeTx{slot_buf.data(), slot_buf.size()}, msgs);
            EXPECT_EQ(engine.script_error_count(), 0u);

            pylabhub::scripting::engine_lifecycle_shutdown(nullptr, &params);
        },
        "python_engine::flexzone_logical_size_array_fields",
        Logger::GetLifecycleModule());
}

namespace
{
// Shared C struct matching padding_schema under "aligned":
//   ts    float64 @ 0  (8 bytes)
//   flag  uint8   @ 8  (1 byte)
//   pad           @ 9  (3 bytes)
//   count int32   @ 12 (4 bytes)
//   total = 16 bytes
struct PaddingSchemaLayout
{
    double  ts;
    uint8_t flag;
    uint8_t pad[3];
    int32_t count;
};
static_assert(sizeof(PaddingSchemaLayout) == 16,
              "PaddingSchemaLayout must match padding_schema aligned size");
} // namespace

int full_startup_producer_multifield(const std::string &dir)
{
    // Full startup + field-level round-trip through padded offsets.
    // Strengthened over V2: also verifies the 3-byte interior pad
    // between flag and count is NOT overwritten (regression: a
    // misaligned ctypes Structure could write into pad bytes,
    // corrupting them from their initial 0 state).
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir,
                "def on_produce(tx, msgs, api):\n"
                "    tx.slot.ts    = 1.23456789\n"
                "    tx.slot.flag  = 0xAB\n"
                "    tx.slot.count = -42\n"
                "    return True\n");

            PythonEngine engine;
            RoleHostCore core;
            auto api = make_api(core, "prod");

            auto spec = padding_schema(); spec.packing = "aligned";
            core.set_out_slot_spec(SchemaSpec{spec},
                pylabhub::hub::compute_schema_size(spec, "aligned"));

            pylabhub::scripting::EngineModuleParams params;
            fill_base_params(params, engine, api.get(),
                             script_dir, "prod", "on_produce");
            params.out_slot_spec = spec;
            params.out_packing   = "aligned";

            engine.set_python_venv("");
            ASSERT_NO_THROW(
                pylabhub::scripting::engine_lifecycle_startup(nullptr, &params));
            EXPECT_EQ(engine.type_sizeof("OutSlotFrame"), 16u);

            // Initialize pad bytes to a sentinel — a misaligned write
            // would overwrite them.
            PaddingSchemaLayout buf{};
            buf.pad[0] = 0xDE; buf.pad[1] = 0xAD; buf.pad[2] = 0xBE;

            std::vector<IncomingMessage> msgs;
            auto result = engine.invoke_produce(
                InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(result, InvokeResult::Commit);
            EXPECT_EQ(engine.script_error_count(), 0u);

            EXPECT_DOUBLE_EQ(buf.ts, 1.23456789);
            EXPECT_EQ(buf.flag, 0xAB);
            EXPECT_EQ(buf.count, -42);
            // Pad bytes must be untouched by the ctypes field writes —
            // pins that offsets 9..11 are NOT part of any addressable
            // field in the Structure.
            EXPECT_EQ(buf.pad[0], 0xDE) << "pad byte 0 corrupted";
            EXPECT_EQ(buf.pad[1], 0xAD) << "pad byte 1 corrupted";
            EXPECT_EQ(buf.pad[2], 0xBE) << "pad byte 2 corrupted";

            pylabhub::scripting::engine_lifecycle_shutdown(nullptr, &params);
        },
        "python_engine::full_startup_producer_multifield",
        Logger::GetLifecycleModule());
}

int full_startup_consumer_multifield(const std::string &dir)
{
    // Consumer side: C struct is pre-filled, script reads fields.
    // V2 parity — already covers the key round-trip (read each of ts,
    // flag, count at its correct offset).
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir,
                "def on_consume(rx, msgs, api):\n"
                "    assert abs(rx.slot.ts - 9.87) < 0.001, f'ts={rx.slot.ts}'\n"
                "    assert rx.slot.flag == 0xCD, f'flag={rx.slot.flag}'\n"
                "    assert rx.slot.count == 100, f'count={rx.slot.count}'\n"
                "    return True\n");

            PythonEngine engine;
            RoleHostCore core;
            auto api = make_api(core, "cons");

            auto spec = padding_schema(); spec.packing = "aligned";
            core.set_in_slot_spec(SchemaSpec{spec},
                pylabhub::hub::compute_schema_size(spec, "aligned"));

            pylabhub::scripting::EngineModuleParams params;
            fill_base_params(params, engine, api.get(),
                             script_dir, "cons", "on_consume");
            params.in_slot_spec = spec;
            params.in_packing   = "aligned";

            engine.set_python_venv("");
            ASSERT_NO_THROW(
                pylabhub::scripting::engine_lifecycle_startup(nullptr, &params));

            PaddingSchemaLayout buf{};
            buf.ts = 9.87; buf.flag = 0xCD; buf.count = 100;

            std::vector<IncomingMessage> msgs;
            auto result = engine.invoke_consume(
                pylabhub::scripting::InvokeRx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(result, InvokeResult::Commit);
            EXPECT_EQ(engine.script_error_count(), 0u);

            pylabhub::scripting::engine_lifecycle_shutdown(nullptr, &params);
        },
        "python_engine::full_startup_consumer_multifield",
        Logger::GetLifecycleModule());
}

int full_startup_processor_multifield(const std::string &dir)
{
    // Processor side: rx → tx field transform.  V2 parity, plus
    // pad-byte pin on tx buffer (same as producer test — misaligned
    // ctypes writes would corrupt pad bytes).
    return run_gtest_worker(
        [&]() {
            const fs::path script_dir(dir);
            write_script(script_dir,
                "def on_process(rx, tx, msgs, api):\n"
                "    tx.slot.ts    = rx.slot.ts\n"
                "    tx.slot.flag  = rx.slot.flag\n"
                "    tx.slot.count = rx.slot.count * 2\n"
                "    return True\n");

            PythonEngine engine;
            RoleHostCore core;
            auto api = make_api(core, "proc");

            auto spec = padding_schema(); spec.packing = "aligned";
            const size_t sz = pylabhub::hub::compute_schema_size(spec, "aligned");
            core.set_in_slot_spec(SchemaSpec{spec},  sz);
            core.set_out_slot_spec(SchemaSpec{spec}, sz);

            pylabhub::scripting::EngineModuleParams params;
            fill_base_params(params, engine, api.get(),
                             script_dir, "proc", "on_process");
            params.in_slot_spec  = spec;
            params.out_slot_spec = spec;
            params.in_packing    = "aligned";
            params.out_packing   = "aligned";

            engine.set_python_venv("");
            ASSERT_NO_THROW(
                pylabhub::scripting::engine_lifecycle_startup(nullptr, &params));

            PaddingSchemaLayout in_buf{};
            PaddingSchemaLayout out_buf{};
            in_buf.ts = 1.23456789; in_buf.flag = 0xAB; in_buf.count = -42;
            out_buf.pad[0] = 0xBE; out_buf.pad[1] = 0xEF; out_buf.pad[2] = 0xED;

            std::vector<IncomingMessage> msgs;
            auto result = engine.invoke_process(
                pylabhub::scripting::InvokeRx{&in_buf, sizeof(in_buf)},
                InvokeTx{&out_buf, sizeof(out_buf)}, msgs);
            EXPECT_EQ(result, InvokeResult::Commit);
            EXPECT_EQ(engine.script_error_count(), 0u);

            EXPECT_DOUBLE_EQ(out_buf.ts, 1.23456789);
            EXPECT_EQ(out_buf.flag, 0xAB);
            EXPECT_EQ(out_buf.count, -84);
            EXPECT_EQ(out_buf.pad[0], 0xBE) << "tx.pad[0] corrupted";
            EXPECT_EQ(out_buf.pad[1], 0xEF) << "tx.pad[1] corrupted";
            EXPECT_EQ(out_buf.pad[2], 0xED) << "tx.pad[2] corrupted";

            pylabhub::scripting::engine_lifecycle_shutdown(nullptr, &params);
        },
        "python_engine::full_startup_processor_multifield",
        Logger::GetLifecycleModule());
}

int api_band_all_methods_graceful_no_broker(const std::string &dir)
{
    // V2 parity: at L2 with no broker wired, all four band_* methods
    // must return gracefully (None / False / no-op) without raising.
    // Strengthening: Python-specific `is None` identity checks (not
    // just equality), plus verify band_broadcast's absence-of-raise
    // path by a direct try/except block — V2 called it unguarded and
    // relied on script_error_count to catch raises; we now explicitly
    // scope a try/except so a regression that started raising would
    // be attributed to the right method.
    return produce_worker_with_script(
        dir, "python_engine::api_band_all_methods_graceful_no_broker",
        R"PY(
def on_produce(tx, msgs, api):
    # (1) band_join — graceful no-op returning None without broker.
    r = api.band_join("#l2_test")
    assert r is None, f"band_join expected None, got {r!r}"

    # (2) band_leave — graceful False (no-op couldn't have succeeded).
    r = api.band_leave("#l2_test")
    assert r is False, f"band_leave expected False, got {r!r}"

    # (3) band_broadcast — must not raise even though there's no one
    # to broadcast to.  Scope an explicit try so a regression's
    # exception is attributed here, not to a later line.
    try:
        api.band_broadcast("#l2_test", {"hello": "world"})
    except Exception as e:
        assert False, (
            f"band_broadcast without broker must not raise, got "
            f"{type(e).__name__}: {e}")

    # (4) band_members — None (no broker to query).
    r = api.band_members("#l2_test")
    assert r is None, f"band_members expected None, got {r!r}"
    return True
)PY",
        [](PythonEngine &engine, RoleHostCore & /*core*/) {
            std::vector<IncomingMessage> msgs;
            float buf = 0.0f;
            auto result = engine.invoke_produce(
                InvokeTx{&buf, sizeof(buf)}, msgs);
            EXPECT_EQ(result, InvokeResult::Commit)
                << "all 4 band_* methods must return gracefully without "
                   "a broker — any raise would make on_produce return "
                   "Error instead of Commit";
            EXPECT_EQ(engine.script_error_count(), 0u);
        });
}

} // namespace python_engine
} // namespace pylabhub::tests::worker

// ── Dispatcher ──────────────────────────────────────────────────────────────

namespace
{

struct PythonEngineWorkerRegistrar
{
    PythonEngineWorkerRegistrar()
    {
        ::register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "python_engine")
                    return -1;
                std::string sc(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::python_engine;

                if (argc <= 2) {
                    fmt::print(stderr,
                               "python_engine.{}: missing <dir> arg\n", sc);
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
                if (sc == "register_slot_type_packed_packing")
                    return register_slot_type_packed_packing(dir);
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
                if (sc == "supports_multi_state_returns_false")
                    return supports_multi_state_returns_false(dir);

                if (sc == "invoke_produce_commit_on_true")
                    return invoke_produce_commit_on_true(dir);
                if (sc == "invoke_produce_discard_on_false")
                    return invoke_produce_discard_on_false(dir);
                if (sc == "invoke_produce_none_return_is_error")
                    return invoke_produce_none_return_is_error(dir);
                if (sc == "invoke_produce_none_slot")
                    return invoke_produce_none_slot(dir);
                if (sc == "invoke_produce_script_error")
                    return invoke_produce_script_error(dir);
                if (sc == "invoke_produce_wrong_return_type_is_error")
                    return invoke_produce_wrong_return_type_is_error(dir);
                if (sc == "invoke_produce_wrong_return_string_is_error")
                    return invoke_produce_wrong_return_string_is_error(dir);
                if (sc == "invoke_produce_discard_on_false_but_python_wrote_slot")
                    return invoke_produce_discard_on_false_but_python_wrote_slot(dir);

                if (sc == "invoke_existing_function_returns_true")
                    return invoke_existing_function_returns_true(dir);
                if (sc == "invoke_non_existent_function_returns_false")
                    return invoke_non_existent_function_returns_false(dir);
                if (sc == "invoke_empty_name_returns_false")
                    return invoke_empty_name_returns_false(dir);
                if (sc == "invoke_script_error_returns_false_and_increments_errors")
                    return invoke_script_error_returns_false_and_increments_errors(dir);
                if (sc == "invoke_from_non_owner_thread_queued")
                    return invoke_from_non_owner_thread_queued(dir);
                if (sc == "invoke_from_non_owner_thread_finalize_unblocks")
                    return invoke_from_non_owner_thread_finalize_unblocks(dir);
                if (sc == "invoke_concurrent_non_owner_threads")
                    return invoke_concurrent_non_owner_threads(dir);
                if (sc == "invoke_after_finalize_returns_false")
                    return invoke_after_finalize_returns_false(dir);
                if (sc == "invoke_with_args_calls_function")
                    return invoke_with_args_calls_function(dir);
                if (sc == "invoke_with_args_from_non_owner_thread")
                    return invoke_with_args_from_non_owner_thread(dir);

                if (sc == "eval_returns_scalar_result")
                    return eval_returns_scalar_result(dir);
                if (sc == "eval_error_returns_empty")
                    return eval_error_returns_empty(dir);
                if (sc == "shared_data_persists_across_callbacks")
                    return shared_data_persists_across_callbacks(dir);

                if (sc == "api_loop_overrun_count_reads_from_core")
                    return api_loop_overrun_count_reads_from_core(dir);
                if (sc == "api_last_cycle_work_us_reads_from_core")
                    return api_last_cycle_work_us_reads_from_core(dir);
                if (sc == "api_identity_accessors_return_correct_values")
                    return api_identity_accessors_return_correct_values(dir);
                if (sc == "api_environment_strings_logs_dir_run_dir")
                    return api_environment_strings_logs_dir_run_dir(dir);
                if (sc == "api_report_metrics_non_dict_arg_is_error")
                    return api_report_metrics_non_dict_arg_is_error(dir);

                if (sc == "api_custom_metrics_report_and_read_in_metrics")
                    return api_custom_metrics_report_and_read_in_metrics(dir);
                if (sc == "api_custom_metrics_batch_and_clear")
                    return api_custom_metrics_batch_and_clear(dir);
                if (sc == "api_custom_metrics_overwrite_same_key")
                    return api_custom_metrics_overwrite_same_key(dir);
                if (sc == "api_custom_metrics_zero_value")
                    return api_custom_metrics_zero_value(dir);
                if (sc == "api_custom_metrics_report_type_errors")
                    return api_custom_metrics_report_type_errors(dir);

                if (sc == "api_producer_queue_state_without_queue")
                    return api_producer_queue_state_without_queue(dir);
                if (sc == "api_consumer_queue_state_without_queue")
                    return api_consumer_queue_state_without_queue(dir);
                if (sc == "api_processor_queue_state_dual_defaults")
                    return api_processor_queue_state_dual_defaults(dir);
                if (sc == "api_processor_channels_in_out")
                    return api_processor_channels_in_out(dir);
                if (sc == "api_open_inbox_without_broker")
                    return api_open_inbox_without_broker(dir);
                if (sc == "api_spinlock_count_without_shm")
                    return api_spinlock_count_without_shm(dir);
                if (sc == "api_spinlock_without_shm_is_error")
                    return api_spinlock_without_shm_is_error(dir);
                if (sc == "api_as_numpy_array_field")
                    return api_as_numpy_array_field(dir);
                if (sc == "api_as_numpy_non_array_field_throws")
                    return api_as_numpy_non_array_field_throws(dir);

                if (sc == "full_lifecycle_verifies_callback_execution")
                    return full_lifecycle_verifies_callback_execution(dir);
                if (sc == "full_startup_producer_slot_only")
                    return full_startup_producer_slot_only(dir);
                if (sc == "full_startup_producer_slot_and_flexzone")
                    return full_startup_producer_slot_and_flexzone(dir);
                if (sc == "full_startup_consumer")
                    return full_startup_consumer(dir);
                if (sc == "full_startup_processor")
                    return full_startup_processor(dir);

                if (sc == "slot_logical_size_aligned_padding_sensitive")
                    return slot_logical_size_aligned_padding_sensitive(dir);
                if (sc == "slot_logical_size_packed_no_padding")
                    return slot_logical_size_packed_no_padding(dir);
                if (sc == "slot_logical_size_complex_mixed_aligned")
                    return slot_logical_size_complex_mixed_aligned(dir);
                if (sc == "flexzone_logical_size_array_fields")
                    return flexzone_logical_size_array_fields(dir);
                if (sc == "full_startup_producer_multifield")
                    return full_startup_producer_multifield(dir);
                if (sc == "full_startup_consumer_multifield")
                    return full_startup_consumer_multifield(dir);
                if (sc == "full_startup_processor_multifield")
                    return full_startup_processor_multifield(dir);
                if (sc == "api_band_all_methods_graceful_no_broker")
                    return api_band_all_methods_graceful_no_broker(dir);

                if (sc == "invoke_consume_receives_slot")
                    return invoke_consume_receives_slot(dir);
                if (sc == "invoke_consume_none_slot")
                    return invoke_consume_none_slot(dir);
                if (sc == "invoke_consume_script_error_detected")
                    return invoke_consume_script_error_detected(dir);
                if (sc == "invoke_consume_rx_slot_is_read_only")
                    return invoke_consume_rx_slot_is_read_only(dir);

                if (sc == "invoke_process_dual_slots")
                    return invoke_process_dual_slots(dir);
                if (sc == "invoke_process_both_slots_none")
                    return invoke_process_both_slots_none(dir);
                if (sc == "invoke_process_rx_present_tx_none")
                    return invoke_process_rx_present_tx_none(dir);
                if (sc == "invoke_process_rx_slot_is_read_only")
                    return invoke_process_rx_slot_is_read_only(dir);

                if (sc == "invoke_produce_receives_messages_event_with_details")
                    return invoke_produce_receives_messages_event_with_details(dir);
                if (sc == "invoke_produce_receives_messages_empty_list")
                    return invoke_produce_receives_messages_empty_list(dir);
                if (sc == "invoke_produce_receives_messages_data_message")
                    return invoke_produce_receives_messages_data_message(dir);
                if (sc == "invoke_consume_receives_messages_data_bare_format")
                    return invoke_consume_receives_messages_data_bare_format(dir);

                if (sc == "api_version_info_returns_json_string")
                    return api_version_info_returns_json_string(dir);
                if (sc == "wrong_role_module_import_raises_error")
                    return wrong_role_module_import_raises_error(dir);
                if (sc == "api_stop_sets_shutdown_requested")
                    return api_stop_sets_shutdown_requested(dir);
                if (sc == "api_critical_error_set_and_read_and_stop_reason")
                    return api_critical_error_set_and_read_and_stop_reason(dir);
                if (sc == "api_stop_reason_reflects_all_enum_values")
                    return api_stop_reason_reflects_all_enum_values(dir);

                if (sc == "metrics_individual_accessors_read_core_counters_live")
                    return metrics_individual_accessors_read_core_counters_live(dir);
                if (sc == "metrics_in_slots_received_works_consumer")
                    return metrics_in_slots_received_works_consumer(dir);
                if (sc == "multiple_errors_count_accumulates")
                    return multiple_errors_count_accumulates(dir);
                if (sc == "stop_on_script_error_sets_shutdown_on_error")
                    return stop_on_script_error_sets_shutdown_on_error(dir);
                if (sc == "metrics_all_loop_fields_anchored_values")
                    return metrics_all_loop_fields_anchored_values(dir);
                if (sc == "metrics_hierarchical_table_producer_full_shape")
                    return metrics_hierarchical_table_producer_full_shape(dir);
                if (sc == "metrics_role_script_error_count_reflects_raised_error")
                    return metrics_role_script_error_count_reflects_raised_error(dir);

                if (sc == "load_script_missing_file")
                    return load_script_missing_file(dir);
                if (sc == "load_script_missing_required_callback")
                    return load_script_missing_required_callback(dir);
                if (sc == "register_slot_type_bad_field_type")
                    return register_slot_type_bad_field_type(dir);
                if (sc == "load_script_syntax_error")
                    return load_script_syntax_error(dir);
                if (sc == "has_callback")
                    return has_callback(dir);
                if (sc == "invoke_on_init_script_error")
                    return invoke_on_init_script_error(dir);
                if (sc == "invoke_on_stop_script_error")
                    return invoke_on_stop_script_error(dir);
                if (sc == "invoke_on_inbox_script_error")
                    return invoke_on_inbox_script_error(dir);

                if (sc == "state_persists_across_calls")
                    return state_persists_across_calls(dir);
                if (sc == "invoke_produce_slot_only_no_flexzone_on_invoke")
                    return invoke_produce_slot_only_no_flexzone_on_invoke(dir);
                if (sc == "invoke_on_inbox_typed_data")
                    return invoke_on_inbox_typed_data(dir);
                if (sc == "type_sizeof_inbox_frame_returns_correct_size")
                    return type_sizeof_inbox_frame_returns_correct_size(dir);
                if (sc == "invoke_on_inbox_missing_type_reports_error")
                    return invoke_on_inbox_missing_type_reports_error(dir);

                fmt::print(stderr,
                           "[python_engine] ERROR: unknown scenario '{}'\n",
                           sc);
                return 1;
            });
    }
};
static PythonEngineWorkerRegistrar g_python_engine_registrar;

} // namespace
