/**
 * @file test_engine_factory.cpp
 * @brief L2 tests for create_engine.
 *
 * The factory maps @c script.type → concrete ScriptEngine instance and
 * wires type-specific options (`checksum` for native, `python_venv` for
 * python). Tests verify the dispatch covers every known type and that
 * unknown types fall through to PythonEngine (documented behavior).
 *
 * We don't construct any Python/Lua state here — we only instantiate the
 * engine objects. Full engine lifecycle is covered at L2 by
 * test_python_engine / test_lua_engine / test_scriptengine_native_dylib.
 */
#include "utils/script_engine_factory.hpp"
#include "utils/config/script_config.hpp"
#include "utils/script_engine.hpp"  // complete type for unique_ptr<ScriptEngine> dtor
#include "utils/lifecycle.hpp"
#include "utils/logger.hpp"
#include "utils/file_lock.hpp"
#include "utils/json_config.hpp"
#include "../../src/scripting/python_interpreter_module.hpp"  // ensure_python_interpreter_loaded

#include <gtest/gtest.h>
#include <memory>

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

/// Plugin registry must be initialised once per process before
/// `create_engine` returns non-null.  Per HEP-CORE-0011 §"Engine
/// Construction Lifecycle" the production binaries (`plh_role`,
/// `plh_hub`) call `init_scripting()` from `main()`; this test driver
/// runs the same wiring up-front so the dispatcher is live for every
/// test case.
///
/// `PythonEngine`'s ctor requests the `PythonInterpreter` dynamic
/// lifecycle module via `LifecycleManager::ensure_module_loaded`;
/// that requires a `LifecycleGuard` to be active in the calling
/// process.  We install one in `SetUpTestSuite` (Logger + FileLock +
/// JsonConfig — same minimum set used by the broader L2 in-process
/// tests) so PythonEngine construction succeeds.
class EngineFactoryTest : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        s_lifecycle_ = std::make_unique<pylabhub::utils::LifecycleGuard>(
            pylabhub::utils::MakeModDefList(
                pylabhub::utils::Logger::GetLifecycleModule(),
                pylabhub::utils::FileLock::GetLifecycleModule(),
                pylabhub::utils::JsonConfig::GetLifecycleModule()),
            std::source_location::current());
        pylabhub::scripting::init_scripting();
        // Load PythonInterpreter on the SUITE thread so Py_Initialize +
        // Py_FinalizeEx run on the same thread (= test runner thread,
        // which is what destructs s_lifecycle_ in TearDownTestSuite).
        // Per HEP-CORE-0011 Option E this is the same role plh_role's
        // main() plays in production.
        ASSERT_TRUE(pylabhub::scripting::ensure_python_interpreter_loaded())
            << "PythonInterpreter failed to load — Python tests skipped";
    }
    static void TearDownTestSuite() { s_lifecycle_.reset(); }

private:
    static std::unique_ptr<pylabhub::utils::LifecycleGuard> s_lifecycle_;
};

std::unique_ptr<pylabhub::utils::LifecycleGuard>
EngineFactoryTest::s_lifecycle_;

} // namespace

TEST_F(EngineFactoryTest, Native_ReturnsNonNull)
{
    auto engine = pylabhub::scripting::create_engine(
        make_cfg("native"));
    ASSERT_NE(engine, nullptr);
}

TEST_F(EngineFactoryTest, NativeWithChecksum_Accepts)
{
    auto sc = make_cfg("native");
    sc.checksum = "deadbeefdeadbeefdeadbeefdeadbeef";
    auto engine = pylabhub::scripting::create_engine(sc);
    ASSERT_NE(engine, nullptr);
}

TEST_F(EngineFactoryTest, NativeWithoutChecksum_Accepts)
{
    auto sc = make_cfg("native");
    sc.checksum = "";  // explicit empty — set_expected_checksum NOT called
    auto engine = pylabhub::scripting::create_engine(sc);
    ASSERT_NE(engine, nullptr);
}

TEST_F(EngineFactoryTest, Lua_ReturnsNonNull)
{
    auto engine = pylabhub::scripting::create_engine(
        make_cfg("lua"));
    ASSERT_NE(engine, nullptr);
}

TEST_F(EngineFactoryTest, Python_ReturnsNonNull)
{
    auto engine = pylabhub::scripting::create_engine(
        make_cfg("python"));
    ASSERT_NE(engine, nullptr);
}

TEST_F(EngineFactoryTest, PythonWithVenv_Accepts)
{
    auto sc = make_cfg("python");
    sc.python_venv = "/tmp/some_venv";
    auto engine = pylabhub::scripting::create_engine(sc);
    ASSERT_NE(engine, nullptr);
}

TEST_F(EngineFactoryTest, UnknownType_FallsThroughToPython)
{
    // Documented behavior: anything not "native"/"lua" → PythonEngine.
    // We can't distinguish Python from Lua by the abstract return type,
    // so the contract here is "non-null" only. Full-stack tests exercise
    // the actual engine selection via --validate.
    auto engine = pylabhub::scripting::create_engine(
        make_cfg("some_unknown_flavor"));
    ASSERT_NE(engine, nullptr);
}

TEST_F(EngineFactoryTest, EmptyType_FallsThroughToPython)
{
    auto engine = pylabhub::scripting::create_engine(
        make_cfg(""));
    ASSERT_NE(engine, nullptr);
}
