#pragma once
/**
 * @file python_interpreter.hpp
 * @brief PythonInterpreter — embedded Python singleton lifecycle module.
 *
 * Manages a single CPython interpreter instance for the hubshell.
 * The interpreter is initialised once at lifecycle startup and holds a
 * persistent `__main__` namespace that survives between exec() calls
 * (variables defined in one exec call are visible in the next).
 *
 * ## Lifecycle
 *
 * Register via `LifecycleGuard`:
 * @code
 *   LifecycleGuard lifecycle(MakeModDefList(
 *       Logger::GetLifecycleModule(),
 *       pylabhub::crypto::GetLifecycleModule(),
 *       HubConfig::GetLifecycleModule(),
 *       PythonInterpreter::GetLifecycleModule(),
 *       ...));
 * @endcode
 *
 * Startup order: `Logger → CryptoUtils → HubConfig → PythonInterpreter → ...`
 *
 * ## Thread safety
 *
 * - `exec()` serialises concurrent callers via a mutex; the GIL is acquired
 *   internally for each call.
 * - `get_instance()` is safe from any thread after lifecycle startup.
 * - `reset_namespace()` is safe from any thread after lifecycle startup.
 */

#include "plh_service.hpp"

#include <functional>
#include <memory>
#include <string>

namespace pylabhub
{

/**
 * @brief Result returned by `PythonInterpreter::exec()`.
 */
struct PyExecResult
{
    bool        success{false};  ///< True if no exception was raised.
    std::string output;          ///< Captured stdout + stderr during execution.
    std::string error;           ///< Exception message or traceback (empty on success).
    std::string result_repr;     ///< repr() of the last expression value, if any.
};

/**
 * @class PythonInterpreter
 * @brief Singleton lifecycle module that owns the embedded CPython interpreter.
 */
class PythonInterpreter
{
  public:
    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    /** Returns the ModuleDef for use with LifecycleGuard. */
    static utils::ModuleDef GetLifecycleModule();

    /** Returns the global singleton instance. Call only after lifecycle startup. */
    static PythonInterpreter& get_instance();

    // -----------------------------------------------------------------------
    // Shutdown callback
    // -----------------------------------------------------------------------

    /**
     * @brief Registers a callback invoked when Python code calls `pylabhub.shutdown()`.
     *
     * Call this from hubshell before starting the main loop.
     * The callback is invoked from whichever thread calls exec() with shutdown code;
     * it should only set a flag — do not join threads or do heavy work inside it.
     */
    static void set_shutdown_callback(std::function<void()> cb);

    /** Invokes the registered shutdown callback (called by the pybind11 module). */
    static void request_shutdown();

    // -----------------------------------------------------------------------
    // Python execution
    // -----------------------------------------------------------------------

    /**
     * @brief Executes Python source code in the persistent namespace.
     *
     * Thread-safe: serialises callers with a mutex; acquires the GIL internally.
     * stdout and stderr are redirected to a StringIO buffer for the duration of
     * the call and returned in `PyExecResult::output`.
     *
     * @param code  Python source code to execute (may be multi-line).
     * @return      Execution result with captured output and error info.
     */
    PyExecResult exec(const std::string& code);

    /**
     * @brief Clears all user-defined names from the persistent namespace.
     *
     * Built-in names and imported modules that were set up at startup are
     * preserved. Thread-safe.
     */
    void reset_namespace();

    // -----------------------------------------------------------------------
    // Non-copyable, non-movable singleton
    // -----------------------------------------------------------------------
    PythonInterpreter(const PythonInterpreter&) = delete;
    PythonInterpreter& operator=(const PythonInterpreter&) = delete;
    PythonInterpreter(PythonInterpreter&&) = delete;
    PythonInterpreter& operator=(PythonInterpreter&&) = delete;

    // -----------------------------------------------------------------------
    // Internal lifecycle hooks (public so anonymous-namespace startup/shutdown
    // functions defined in the .cpp can call them; not part of the public API)
    // -----------------------------------------------------------------------
    /// @internal Called by lifecycle startup function.
    void startup_();
    /// @internal Called by lifecycle shutdown function.
    void shutdown_();

  private:
    PythonInterpreter();
    ~PythonInterpreter();

    struct Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace pylabhub
