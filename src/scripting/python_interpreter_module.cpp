/**
 * @file python_interpreter_module.cpp
 * @brief Implementation of the PythonInterpreter dynamic lifecycle module.
 *
 * **Ownership + threading model (HEP-CORE-0011 §"Engine Construction Lifecycle",
 * 2026-05-08 final design):**
 *
 *   - Registered as a **persistent dynamic** lifecycle module
 *     (`set_as_persistent(true)`).  Loaded conditionally — `main()` calls
 *     `ensure_python_interpreter_loaded()` after config parse iff any
 *     role/hub uses Python.  Native-only / Lua-only deployments never
 *     load it; no Py_InitializeFromConfig cost paid.
 *
 *   - **Init runs on whichever thread first calls
 *     `ensure_python_interpreter_loaded`** — production: main, after
 *     config parse.  pi_startup records the owner thread, runs
 *     `Py_InitializeFromConfig`, then immediately releases the GIL via
 *     a stored `py::gil_scoped_release` so worker threads can acquire
 *     it via `py::gil_scoped_acquire`.
 *
 *   - **Workers acquire the GIL via `py::gil_scoped_acquire`** at the
 *     top of `worker_main_` (see PythonGilLease in
 *     utils/script_engine_factory.hpp).  Workers never call
 *     `ensure_*` / `release_*` — the persistent contract makes that
 *     unnecessary.
 *
 *   - **Finalize runs on the `~LifecycleGuard` thread** (= main, in
 *     production).  Persistent modules stay LOADED across refcount
 *     drops; only `finalize()` Phase 2 tears them down — and Phase 2
 *     runs synchronously inline after Phase 1 has joined the dynamic-
 *     shutdown thread.  pi_shutdown therefore CANNOT run on the
 *     dynamic-shutdown thread for this module, by construction.
 *
 *   - **pi_shutdown self-check.**  As defence-in-depth against future
 *     framework regressions, pi_shutdown verifies the calling thread
 *     matches the recorded owner and PLH_PANICs on mismatch — the
 *     wrong-thread Py_FinalizeEx that motivated this design becomes
 *     impossible.
 *
 *   - **Sticky terminated flag** (`g_terminated`) refuses
 *     post-finalize re-init.  CPython does not robustly support
 *     init→finalize→init cycles (extension state, GC roots, atexit
 *     hooks all leak across cycles); we forbid re-init explicitly.
 */

#include "python_interpreter_module.hpp"

#include "utils/lifecycle.hpp"
#include "utils/module_def.hpp"
#include "utils/logger.hpp"
#include "utils/debug_info.hpp"          // PLH_PANIC for cross-thread guard
#include "plh_platform.hpp"

#include <pybind11/embed.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

namespace py = pybind11;
namespace fs = std::filesystem;

namespace pylabhub::scripting
{

const char *const kPythonInterpreterModuleName =
    "pylabhub::scripting::PythonInterpreter";

namespace
{

/// Module userdata.  The interpreter and the main-thread GIL release
/// guard live here; lifetime is bounded by load (pi_startup) and the
/// once-per-process finalize (pi_shutdown via LifecycleManager Phase 2).
struct InterpreterState
{
    std::optional<py::scoped_interpreter> interp;

    /// pi_startup parks a `py::gil_scoped_release` here so the
    /// post-init GIL becomes available to workers.  pi_shutdown drops
    /// it BEFORE destroying `interp`, re-acquiring the GIL on the
    /// owner thread for Py_FinalizeEx.
    std::optional<py::gil_scoped_release> main_gil_release;

    /// True between successful pi_startup and pi_shutdown.
    std::atomic<bool> alive{false};

