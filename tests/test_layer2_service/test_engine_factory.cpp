/**
 * @file test_engine_factory.cpp
 * @brief L2 tests for make_engine_from_script_config.
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
#include "engine_factory.hpp"
#include "utils/config/script_config.hpp"

#include <gtest/gtest.h>

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

} // namespace

TEST(EngineFactoryTest, Native_ReturnsNonNull)
{
    auto engine = pylabhub::scripting::make_engine_from_script_config(
        make_cfg("native"));
    ASSERT_NE(engine, nullptr);
}

TEST(EngineFactoryTest, NativeWithChecksum_Accepts)
{
    auto sc = make_cfg("native");
    sc.checksum = "deadbeefdeadbeefdeadbeefdeadbeef";
    auto engine = pylabhub::scripting::make_engine_from_script_config(sc);
    ASSERT_NE(engine, nullptr);
}

TEST(EngineFactoryTest, NativeWithoutChecksum_Accepts)
{
    auto sc = make_cfg("native");
    sc.checksum = "";  // explicit empty — set_expected_checksum NOT called
    auto engine = pylabhub::scripting::make_engine_from_script_config(sc);
    ASSERT_NE(engine, nullptr);
}

TEST(EngineFactoryTest, Lua_ReturnsNonNull)
{
    auto engine = pylabhub::scripting::make_engine_from_script_config(
        make_cfg("lua"));
    ASSERT_NE(engine, nullptr);
}

TEST(EngineFactoryTest, Python_ReturnsNonNull)
{
    auto engine = pylabhub::scripting::make_engine_from_script_config(
        make_cfg("python"));
    ASSERT_NE(engine, nullptr);
}

TEST(EngineFactoryTest, PythonWithVenv_Accepts)
{
    auto sc = make_cfg("python");
    sc.python_venv = "/tmp/some_venv";
    auto engine = pylabhub::scripting::make_engine_from_script_config(sc);
    ASSERT_NE(engine, nullptr);
}

TEST(EngineFactoryTest, UnknownType_FallsThroughToPython)
{
    // Documented behavior: anything not "native"/"lua" → PythonEngine.
    // We can't distinguish Python from Lua by the abstract return type,
    // so the contract here is "non-null" only. Full-stack tests exercise
    // the actual engine selection via --validate.
    auto engine = pylabhub::scripting::make_engine_from_script_config(
        make_cfg("some_unknown_flavor"));
    ASSERT_NE(engine, nullptr);
}

TEST(EngineFactoryTest, EmptyType_FallsThroughToPython)
{
    auto engine = pylabhub::scripting::make_engine_from_script_config(
        make_cfg(""));
    ASSERT_NE(engine, nullptr);
}
