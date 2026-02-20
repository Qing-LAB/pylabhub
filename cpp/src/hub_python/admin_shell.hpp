#pragma once
/**
 * @file admin_shell.hpp
 * @brief AdminShell — ZMQ REP server for the embedded Python admin interface.
 *
 * Binds a ZMQ REP socket to `HubConfig::admin_endpoint()` (default
 * `tcp://127.0.0.1:5600`) and dispatches JSON requests to
 * `PythonInterpreter::exec()`. Clients send Python source code; results are
 * returned as JSON.
 *
 * ## Protocol
 *
 * **Request** (JSON string over ZMQ):
 * @code
 * {
 *   "token": "optional-pre-shared-token",
 *   "code":  "python_source_code_here"
 * }
 * @endcode
 *
 * **Response** (JSON string over ZMQ):
 * @code
 * {
 *   "success": true | false,
 *   "output": "captured stdout + stderr during execution",
 *   "error":  "exception message or empty string on success"
 * }
 * @endcode
 *
 * If an `admin_token` is configured in `hub.user.json`, the request `token`
 * field must match; otherwise the response is `{"success":false,"error":"unauthorized"}`.
 * If no token is configured, any local connection is accepted (the socket is
 * bound to `127.0.0.1` by default, restricting access to the local machine).
 *
 * ## Lifecycle
 *
 * Register via `LifecycleGuard`:
 * @code
 *   LifecycleGuard lifecycle(MakeModDefList(
 *       Logger::GetLifecycleModule(),
 *       HubConfig::GetLifecycleModule(),
 *       PythonInterpreter::GetLifecycleModule(),
 *       pylabhub::hub::GetZMQContextModule(),
 *       AdminShell::GetLifecycleModule(),
 *       ...));
 * @endcode
 *
 * Startup order: `Logger → HubConfig → ZMQContext → PythonInterpreter → AdminShell`
 *
 * ## Thread safety
 *
 * - `startup_()` and `shutdown_()` are called from the lifecycle thread (main).
 * - The worker thread runs independently and calls `PythonInterpreter::exec()`,
 *   which is itself thread-safe (serialises via mutex + acquires the GIL).
 */

#include "plh_service.hpp"

#include <memory>

namespace pylabhub
{

/**
 * @class AdminShell
 * @brief Singleton lifecycle module that owns the admin ZMQ REP shell.
 */
class AdminShell
{
  public:
    // -----------------------------------------------------------------------
    // Lifecycle
    // -----------------------------------------------------------------------

    /** Returns the ModuleDef for use with LifecycleGuard. */
    static utils::ModuleDef GetLifecycleModule();

    /** Returns the global singleton. Call only after lifecycle startup. */
    static AdminShell& get_instance();

    // -----------------------------------------------------------------------
    // Internal lifecycle hooks (public so anonymous-namespace functions can reach them)
    // -----------------------------------------------------------------------

    /// @internal Called by lifecycle startup function.
    void startup_();
    /// @internal Called by lifecycle shutdown function.
    void shutdown_();

    // -----------------------------------------------------------------------
    // Non-copyable, non-movable singleton
    // -----------------------------------------------------------------------
    AdminShell(const AdminShell&) = delete;
    AdminShell& operator=(const AdminShell&) = delete;
    AdminShell(AdminShell&&) = delete;
    AdminShell& operator=(AdminShell&&) = delete;

  private:
    AdminShell();
    ~AdminShell();

    struct Impl;
    std::unique_ptr<Impl> pImpl;
};

} // namespace pylabhub
