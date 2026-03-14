/**
 * @file python_script_host.cpp
 * @brief PythonScriptHost — CPython interpreter boilerplate (home, PyConfig, scoped_interpreter).
 *
 * Implements `do_initialize()`: derives the Python home path, configures PyConfig,
 * creates `py::scoped_interpreter`, and delegates to `do_python_work()`.
 *
 * Cleanup (Py_Finalize) is handled by `py::scoped_interpreter`'s destructor at
 * the end of `do_initialize()`'s scope — no separate `do_finalize()` needed.
 */
#include "plh_platform.hpp"
#include "python_script_host.hpp"

#include "utils/logger.hpp"

#include <pybind11/embed.h>

#include <cstdlib>
#include <filesystem>
#include <stdexcept>

namespace py = pybind11;
namespace fs = std::filesystem;

namespace pylabhub::scripting
{

bool PythonScriptHost::do_initialize(const fs::path& script_path)
{
    // -------------------------------------------------------------------------
    // Phase 1: Derive Python home from the executable path.
    //
    // Convention: <exe_dir>/../opt/python  (relative to the staged binary).
    // This is the standalone Python installed by `cmake --build build --target stage_all`.
    // Hard error if missing — there is no safe fallback (silently using the
    // system Python would break reproducibility and may not have pybind11).
    // -------------------------------------------------------------------------
    const fs::path exe_path(platform::get_executable_name(/*include_path=*/true));
    const fs::path python_home =
        fs::weakly_canonical(exe_path.parent_path() / ".." / "opt" / "python");

    if (!fs::is_directory(python_home))
    {
        throw std::runtime_error(
            "PythonScriptHost: standalone Python not found at '"
            + python_home.string()
            + "'. Run 'cmake --build build --target stage_all' first.");
    }

    LOGGER_INFO("PythonScriptHost: Python home '{}'", python_home.string());

    // -------------------------------------------------------------------------
    // Phase 2: Configure PyConfig.
    //
    // parse_argv = 0             — do not consume process argv
    // install_signal_handlers = 0 — caller manages signals (e.g., SIGINT)
    // home                       — PYTHONHOME for the bundled standalone Python
    // -------------------------------------------------------------------------
    // Force unbuffered Python I/O. Must be set before PyConfig_InitPythonConfig
    // reads the environment. This is the C-level mechanism; no Python-side
    // monkey-patching needed.
    // XPLAT: setenv is POSIX-only; _putenv_s is the Windows equivalent.
#if defined(_WIN32)
    _putenv_s("PYTHONUNBUFFERED", "1");
#else
    setenv("PYTHONUNBUFFERED", "1", 1);
#endif

    PyConfig config;
    PyConfig_InitPythonConfig(&config);
    config.parse_argv             = 0;
    config.install_signal_handlers = 0;

    const std::string home_str  = python_home.string();
    wchar_t*          home_wstr = Py_DecodeLocale(home_str.c_str(), nullptr);
    if (!home_wstr)
    {
        PyConfig_Clear(&config);
        throw std::runtime_error(
            "PythonScriptHost: Py_DecodeLocale failed for path '" + home_str + "'");
    }
    PyConfig_SetString(&config, &config.home, home_wstr);
    PyMem_RawFree(home_wstr);

    // -------------------------------------------------------------------------
    // Phase 3–N: Interpreter lifetime.
    //
    // py::scoped_interpreter constructor = Py_Initialize (GIL held on entry).
    // py::scoped_interpreter destructor  = Py_Finalize  (runs at scope exit).
    //
    // All py::object instances created by do_python_work() MUST be destroyed
    // before this scope closes — otherwise Py_Finalize will crash.
    // -------------------------------------------------------------------------
    py::scoped_interpreter interp_guard(&config);
    LOGGER_INFO("PythonScriptHost: Python {} initialized on interpreter thread",
                Py_GetVersion());

    // Delegate all application-specific Python work to the subclass.
    // do_python_work() must call signal_ready_() when ready, then block
    // until stop_ is set, then release all py::objects and return.
    do_python_work(script_path);

    LOGGER_INFO("PythonScriptHost: interpreter thread done; finalizing Python...");
    return true;
    // interp_guard destructor → Py_Finalize
}

} // namespace pylabhub::scripting
