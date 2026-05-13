/**
 * @file test_hub_python_integration.cpp
 * @brief Pattern 3 driver — HubHost+PythonEngine integration test
 *        (HEP-CORE-0033 Phase 7 D4.2; mirrors test_hub_lua_integration.cpp).
 *
 * Single TEST_F by design: pybind11's `py::scoped_interpreter` calls
 * `Py_InitializeEx` / `Py_FinalizeEx` around the engine's lifetime;
 * multiple init/finalize cycles in a single process are problematic
 * for embedded modules.  Pattern 3 isolation gives each scenario its
 * own fresh subprocess — see workers/hub_python_integration_workers.cpp.
 *
 * Migrated 2026-05-13 from the in-process `SetUpTestSuite`-owned
 * `LifecycleGuard` antipattern explicitly ruled out by
 * `docs/README/README_testing.md` § "Antipatterns".
 */

#include "test_patterns.h"
#include <gtest/gtest.h>

using pylabhub::tests::IsolatedProcessTest;

class HubPythonIntegrationTest : public IsolatedProcessTest
{
};

TEST_F(HubPythonIntegrationTest, RealPythonScript_OnInitOnStop_FireAndLog)
{
    auto w = SpawnWorker(
        "hub_python_integration.realpythonscript_oninit_onstop_fireandlog");
    ExpectWorkerOk(w);
}
