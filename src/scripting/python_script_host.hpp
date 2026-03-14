#pragma once
/**
 * @file python_script_host.hpp
 * @brief PythonScriptHost — CPython concrete ScriptHost for embedded Python.
 *
 * Part of `pylabhub-scripting` static library.  Handles all CPython
 * initialization boilerplate so that subclasses only need to implement
 * the application-specific Python work.
 *
 * ## What PythonScriptHost does
 *
 * `do_initialize()` (called on the dedicated interpreter thread by the base
 * `ScriptHost::thread_fn_()`) performs:
 *
 *  1. Derive Python home: `<exe_dir>/../opt/python` (hard error if missing).
 *  2. Configure `PyConfig`: `parse_argv = 0`, `install_signal_handlers = 0`.
 *  3. Create `py::scoped_interpreter` → `Py_Initialize` (GIL held on entry).
 *  4. Call `do_python_work(script_path)` — pure virtual, runs on this thread.
 *  5. `py::scoped_interpreter` destructor → `Py_Finalize`.
 *
 * `do_finalize()` is a no-op because all cleanup is done inside
 * `do_initialize()`'s scope (before `py::scoped_interpreter` destructs).
 *
 * ## Subclass contract
 *
 * `do_python_work()` is called with the GIL **held** and a live interpreter.
 * The implementation must:
 *  - Call `signal_ready_()` (inherited from `ScriptHost`) when the interpreter
 *    is ready for external use (e.g., after `on_start` completes).  This
 *    unblocks `base_startup_()` on the main thread.
 *  - Block in a tick loop, checking `stop_` (inherited `std::atomic<bool>`)
 *    regularly.
 *  - Release all `py::object` instances before returning, since `Py_Finalize`
 *    follows immediately.
 *  - NOT propagate exceptions: catch all `py::error_already_set` and
 *    `std::exception` internally.
 *
 * ## See also
 *
 * HEP-CORE-0011: ScriptHost Abstraction Framework (docs/HEP/)
 */

#include "plh_platform.hpp"
#include "utils/script_host.hpp"

#include <filesystem>

namespace pylabhub::scripting
{

/**
 * @class PythonScriptHost
 * @brief Abstract ScriptHost that manages the CPython interpreter lifetime.
 *
 * Subclasses implement `do_python_work()` with the application-specific
 * script loading, callback dispatch, and tick loop.
 */
class PythonScriptHost : public utils::ScriptHost
{
public:
    ~PythonScriptHost() override = default;

    /// Python requires a dedicated interpreter thread. Always returns `true`.
    [[nodiscard]] bool owns_dedicated_thread() const noexcept override { return true; }

protected:
    /**
     * @brief Application-specific Python work. Called with interpreter live and GIL held.
     *
     * The implementation must:
     *  - Call `signal_ready_()` once the interpreter is ready for external callers.
     *  - Block in a tick loop until `stop_` is set.
     *  - Release all `py::object` locals before returning.
     *
     * @param script_path  Base script directory; subclass appends language-specific subpath.
     */
    virtual void do_python_work(const std::filesystem::path& script_path) = 0;

    // ScriptHost interface
    bool do_initialize(const std::filesystem::path& script_path) override;
    void do_finalize() noexcept override {} ///< No-op: cleanup done in do_initialize scope.
};

} // namespace pylabhub::scripting
