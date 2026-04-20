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
using pylabhub::scripting::IncomingMessage;
using pylabhub::scripting::InvokeResult;
using pylabhub::scripting::InvokeRx;
using pylabhub::scripting::InvokeTx;
using pylabhub::scripting::PythonEngine;
using pylabhub::scripting::RoleAPIBase;
using pylabhub::scripting::RoleHostCore;
using pylabhub::tests::all_types_schema;
using pylabhub::tests::helper::run_gtest_worker;
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
    if (tag == "prod") uid = "PROD-TestEngine-00000001";
    else if (tag == "cons") uid = "CONS-TestEngine-00000001";
    else if (tag == "proc") uid = "PROC-TestEngine-00000001";
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

} // namespace

// ── Lifecycle ───────────────────────────────────────────────────────────────

int full_lifecycle(const std::string &dir)
{
    // Strengthened vs V2 FullLifecycle.  V2 had empty `pass` bodies in
    // on_init/on_stop — a no-op engine implementation would pass.
    //
    // This body reports a metric from each callback via
    // api.report_metric (pybind11-bound at producer_api.cpp:255),
    // then asserts core.custom_metrics_snapshot() contains both
    // keys post-invoke.  Source-traced:
    //   - invoke_on_init  (python_engine.cpp:897-913): acquires
    //     GIL, calls py_on_init_(api_obj_); catches py::error_already_set.
    //   - invoke_on_stop  (python_engine.cpp:919-935): same shape.
    // Both dispatch to the user's Python function only if
    // is_callable(py_on_init_/py_on_stop_).  api_obj_ is set at
    // build_api time (python_engine.cpp:475,485,497) as the
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
    // inline at python_engine.hpp:68.
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
    // register_slot_type (python_engine.cpp:800-807) internally
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
    // compute_schema_size (python_engine.cpp:800-807), so a size-
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
    // script_engine.hpp:388-394, this flag tells the engine's own
    // threading-dispatch path whether to route non-owner-thread
    // invoke() calls directly (true, Lua) or queue them to the
    // owner thread (false, Python).  It is NOT exposed to scripts —
    // the engine handles threading transparently.
    //
    // Python returns false (python_engine.hpp:122): single
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
// Source-traced against python_engine.cpp:957-981 (invoke_produce) and
// python_engine.cpp:1179-1216 (parse_return_value_).  Python-specific
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
// raises a Lua error on write (lua_state.cpp:285-291).  Different
// runtime mechanics, same design contract: loud failure, never
// silent.  Python has one known limitation documented in
// python_helpers.hpp:77 — mutation through array subscript
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
    //   src/scripting/python_engine.cpp:831
    //     register_slot_type("InSlotFrame") →
    //     in_slot_type_ro_ = wrap_readonly_(type)
    //
    //   src/scripting/python_helpers.hpp:96-112 (wrap_as_readonly_ctypes)
    //     Creates a subclass with __setattr__ that raises
    //     AttributeError("read-only slot: field '<name>' cannot be
    //     written (in_slot is a zero-copy SHM view -- use out_slot
    //     to write)").
    //
    //   src/scripting/python_engine.cpp:1000-1002 (invoke_consume path)
    //     rx_ch.slot = make_slot_view_(..., in_slot_type_ro_, …,
    //                                   readonly=true);
    //
    //   src/scripting/python_engine.cpp:1008-1011 (catch)
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
    // python_helpers.hpp:77 is that array-subscript mutation
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
// (python_engine.cpp:1009-1040) builds BOTH an rx and a tx channel
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
    // (python_engine.cpp:1019-1024 builds rx_ch via in_slot_type_ro_
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
// src/scripting/python_engine.cpp:1118 and :1143:
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
    // pass.  The Python contract per python_engine.cpp:1128 is that
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
                   "the documented shape at python_engine.cpp:1135";
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
    // python_engine.cpp:1161.
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
                   "shape at python_engine.cpp:1161";
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
//     (role_api_base.cpp:671)
//   - api.set_critical_error(): three side effects
//     (role_api_base.cpp:672 → core->set_critical_error):
//     (a) critical_error_ = true → api.critical_error() returns True
//     (b) shutdown_requested_ = true
//     (c) stop_reason_ = CriticalError(3) → api.stop_reason()
//         returns "critical_error"
//   - api.stop_reason(): RoleAPIBase::stop_reason() → core->
//     stop_reason_string() → "normal"/"peer_dead"/"hub_dead"/
//     "critical_error" per StopReason enum
//     (role_host_core.hpp:270-279).
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
    // pins all three (role_api_base.cpp:672 → core->
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

                fmt::print(stderr,
                           "[python_engine] ERROR: unknown scenario '{}'\n",
                           sc);
                return 1;
            });
    }
};
static PythonEngineWorkerRegistrar g_python_engine_registrar;

} // namespace