    /// Thread that ran pi_startup.  pi_shutdown PANICs if invoked on a
    /// different thread (defence-in-depth — the framework's persistent
    /// + Phase-2 contract guarantees same-thread shutdown, this catches
    /// any future regression loudly instead of via CPython UB).
    std::thread::id owner_thread{};
};

InterpreterState &state()
{
    static InterpreterState s;
    return s;
}

/// **Sticky termination flag.**  Set by pi_shutdown.  Once set,
/// `ensure_python_interpreter_loaded` refuses to re-init.  CPython's
/// init→finalize→init cycle is unsupported in practice (interned
/// strings, GC roots, signal handlers, extension state all leak across
/// cycles); we forbid it explicitly.
std::atomic<bool> g_terminated{false};

std::once_flag g_register_once;
std::atomic<bool> g_register_failed{false};

/// 3-tier Python home resolution (env → pylabhub.json → standalone).
fs::path resolve_python_home(const fs::path &exe_path)
{
    const fs::path prefix = fs::weakly_canonical(exe_path.parent_path() / "..");

    if (const char *env_home = std::getenv("PYLABHUB_PYTHON_HOME");
        env_home && *env_home)
    {
        fs::path home(env_home);
        if (home.is_relative()) home = fs::weakly_canonical(prefix / home);
        if (fs::is_directory(home))
        {
            LOGGER_INFO("PythonInterpreter: Python home from $PYLABHUB_PYTHON_HOME: '{}'",
                        home.string());
            return home;
        }
        LOGGER_WARN("PythonInterpreter: $PYLABHUB_PYTHON_HOME='{}' is not a "
                    "directory — falling through to config file",
                    home.string());
    }

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
                    home = home.is_relative()
                               ? fs::weakly_canonical(prefix / home)
                               : fs::weakly_canonical(home);
                    if (fs::is_directory(home))
                    {
                        LOGGER_INFO(
                            "PythonInterpreter: Python home from '{}': '{}'",
                            config_file.string(), home.string());
                        return home;
                    }
                    LOGGER_WARN("PythonInterpreter: python_home='{}' from config "
                                "is not a directory — falling through to standalone",
                                home.string());
                }
            }
        }
        catch (const nlohmann::json::exception &e)
        {
            LOGGER_WARN("PythonInterpreter: failed to parse '{}': {} — falling through",
                        config_file.string(), e.what());
        }
    }

    const fs::path standalone = prefix / "opt" / "python";
    if (fs::is_directory(standalone))
    {
        LOGGER_INFO("PythonInterpreter: Python home (standalone): '{}'",
                    standalone.string());
        return standalone;
    }

    throw std::runtime_error(
        "PythonInterpreter: cannot locate a Python installation.\n"
        "  Checked: $PYLABHUB_PYTHON_HOME, " + config_file.string()
        + ", " + standalone.string());
}

/// LifecycleCallback startup: construct py::scoped_interpreter on the
/// CALLING thread and release the GIL so workers can acquire it.
/// Records the calling thread as the owner; pi_shutdown PANICs if
/// invoked on a different thread (defence-in-depth).
void pi_startup(const char * /*arg*/, void * /*userdata*/)
{
    if (g_terminated.load(std::memory_order_acquire))
    {
        LOGGER_ERROR("PythonInterpreter: pi_startup invoked after "
                     "termination — refusing to re-initialise.");
        throw std::runtime_error("PythonInterpreter terminated; cannot re-initialise");
    }

    auto &s = state();
    if (s.alive.load(std::memory_order_acquire))
        return;  // already alive (defensive)

    const auto tid = pylabhub::platform::get_native_thread_id();
    LOGGER_INFO("PythonInterpreter: pi_startup ENTER on thread {} — "
                "this thread becomes the interpreter OWNER and will be "
                "the thread that runs Py_FinalizeEx.  pi_shutdown is "
                "guarded against cross-thread invocation (will abort "
                "loudly with a CPython-UB diagnostic if reached on a "
                "non-owner thread).", tid);

    try
    {
#if defined(_WIN32)
        _putenv_s("PYTHONUNBUFFERED", "1");
#else
        setenv("PYTHONUNBUFFERED", "1", 1);
#endif

        const fs::path exe_path(
            pylabhub::platform::get_executable_name(/*include_path=*/true));
        const fs::path python_home = resolve_python_home(exe_path);

        PyConfig config;
        PyConfig_InitPythonConfig(&config);
        config.parse_argv             = 0;
        config.install_signal_handlers = 0;

        const std::string home_str  = python_home.string();
        wchar_t          *home_wstr = Py_DecodeLocale(home_str.c_str(), nullptr);
        if (!home_wstr)
        {
            PyConfig_Clear(&config);
            throw std::runtime_error(
                "PythonInterpreter: Py_DecodeLocale failed for '" + home_str + "'");
        }
        PyConfig_SetString(&config, &config.home, home_wstr);
        PyMem_RawFree(home_wstr);

        // Init the interpreter — GIL acquired by THIS thread.
        s.interp.emplace(&config);
        PyConfig_Clear(&config);

        // Record the owner thread BEFORE releasing the GIL so the
        // identity is observable even by code racing to read it.
        s.owner_thread = std::this_thread::get_id();

        // Release the GIL on THIS thread so workers can acquire it.
        s.main_gil_release.emplace();

        s.alive.store(true, std::memory_order_release);

        LOGGER_INFO("PythonInterpreter: Python {} initialised on thread {}; "
                    "home='{}'.  GIL released to the cross-thread pool.",
                    Py_GetVersion(),
                    pylabhub::platform::get_native_thread_id(),
                    python_home.string());
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("PythonInterpreter: startup failed: {}", e.what());
        s.main_gil_release.reset();
        s.interp.reset();
        s.alive.store(false, std::memory_order_release);
        throw;
    }
}

