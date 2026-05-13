#pragma once
/**
 * @file hub_python_integration_workers.h
 * @brief Worker functions for HubHost+PythonEngine integration tests (Pattern 3).
 *
 * Single worker because pybind11's `py::scoped_interpreter` calls
 * `Py_InitializeEx` / `Py_FinalizeEx` around the engine's lifetime, and
 * multiple init/finalize cycles in a single process are problematic for
 * embedded modules.  Pattern 3 isolation gives this body its own fresh
 * subprocess.
 */

namespace pylabhub::tests::worker
{
namespace hub_python_integration
{

int realpythonscript_oninit_onstop_fireandlog();

} // namespace hub_python_integration
} // namespace pylabhub::tests::worker
