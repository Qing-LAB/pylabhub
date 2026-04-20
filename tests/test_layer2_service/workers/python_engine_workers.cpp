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

int register_slot_type_packed_vs_aligned(const std::string &dir)
{
    // Strengthened vs V2 RegisterSlotType_PackedPacking.  V2 only
    // registered the schema as "packed" and asserted 5 bytes.  A
    // regression where the packing argument is silently ignored
    // (always using aligned) would slip through if aligned also
    // produced 5 bytes.
    //
    // PYTHON-SPECIFIC DESIGN: PythonEngine's type_sizeof
    // (python_engine.cpp:854-881) only returns a size for the 5
    // known type names (In/OutSlotFrame, In/OutFlexFrame, InboxFrame)
    // — any other name returns 0 because the engine caches types in
    // explicit py::object fields keyed by the known names.  Lua's
    // ffi.typeof is string-keyed so any registered name is
    // retrievable; Python is not.
    //
    // To pin the packing argument's effect under this constraint,
    // we RE-REGISTER the same spec under OutSlotFrame (allowed — it
    // overwrites out_slot_type_ at python_engine.cpp:816) with
    // different packings and capture sizeof between the two
    // registrations.
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

            // bool + int32: aligned = 8 (3 bytes pad), packed = 5.
            SchemaSpec spec;
            spec.has_schema = true;
            spec.fields.push_back({"flag", "bool",  1, 0});
            spec.fields.push_back({"val",  "int32", 1, 0});

            // Phase 1 — register aligned, capture sizeof.
            ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame",
                                                   "aligned"));
            const size_t aligned_sz = engine.type_sizeof("OutSlotFrame");
            EXPECT_EQ(aligned_sz, 8u)
                << "aligned: bool(1) + 3 pad + int32(4) = 8 "
                   "(ctypes natural padding)";

            // Phase 2 — RE-register same spec as packed, capture sizeof.
            ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame",
                                                   "packed"));
            const size_t packed_sz = engine.type_sizeof("OutSlotFrame");
            EXPECT_EQ(packed_sz, 5u)
                << "packed: bool(1) + int32(4) = 5 (ctypes _pack_=1)";

            EXPECT_NE(aligned_sz, packed_sz)
                << "aligned and packed MUST produce different layouts "
                   "for this schema — if equal, the packing arg was "
                   "silently ignored by register_slot_type";

            engine.finalize();
        },
        "python_engine::register_slot_type_packed_vs_aligned",
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
    // type-registration path, leaving the ctypes type-build branches
    // for those types untested at L2.
    //
    // PYTHON-SPECIFIC DESIGN: type_sizeof only works for the 5 known
    // names (see register_slot_type_packed_vs_aligned rationale).
    // We use OutSlotFrame as the anchor and re-register for both
    // packings.  The core claim — "every scalar type in
    // all_types_schema() builds a valid ctypes struct whose size
    // matches compute_schema_size" — is checked for BOTH packings.
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

            // Phase 1 — aligned.
            ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame",
                                                   "aligned"))
                << "every scalar type in all_types_schema must build a "
                   "ctypes struct under aligned packing";
            const size_t aligned_sz = engine.type_sizeof("OutSlotFrame");
            EXPECT_GT(aligned_sz, 0u);
            EXPECT_EQ(aligned_sz,
                      pylabhub::hub::compute_schema_size(spec, "aligned"))
                << "engine ctypes_sizeof must match compute_schema_size "
                   "for every scalar type";

            // Phase 2 — packed (re-register under same name).
            ASSERT_TRUE(engine.register_slot_type(spec, "OutSlotFrame",
                                                   "packed"))
                << "every scalar type must also build under packed";
            const size_t packed_sz = engine.type_sizeof("OutSlotFrame");
            EXPECT_GT(packed_sz, 0u);
            EXPECT_EQ(packed_sz,
                      pylabhub::hub::compute_schema_size(spec, "packed"));

            EXPECT_GE(aligned_sz, packed_sz)
                << "aligned layout can never be smaller than packed for "
                   "the same schema — sub-word-aligned fields may need "
                   "padding under aligned that packed drops";

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
                if (sc == "supports_multi_state_returns_false")
                    return supports_multi_state_returns_false(dir);

                fmt::print(stderr,
                           "[python_engine] ERROR: unknown scenario '{}'\n",
                           sc);
                return 1;
            });
    }
};
static PythonEngineWorkerRegistrar g_python_engine_registrar;

} // namespace
