/**
 * @file test_engine_factory.cpp
 * @brief Pattern 3 driver — `create_engine` factory dispatch tests
 *        (HEP-CORE-0011 §"Engine Construction Lifecycle").
 *
 * Migrated 2026-05-13 from the in-process `SetUpTestSuite`-owned
 * `LifecycleGuard` antipattern.  Worker bodies live in
 * `workers/engine_factory_workers.cpp`.
 *
 * Pattern 3 is the natural fit here: CPython's `Py_Initialize` /
 * `Py_FinalizeEx` cycle is unsafe to repeat in the same process, so
 * the original suite paid for that constraint by loading Python once
 * for every test (including Lua-only and Native-only cases).  One
 * subprocess per test means each Python-path test gets its own clean
 * init/finalize cycle, and Native-/Lua-path tests never trigger
 * Python at all — honoring the HEP-CORE-0011 laziness contract.
 */

#include "test_patterns.h"
#include <gtest/gtest.h>

using pylabhub::tests::IsolatedProcessTest;

class EngineFactoryTest : public IsolatedProcessTest
{
};

TEST_F(EngineFactoryTest, Native_ReturnsNonNull)
{
    auto w = SpawnWorker("engine_factory.native_returns_non_null");
    ExpectWorkerOk(w);
}

TEST_F(EngineFactoryTest, NativeWithChecksum_Accepts)
{
    auto w = SpawnWorker("engine_factory.native_with_checksum_accepts");
    ExpectWorkerOk(w);
}

TEST_F(EngineFactoryTest, NativeWithoutChecksum_Accepts)
{
    auto w = SpawnWorker("engine_factory.native_without_checksum_accepts");
    ExpectWorkerOk(w);
}

TEST_F(EngineFactoryTest, Lua_ReturnsNonNull)
{
    auto w = SpawnWorker("engine_factory.lua_returns_non_null");
    ExpectWorkerOk(w);
}

TEST_F(EngineFactoryTest, Python_ReturnsNonNull)
{
    auto w = SpawnWorker("engine_factory.python_returns_non_null");
    ExpectWorkerOk(w);
}

TEST_F(EngineFactoryTest, PythonWithVenv_Accepts)
{
    auto w = SpawnWorker("engine_factory.python_with_venv_accepts");
    ExpectWorkerOk(w);
}

TEST_F(EngineFactoryTest, UnknownType_FallsThroughToPython)
{
    auto w = SpawnWorker("engine_factory.unknown_type_falls_through_to_python");
    ExpectWorkerOk(w);
}

TEST_F(EngineFactoryTest, EmptyType_FallsThroughToPython)
{
    auto w = SpawnWorker("engine_factory.empty_type_falls_through_to_python");
    ExpectWorkerOk(w);
}
