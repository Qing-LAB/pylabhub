/**
 * @file engine_factory_workers.cpp
 * @brief Worker bodies for ScriptEngine factory dispatch tests
 *        (HEP-CORE-0011 §"Engine Construction Lifecycle"; Pattern 3).
 *
 * Migrated 2026-05-13 from in-process `SetUpTestSuite`-owned
 * `LifecycleGuard` to the subprocess-isolation model.  Pattern 3 fits
 * this file especially well: CPython's `Py_Initialize` →
 * `Py_FinalizeEx` → `Py_Initialize` cycle is unsafe in the same
 * process, so the original suite paid for that constraint by loading
 * Python proactively for every test (including Native- and Lua-only
 * cases) and never finalizing until program exit.  Pattern 3 gives
 * each test its own subprocess: Python-path tests get a clean
 * init/finalize cycle, and Native/Lua paths never trigger Python at
 * all — honoring the HEP-CORE-0011 §"PythonInterpreter dynamic
 * lifecycle module" laziness contract more faithfully than the
 * original in-process pattern could.
 *
 * Real production wiring: uses real `create_engine` (production
 * dispatcher via the plugin registry), real `init_scripting()`, real
 * `NativeEngine` / `LuaEngine` / `PythonEngine` constructors, real
 * `LifecycleGuard` + real dynamic `PythonInterpreter` module.  The
 * factory's `py::gil_scoped_acquire` on the Python path
 * (engine_factory.cpp:77-79) is documented as supporting test callers
 * that don't pre-acquire the GIL — that's this test file.
 */

#include "engine_factory_workers.h"

#include "shared_test_helpers.h"
#include "test_entrypoint.h"

#include "utils/config/script_config.hpp"
#include "utils/file_lock.hpp"
#include "utils/json_config.hpp"
#include "utils/logger.hpp"
#include "utils/script_engine.hpp"
#include "utils/script_engine_factory.hpp"
#include "../../src/scripting/python_interpreter_module.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string_view>

using pylabhub::tests::helper::run_gtest_worker;
using pylabhub::utils::FileLock;
using pylabhub::utils::JsonConfig;
using pylabhub::utils::Logger;

namespace pylabhub::tests::worker
{
namespace engine_factory
{

namespace
{

pylabhub::config::ScriptConfig make_cfg(const char *type)
{
    pylabhub::config::ScriptConfig sc;
    sc.type          = type;
    sc.path          = ".";
    sc.type_explicit = true;
    return sc;
}

/// Common worker prologue: install lifecycle (Logger+FileLock+JsonConfig)
/// and register the scripting factory.  Does NOT load PythonInterpreter
/// — that is the responsibility of the engine ctor on first construct,
/// per HEP-CORE-0011 §"PythonInterpreter dynamic lifecycle module"
/// (lazy load, never on process startup for Lua-only / Native-only).
template <typename Body>
int run_with_scripting(std::string_view worker_name, Body &&body)
{
    return run_gtest_worker(
        [body = std::forward<Body>(body)]() mutable {
            pylabhub::scripting::init_scripting();
            body();
        },
        std::string(worker_name).c_str(),
        Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule());
}

/// Python-path variant: additionally pre-loads PythonInterpreter so a
/// missing libpython produces a fail-fast at a located line rather
/// than a cryptic crash inside `PythonEngine`'s ctor.  Preserves the
/// original SetUpTestSuite's `ASSERT_TRUE(ensure_python_interpreter_loaded())`
/// defensive check — but now scoped to only the Python-path tests, so
/// Native/Lua tests no longer pay the Python init cost.
template <typename Body>
int run_with_python(std::string_view worker_name, Body &&body)
{
    return run_with_scripting(worker_name, [body = std::forward<Body>(body)]() mutable {
        ASSERT_TRUE(pylabhub::scripting::ensure_python_interpreter_loaded())
            << "PythonInterpreter failed to load — Python tests skipped";
        body();
    });
}

} // namespace

int native_returns_non_null()
{
    return run_with_scripting(
        "engine_factory::native_returns_non_null",
        [] {
            auto engine = pylabhub::scripting::create_engine(make_cfg("native"));
            ASSERT_NE(engine, nullptr);
        });
}

int native_with_checksum_accepts()
{
    return run_with_scripting(
        "engine_factory::native_with_checksum_accepts",
        [] {
            auto sc = make_cfg("native");
            sc.checksum = "deadbeefdeadbeefdeadbeefdeadbeef";
            auto engine = pylabhub::scripting::create_engine(sc);
            ASSERT_NE(engine, nullptr);
        });
}

int native_without_checksum_accepts()
{
    return run_with_scripting(
        "engine_factory::native_without_checksum_accepts",
        [] {
            auto sc = make_cfg("native");
            sc.checksum = "";  // explicit empty — set_expected_checksum NOT called
            auto engine = pylabhub::scripting::create_engine(sc);
            ASSERT_NE(engine, nullptr);
        });
}

int lua_returns_non_null()
{
    return run_with_scripting(
        "engine_factory::lua_returns_non_null",
        [] {
            auto engine = pylabhub::scripting::create_engine(make_cfg("lua"));
            ASSERT_NE(engine, nullptr);
        });
}

int python_returns_non_null()
{
    return run_with_python(
        "engine_factory::python_returns_non_null",
        [] {
            auto engine = pylabhub::scripting::create_engine(make_cfg("python"));
            ASSERT_NE(engine, nullptr);
        });
}

int python_with_venv_accepts()
{
    return run_with_python(
        "engine_factory::python_with_venv_accepts",
        [] {
            auto sc = make_cfg("python");
            sc.python_venv = "/tmp/some_venv";
            auto engine = pylabhub::scripting::create_engine(sc);
            ASSERT_NE(engine, nullptr);
        });
}

int unknown_type_falls_through_to_python()
{
    return run_with_python(
        "engine_factory::unknown_type_falls_through_to_python",
        [] {
            // Documented behavior: anything not "native"/"lua" → PythonEngine.
            // We can't distinguish Python from Lua by the abstract return type,
            // so the contract here is "non-null" only. Full-stack tests exercise
            // the actual engine selection via --validate.
            auto engine = pylabhub::scripting::create_engine(
                make_cfg("some_unknown_flavor"));
            ASSERT_NE(engine, nullptr);
        });
}

int empty_type_falls_through_to_python()
{
    return run_with_python(
        "engine_factory::empty_type_falls_through_to_python",
        [] {
            auto engine = pylabhub::scripting::create_engine(make_cfg(""));
            ASSERT_NE(engine, nullptr);
        });
}

} // namespace engine_factory
} // namespace pylabhub::tests::worker

// ── Dispatcher registrar ────────────────────────────────────────────────────

namespace
{

struct EngineFactoryRegistrar
{
    EngineFactoryRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2) return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "engine_factory")
                    return -1;
                std::string sc(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::engine_factory;

                if (sc == "native_returns_non_null")
                    return native_returns_non_null();
                if (sc == "native_with_checksum_accepts")
                    return native_with_checksum_accepts();
                if (sc == "native_without_checksum_accepts")
                    return native_without_checksum_accepts();
                if (sc == "lua_returns_non_null")
                    return lua_returns_non_null();
                if (sc == "python_returns_non_null")
                    return python_returns_non_null();
                if (sc == "python_with_venv_accepts")
                    return python_with_venv_accepts();
                if (sc == "unknown_type_falls_through_to_python")
                    return unknown_type_falls_through_to_python();
                if (sc == "empty_type_falls_through_to_python")
                    return empty_type_falls_through_to_python();
                return -1;
            });
    }
} g_registrar;

} // namespace
