#pragma once
/**
 * @file role_host_base.hpp
 * @brief RoleHostBase — abstract base for role hosts.
 *
 * Unifies the public lifecycle surface that binaries (plh_role, per-role
 * legacy mains) call on a role host:
 *
 *     construct → startup_() → (is_running()/wait_for_wakeup() loop) → shutdown_()
 *
 * The trailing-underscore methods follow the project convention: they are
 * intended to be called only by code tightly coupled with the role module
 * (binary mains, not generic external consumers).
 *
 * Each concrete role (Producer/Consumer/Processor/custom) derives and
 * implements a single pure virtual hook, @ref worker_main_, which runs
 * the 14-step engine-and-broker lifecycle on the role's worker thread.
 *
 * Base owns shared state (role_tag, config, engine, RoleHostCore, RoleAPIBase,
 * ready-promise). Derived owns role-specific state (schemas, queues,
 * BrokerRequestComm, InboxQueue, CycleOps). The @ref RoleAPIBase is
 * constructed lazily in @ref startup so the worker thread can be spawned
 * via its ThreadManager — putting it under the same bounded-join /
 * ERROR-on-timeout contract as every other role-scope thread.
 *
 * New roles add custom worker threads by calling @c api().thread_manager()
 * @c .spawn(...) inside their @ref worker_main_; all such threads are
 * drained by `ThreadManager::~ThreadManager` during @ref shutdown.
 *
 * Thread safety: construct, startup(), shutdown() on one thread (usually
 * main). is_running()/script_load_ok()/wait_for_wakeup() may be called
 * from the same thread while the worker runs. Not copyable, not movable
 * (owns an internal worker thread).
 */

#include "pylabhub_utils_export.h"
#include "utils/config/role_config.hpp"
#include "utils/role_api_base.hpp"
#include "utils/role_host_core.hpp"
#include "utils/script_engine.hpp"

#include <atomic>
#include <future>
#include <memory>
#include <string>
#include <string_view>

namespace pylabhub::scripting
{

class PYLABHUB_UTILS_EXPORT RoleHostBase
{
  public:
    /// @param role_tag    Short role tag used for RoleAPIBase construction
    ///                    and log prefixes (e.g. "prod"/"cons"/"proc").
    /// @param config      Parsed role config (moved into the host).
    /// @param engine      Script engine (moved into the host).
    /// @param shutdown_flag  External shutdown signal shared with main;
    ///                       may be nullptr.
    RoleHostBase(std::string_view role_tag,
                 config::RoleConfig config,
                 std::unique_ptr<ScriptEngine> engine,
                 std::atomic<bool> *shutdown_flag = nullptr);

    /// Pure virtual — RoleHostBase is abstract. Every derived class must
    /// declare its own destructor and call @ref shutdown_ as the FIRST
    /// statement of that destructor so the worker thread is joined
    /// before any derived-owned infrastructure members begin destruction.
    ///
    /// **C++ guarantee that this destructor always runs**:
    /// For any object whose construction completed through this class's
    /// constructor, the C++ language guarantees that this destructor
    /// body is executed when the object's lifetime ends — via normal
    /// scope exit, exception unwinding, `delete`, `unique_ptr::reset`,
    /// container teardown, or static/thread-local destruction.
    /// The pure-virtual marker (`= 0`) **does not** change this; it only
    /// makes the class abstract. The out-of-line definition (in
    /// role_host_base.cpp) is required and is always invoked by every
    /// derived destructor's implicit base-destructor call.
    /// The only way to skip this destructor is if `RoleHostBase`'s own
    /// constructor threw — but in that case no object exists, so there
    /// is nothing to clean up.
    ///
    /// **Contract enforcement**:
    /// The body checks the @c shutdown_called_ flag set by @ref shutdown_
    /// and calls `std::abort()` with a clear diagnostic if the flag was
    /// never set. No silent fallback — programmer error (a missed
    /// @ref shutdown_ call or an override that did not forward to
    /// `RoleHostBase::shutdown_()`) is surfaced loudly.
    virtual ~RoleHostBase() = 0;

    RoleHostBase(const RoleHostBase &)            = delete;
    RoleHostBase &operator=(const RoleHostBase &) = delete;
    RoleHostBase(RoleHostBase &&)                 = delete;
    RoleHostBase &operator=(RoleHostBase &&)      = delete;

    // ── Configuration ────────────────────────────────────────────────────

