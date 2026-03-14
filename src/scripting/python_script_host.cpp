/**
 * @file python_script_host.cpp
 * @brief PythonScriptHost — CPython interpreter boilerplate (home, PyConfig, scoped_interpreter).
 *
 * Implements `do_initialize()`: resolves the Python home path, configures PyConfig,
 * creates `py::scoped_interpreter`, optionally activates a venv, and delegates
 * to `do_python_work()`.
 *
 * Cleanup (Py_Finalize) is handled by `py::scoped_interpreter`'s destructor at
 * the end of `do_initialize()`'s scope — no separate `do_finalize()` needed.
 */
#include "plh_platform.hpp"
#include "python_script_host.hpp"

#include "utils/logger.hpp"

#include "utils/json_fwd.hpp"
#include <pybind11/embed.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace py = pybind11;
namespace fs = std::filesystem;

namespace pylabhub::scripting
{

// ============================================================================
// resolve_python_home — 3-tier Python home resolution
// ============================================================================
//
// Priority:
//   1. $PYLABHUB_PYTHON_HOME environment variable  (explicit override)
//   2. <prefix>/config/pylabhub.json → "python_home" (system config)
//   3. <prefix>/opt/python/           (standalone default)
//
// The "prefix" is derived from the executable path: <exe_dir>/..
//
// When source (1) or (2) provides a relative path, it is resolved relative
// to <prefix>.  Absolute paths are used as-is.
//
// Returns the resolved, validated Python home directory.
// Throws std::runtime_error if none of the sources yield a valid directory.
// ============================================================================

static fs::path resolve_python_home(const fs::path &exe_path)
{
    const fs::path prefix = fs::weakly_canonical(exe_path.parent_path() / "..");

    // --- Tier 1: Environment variable override ---
    const char *env_home = std::getenv("PYLABHUB_PYTHON_HOME");
    if (env_home && *env_home)
    {
        fs::path home(env_home);
        if (home.is_relative())
            home = fs::weakly_canonical(prefix / home);
        if (fs::is_directory(home))
        {
            LOGGER_INFO("PythonScriptHost: Python home from $PYLABHUB_PYTHON_HOME: '{}'",
                        home.string());
            return home;
        }
        LOGGER_WARN("PythonScriptHost: $PYLABHUB_PYTHON_HOME='{}' is not a directory — "
                    "falling through to config file",
                    home.string());
    }

    // --- Tier 2: System config file ---
    const fs::path config_file = prefix / "config" / "pylabhub.json";
    if (fs::is_regular_file(config_file))
    {
        try
        {
            std::ifstream ifs(config_file);
            const auto    j = nlohmann::json::parse(ifs);

            if (j.contains("python_home") && j["python_home"].is_string())
            {
                const auto val = j["python_home"].get<std::string>();
                if (!val.empty())
                {
                    fs::path home(val);
                    if (home.is_relative())
                        home = fs::weakly_canonical(prefix / home);
                    else
                        home = fs::weakly_canonical(home);
                    if (fs::is_directory(home))
                    {
                        LOGGER_INFO("PythonScriptHost: Python home from '{}': '{}'",
                                    config_file.string(), home.string());
                        return home;
                    }
                    LOGGER_WARN("PythonScriptHost: python_home='{}' from config is not "
                                "a directory — falling through to standalone",
                                home.string());
                }
            }
        }
        catch (const nlohmann::json::exception &e)
        {
            LOGGER_WARN("PythonScriptHost: failed to parse '{}': {} — falling through",
                        config_file.string(), e.what());
        }
    }

    // --- Tier 3: Standalone default ---
    const fs::path standalone = prefix / "opt" / "python";
    if (fs::is_directory(standalone))
    {
        LOGGER_INFO("PythonScriptHost: Python home (standalone): '{}'",
                    standalone.string());
        return standalone;
    }

    // --- All tiers exhausted ---
    throw std::runtime_error(
        "PythonScriptHost: cannot locate a Python installation.\n"
        "  Checked:\n"
        "    1. $PYLABHUB_PYTHON_HOME (not set or invalid)\n"
        "    2. " + config_file.string() + " (missing or no python_home)\n"
        "    3. " + standalone.string() + " (not found)\n\n"
        "  For standalone builds:  cmake --build build --target stage_all\n"
        "  For system Python:      create " + config_file.string() + " with:\n"
        "    {\"python_home\": \"/usr/local\"}\n");
}

// ============================================================================
// resolve_venvs_dir — where virtual environments are stored
// ============================================================================
// Always <prefix>/opt/python/venvs/. This is a fixed staging layout location.
// User customization is via requirements.txt, not directory structure.

static fs::path resolve_venvs_dir(const fs::path &exe_path)
{
    return fs::weakly_canonical(exe_path.parent_path() / ".." / "opt" / "python" / "venvs");
}

// ============================================================================
// do_initialize
// ============================================================================

bool PythonScriptHost::do_initialize(const fs::path& script_path)
{
    // -------------------------------------------------------------------------
    // Phase 1: Resolve Python home via 3-tier priority.
    // -------------------------------------------------------------------------
    const fs::path exe_path(platform::get_executable_name(/*include_path=*/true));
    const fs::path python_home = resolve_python_home(exe_path);

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

    // -------------------------------------------------------------------------
    // Phase 3b: Virtual environment activation (optional).
    //
    // When python_venv_ is non-empty, activate the named venv by adding its
    // site-packages to sys.path via site.addsitedir().  PYTHONHOME stays
    // pointing at the base interpreter (stdlib still works); the venv packages
    // overlay the base packages and take precedence (earlier in sys.path).
    //
    // Venv layout (created by `pylabhub-pyenv create-venv <name>`):
    //   <venvs_dir>/<name>/lib/python3.XX/site-packages  (Unix)
    //   <venvs_dir>/<name>/Lib/site-packages              (Windows)
    // -------------------------------------------------------------------------
    if (!python_venv_.empty())
    {
        const fs::path venvs_dir = resolve_venvs_dir(exe_path);
        const fs::path venv_dir  = venvs_dir / python_venv_;
        if (!fs::is_directory(venv_dir))
        {
            throw std::runtime_error(
                "PythonScriptHost: virtual environment '" + python_venv_
                + "' not found at '" + venv_dir.string()
                + "'. Create it with: pylabhub-pyenv create-venv " + python_venv_);
        }

        // Find the site-packages directory inside the venv.
        fs::path site_packages;
#if defined(_WIN32)
        site_packages = venv_dir / "Lib" / "site-packages";
#else
        // Unix: lib/python3.XX/site-packages — scan for the python3.* directory.
        const fs::path lib_dir = venv_dir / "lib";
        if (fs::is_directory(lib_dir))
        {
            for (const auto &entry : fs::directory_iterator(lib_dir))
            {
                if (entry.is_directory()
                    && entry.path().filename().string().starts_with("python"))
                {
                    site_packages = entry.path() / "site-packages";
                    break;
                }
            }
        }
#endif
        if (site_packages.empty() || !fs::is_directory(site_packages))
        {
            throw std::runtime_error(
                "PythonScriptHost: venv '" + python_venv_
                + "' exists but site-packages not found at '"
                + (site_packages.empty() ? venv_dir.string() : site_packages.string())
                + "'. The venv may be corrupted — recreate with: "
                  "pylabhub-pyenv remove-venv " + python_venv_
                + " && pylabhub-pyenv create-venv " + python_venv_);
        }

        // Activate: site.addsitedir() adds site-packages to sys.path and
        // processes .pth files (important for namespace packages).
        py::module_::import("site").attr("addsitedir")(site_packages.string());

        LOGGER_INFO("PythonScriptHost: activated venv '{}' (site-packages: '{}')",
                    python_venv_, site_packages.string());
    }

    // Delegate all application-specific Python work to the subclass.
    // do_python_work() must call signal_ready_() when ready, then block
    // until stop_ is set, then release all py::objects and return.
    do_python_work(script_path);

    LOGGER_INFO("PythonScriptHost: interpreter thread done; finalizing Python...");
    return true;
    // interp_guard destructor → Py_Finalize
}

} // namespace pylabhub::scripting
