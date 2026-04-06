/**
 * @file test_datahub_engine_roundtrip.cpp
 * @brief L3 engine integration tests: script engine → RoleAPIBase → SHM → consumer.
 *
 * Verifies that all three script engines (Python, Lua, Native) produce correct
 * multifield data through real infrastructure (broker + SHM + queues).
 * Each test spawns a worker process for interpreter isolation.
 */
#include "test_patterns.h"
#include "test_process_utils.h"
#include <gtest/gtest.h>

using namespace pylabhub::tests;

class DatahubEngineRoundtripTest : public IsolatedProcessTest
{
};

TEST_F(DatahubEngineRoundtripTest, PythonEngine_SHM_Roundtrip)
{
    auto proc = SpawnWorker("engine.python_shm_roundtrip", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubEngineRoundtripTest, LuaEngine_SHM_Roundtrip)
{
    auto proc = SpawnWorker("engine.lua_shm_roundtrip", {});
    ExpectWorkerOk(proc);
}

TEST_F(DatahubEngineRoundtripTest, NativeEngine_SHM_Roundtrip)
{
    auto proc = SpawnWorker("engine.native_shm_roundtrip", {});
    ExpectWorkerOk(proc);
}