    /// Enable validate-only mode (parse + engine load; no broker / no loop).
    /// Must be called before @ref startup.
    void set_validate_only(bool v) { core_.set_validate_only(v); }

    // ── Lifecycle ────────────────────────────────────────────────────────

    /// Spawn worker thread via api_->thread_manager(); block until ready
    /// (or the worker signals failure). Constructs @ref api lazily so the
    /// ThreadManager owning the worker is this role's own.
    void startup_();

    /// Signal shutdown, notify the worker, drop @ref api — the resulting
    /// ThreadManager destructor bounded-joins every role-scope thread
    /// (worker, ctrl, and any custom threads the role spawned).
    ///
    /// **Idempotent**: first call performs the work and sets an internal
    /// "shutdown done" flag; subsequent calls early-return.
    ///
    /// **Virtual** — derived classes MAY override to add pre- or post-
    /// shutdown custom steps (e.g., signaling custom worker threads to
    /// exit before the join, or role-specific post-join cleanup). Any
    /// override MUST call @c RoleHostBase::shutdown_() at some point in
    /// its body — otherwise the internal flag is never set and the base
    /// destructor aborts. Recommended pattern:
    /// @code
    ///   void MyRoleHost::shutdown_() noexcept override {
    ///       // optional: pre-join work on derived state
    ///       RoleHostBase::shutdown_();  // joins worker, sets flag
    ///       // optional: post-join cleanup (worker is gone here)
    ///   }
    /// @endcode
    ///
    /// **Contract enforcement**: every derived destructor MUST call
    /// @ref shutdown_ as its first statement. If the base destructor
    /// observes the flag unset it calls `std::abort()` with a clear
    /// diagnostic rather than silently papering over the bug.
    virtual void shutdown_() noexcept;

    // ── Queries (called from main thread) ────────────────────────────────

    [[nodiscard]] bool is_running()     const noexcept { return core_.is_running(); }
    [[nodiscard]] bool script_load_ok() const noexcept { return core_.is_script_load_ok(); }
    [[nodiscard]] const config::RoleConfig &config()   const noexcept { return config_; }
    [[nodiscard]] std::string_view          role_tag() const noexcept { return role_tag_; }

    /// Block until wakeup (shutdown, incoming message, or timeout).
    void wait_for_wakeup(int timeout_ms) { core_.wait_for_incoming(timeout_ms); }

  protected:
    // ── Accessors for derived class hooks ────────────────────────────────

    RoleHostCore         &core()          noexcept { return core_; }
    const RoleHostCore   &core()    const noexcept { return core_; }
    ScriptEngine         &engine()        noexcept { return *engine_; }
    RoleAPIBase          &api()           noexcept { return *api_; }
    bool                  has_api() const noexcept { return api_ != nullptr; }

    /// Valid only while @ref startup_ is in progress or the worker is
    /// active. Derived classes set the result from the worker thread
    /// before exiting the startup path (success or failure).
    ///
    /// For custom worker threads, call @c api().thread_manager().spawn(...)
    /// directly — no short-form accessor is provided.
    std::promise<bool>   &ready_promise() noexcept { return ready_promise_; }

    // ── Virtual hook (derived must implement) ────────────────────────────

    /// Worker thread entry point. Runs the full engine + data-loop
    /// lifecycle (schema resolve → infra → engine_lifecycle_startup →
    /// on_init → broker register → ready_promise_.set_value → data loop →
    /// on_stop → finalize → teardown → thread_manager().drain()).
    ///
    /// MUST call `ready_promise().set_value(ok)` on every path out of the
    /// startup phase (success or failure) or the parent thread will block
    /// forever in @ref startup_.
    virtual void worker_main_() = 0;

  private:
    std::string                    role_tag_;
    config::RoleConfig             config_;
    std::unique_ptr<ScriptEngine>  engine_;
    RoleHostCore                   core_;

    // Constructed in startup_() so the worker thread can live under this
    // api's ThreadManager. Dropped in shutdown_() → ThreadManager dtor
    // bounded-joins every role-scope thread.
    std::unique_ptr<RoleAPIBase>   api_;

    std::promise<bool>             ready_promise_;

    // Contract-enforcement flag. Set by the first effective call to
    // @ref shutdown_ via a CAS exchange. Read by the destructor to
    // detect missing-shutdown bugs. Atomic because @ref shutdown_ may
    // be invoked from a signal handler / other thread before the dtor
    // runs on the owning thread.
    std::atomic<bool>              shutdown_called_{false};
};

} // namespace pylabhub::scripting