/// LifecycleCallback shutdown: re-acquire GIL + destroy the interpreter
/// (`Py_FinalizeEx`).  Reached only via `LifecycleManagerImpl::finalize()`
/// Phase 2 for persistent modules — synchronous on the
/// `~LifecycleGuard` thread.  PANICs if (defensively) invoked on a
/// different thread than the recorded owner.
void pi_shutdown(const char * /*arg*/, void * /*userdata*/)
{
    auto &s = state();
    if (!s.alive.load(std::memory_order_acquire))
        return;  // never started, or already finalised

    if (std::this_thread::get_id() != s.owner_thread)
    {
        // The framework's persistent + Phase-2 contract guarantees
        // pi_shutdown runs on the `~LifecycleGuard` thread, which by
        // construction matches pi_startup's thread (both are main, in
        // production).  Reaching this branch indicates a framework
        // regression OR a non-production path (mistakenly registered
        // non-persistent + UnloadModule-from-the-wrong-thread).  We
        // refuse to run Py_FinalizeEx (CPython UB) and PLH_PANIC so
        // the regression surfaces immediately rather than as a deep
        // CPython crash.
        PLH_PANIC("PythonInterpreter: pi_shutdown invoked on a thread other "
                  "than the recorded owner.  Refusing Py_FinalizeEx to avoid "
                  "CPython single-thread-finalise UB.  Likely cause: "
                  "framework regression in LifecycleManagerImpl::finalize() "
                  "Phase 2 thread affinity, OR an UnloadModule call on a "
                  "non-persistent registration of this module.");
    }

    // Bookended shutdown logging.  If Py_FinalizeEx hangs (Python
    // atexit handler deadlock, extension stuck in a finalizer, GIL
    // contention with a stray thread, etc.) the operator needs to see
    // exactly which step is in flight.  Each stage logs entry +
    // completion so a `tail -f` shows the progression and an absent
    // "completed" line pinpoints the stuck stage.
    const auto tid = pylabhub::platform::get_native_thread_id();
    LOGGER_INFO("PythonInterpreter: pi_shutdown ENTER on thread {} "
                "(persistent dynamic module — invoked from "
                "LifecycleManager::finalize() Phase 2, synchronous on the "
                "~LifecycleGuard thread).  Sequence: alive→false, "
                "terminated→true, drop main_gil_release (re-acquire GIL "
                "on this thread), then ~scoped_interpreter (Py_FinalizeEx).",
                tid);

    s.alive.store(false, std::memory_order_release);
    g_terminated.store(true, std::memory_order_release);

    LOGGER_INFO("PythonInterpreter: pi_shutdown step 1 — re-acquiring GIL "
                "on thread {} by dropping main_gil_release...", tid);
    s.main_gil_release.reset();
    LOGGER_INFO("PythonInterpreter: pi_shutdown step 1 done — GIL held on "
                "thread {}.", tid);

    LOGGER_INFO("PythonInterpreter: pi_shutdown step 2 — running "
                "Py_FinalizeEx via ~scoped_interpreter on thread {}...  "
                "If this is the LAST log line, the hang is in CPython "
                "finalisation: check for Python atexit handlers, "
                "extension finalizers (numpy/scipy), or threads still "
                "holding the GIL.", tid);
    s.interp.reset();
    LOGGER_INFO("PythonInterpreter: pi_shutdown step 2 done — Py_FinalizeEx "
                "returned on thread {}.  Interpreter is gone.", tid);

    LOGGER_INFO("PythonInterpreter: pi_shutdown EXIT on thread {} — "
                "embedded interpreter finalised.", tid);
}

bool register_module()
{
    try
    {
        pylabhub::utils::ModuleDef mod(
            kPythonInterpreterModuleName,
            /*userdata=*/nullptr,
            /*validate=*/nullptr);
        mod.add_dependency("pylabhub::utils::Logger");
        mod.set_startup(pi_startup);
        mod.set_shutdown(pi_shutdown,
                         std::chrono::milliseconds{5000});
        // Persistent: refcount drops do NOT trigger unload.  Module
        // stays loaded until LifecycleManager::finalize() Phase 2.
        mod.set_as_persistent(true);

        // Synchronous shutdown: Phase 2 calls pi_shutdown DIRECTLY on
        // the calling thread (= `~LifecycleGuard`'s thread = main, in
        // production) instead of spawning a timedShutdown worker.
        // Required for CPython's single-thread Py_Initialize/Py_Finalize
        // contract.  pi_shutdown's defensive owner-thread check is
        // belt-and-braces against any future framework regression.
        mod.set_synchronous_shutdown(true);

        if (!pylabhub::utils::LifecycleManager::instance()
                 .register_dynamic_module(std::move(mod)))
        {
            LOGGER_ERROR("PythonInterpreter: register_dynamic_module returned false");
            return false;
        }
        return true;
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("PythonInterpreter: register threw: {}", e.what());
        return false;
    }
}

} // anonymous namespace

bool ensure_python_interpreter_loaded() noexcept
{
    if (g_terminated.load(std::memory_order_acquire))
    {
        LOGGER_ERROR("PythonInterpreter: ensure_python_interpreter_loaded "
                     "called after termination — Python finalisation in "
                     "this process is permanent.  Returning false.");
        return false;
    }

    try
    {
        std::call_once(g_register_once, [&] {
            if (!register_module())
                g_register_failed.store(true, std::memory_order_release);
        });
    }
    catch (...)
    {
        g_register_failed.store(true, std::memory_order_release);
        return false;
    }

    if (g_register_failed.load(std::memory_order_acquire))
    {
        LOGGER_ERROR("PythonInterpreter: module registration failed permanently — "
                     "ensure_python_interpreter_loaded() returning false");
        return false;
    }

    try
    {
        if (!pylabhub::utils::LoadModule(kPythonInterpreterModuleName))
        {
            LOGGER_ERROR("PythonInterpreter: LoadModule returned false");
            return false;
        }
    }
    catch (const std::exception &e)
    {
        LOGGER_ERROR("PythonInterpreter: LoadModule threw: {}", e.what());
        return false;
    }

    return true;
}

bool python_interpreter_is_alive() noexcept
{
    return state().alive.load(std::memory_order_acquire);
}

bool validate_python_interpreter() noexcept
{
    if (!python_interpreter_is_alive())
        return false;
    if (Py_IsInitialized() == 0)
        return false;
    if (PyGILState_Check() == 0)
        return false;   // calling thread does not hold the GIL
    return true;
}

} // namespace pylabhub::scripting
