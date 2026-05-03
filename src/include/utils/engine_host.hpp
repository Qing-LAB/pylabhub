#pragma once
/**
 * @file engine_host.hpp
 * @brief EngineHost<ApiT> — template base for script-hosting start/stop.
 *
 * Not a `LifecycleGuard` module — the host's own start/stop is invoked
 * directly by the binary's main (or by tests), distinct from the
 * process-wide `LifecycleGuard` modules (Logger, ZMQContext, …) that
 * must be active before any EngineHost is constructed.
 *
 * (Renamed from `role_host_base.hpp` on 2026-04-23 alongside the
 * HEP-CORE-0033 G1 refactor that promoted the former `RoleHostBase`
 * class to a template.  The type alias `RoleHostBase =
 * EngineHost<RoleAPIBase>` at the bottom of this header preserves
 * source-level compatibility for role-side callers.)
 *
 * Unifies the public start/stop surface that binaries (`plh_role`,
 * future `plh_hub`) call on a script host:
 *
 *     construct → startup_() → (is_running()/wait_for_wakeup() loop) → shutdown_()
 *
 * The trailing-underscore methods follow the project convention: they are
 * intended to be called only by code tightly coupled with the role/hub
 * module (binary mains, not generic external consumers).
 *
 * The class is a template parameterised on `ApiT`, the script-visible API
 * class.  Two instantiations exist:
 *
 *     using RoleHostBase        = EngineHost<RoleAPIBase>;   // role side ✅
 *     using HubScriptRunnerBase = EngineHost<HubAPI>;        // hub side (Phase 7)
 *
 * Three role-side concrete classes derive from `RoleHostBase`
 * (Producer/Consumer/Processor); one hub-side concrete class derives
 * from `HubScriptRunnerBase` (`HubScriptRunner`).  Each implements a
 * single pure virtual hook, @ref worker_main_, which runs the full
 * engine-and-loop sequence on the host's worker thread.
 *
 * Note (per §G1 retraction below): the OUTER hub container `HubHost`
 * is a plain concrete class — NOT derived from `EngineHost`.  The
 * `EngineHost<HubAPI>` instantiation is for the inner script-thread
 * runtime (`HubScriptRunner`), which is one of `HubHost`'s owned
 * subsystems.
 *
 * Base owns shared state (role_tag, config, engine, RoleHostCore, ApiT,
 * ready-promise). Derived owns role/hub-specific state (schemas, queues,
 * BrokerRequestComm, InboxQueue, CycleOps for role side; admin/event
 * pumps for hub side). The @ref ApiT is constructed lazily in @ref
 * startup_ so the worker thread can be spawned via its ThreadManager —
 * putting it under the same bounded-join / ERROR-on-timeout contract as
 * every other role- or hub-scope thread.
 *
 * Contract on `ApiT`:
 *   - Constructor `ApiT(RoleHostCore &, std::string role_tag, std::string uid)`
 *   - `ThreadManager &thread_manager()` accessor
 *   (HubAPI will satisfy both when HEP-0033 Phase 8 lands.)
 *
 * New hosts add custom worker threads by calling @c api().thread_manager()
 * @c .spawn(...) inside their @ref worker_main_; all such threads are
 * drained by `ThreadManager::~ThreadManager` during @ref shutdown_.
 *
 * Thread safety: construct, startup(), shutdown() on one thread (usually
 * main). is_running()/script_load_ok()/wait_for_wakeup() may be called
 * from the same thread while the worker runs. Not copyable, not movable
 * (owns an internal worker thread).
 *
 * Design rationale (retraction scope): the original §G1 prereqs proposal
 * had a `HubHostBase = EngineHost<HubAPI>` typedef that `HubHost` itself
 * would derive from.  That OUTER-CONTAINER unification was retracted —
 * hubs are singletons (one binary kind, one config, no dispatch over
 * multiple hub kinds), so there is no polymorphism to abstract over at
 * the HubHost layer; `HubHost` is a single concrete class owning its
 * subsystems directly.  The retraction does NOT touch the inner
 * script-thread runtime: `HubScriptRunner` (Phase 7) derives from
 * `EngineHost<HubAPI>` for code reuse — getting the phase FSM, thread
 * spawn, ready promise, and RAII shutdown machinery.  See HEP-CORE-0033
 * §4 for HubHost layering and §15 Phase 7 for the runner.
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

// ============================================================================
// script_host_traits<ApiT> — per-ApiT trait carrier
// ============================================================================
//
// Decouples `EngineHost<ApiT>` from a hardcoded config type so the same
// template body works for the role-side instantiation (`ConfigT =
// RoleConfig` — the role binary owns its config) and the hub-side
// instantiation (`ConfigT = std::monostate` — HubHost owns HubConfig;
// the runner reads through the host backref, see HEP-CORE-0033 §4 +
// Phase 7 Option E).  Pure compile-time aliasing.
//
// Trait contract:
//   - `using ConfigT = ...`               — the per-ApiT config type.
//   - `static std::string uid_from_config(const ConfigT &c)`
//                                         — extracts the host instance
//     uid from the moved-in config.  EngineHost calls this once in its
//     ctor body, AFTER the move into `config_`.  Role-side returns
//     `c.identity().uid`.  Hub-side returns "" — HubScriptRunner's
//     ctor body then calls `set_uid(host_.config().identity().uid)`
//     to populate the real uid before startup_().
//
// Adding a new ApiT:
//   1. Define ApiT.
//   2. Specialize `script_host_traits<NewApiT>` with its ConfigT and
//      `uid_from_config()` extractor (use `std::monostate` ConfigT +
//      empty-string uid if the new host is a subsystem; the outer
//      container's derived class then sets the real uid via setter).
//   3. Add `template class EngineHost<NewApiT>;` to engine_host.cpp.
//   4. Hub-side specializations live in `hub_api.hpp` (alongside the
//      ApiT class) so engine_host.hpp doesn't need to include hub
//      headers that role-only TUs would never use.
template <typename ApiT> struct script_host_traits;

template <> struct script_host_traits<RoleAPIBase>
{
    using ConfigT = config::RoleConfig;
    static std::string uid_from_config(const ConfigT &c)
    {
        return c.identity().uid;
    }
};

template <typename ApiT>
class PYLABHUB_UTILS_EXPORT EngineHost
{
  public:
    /// Convenience alias for the per-ApiT config type carried by the
    /// trait.  For `EngineHost<RoleAPIBase>` this resolves to
    /// `config::RoleConfig`; for `EngineHost<HubAPI>` (Phase 7 Commit B)
    /// it will resolve to the hub-side config type.
    using ConfigT = typename script_host_traits<ApiT>::ConfigT;

    /// @param role_tag    Short host tag used for ApiT construction
    ///                    and log prefixes (e.g. "prod"/"cons"/"proc"
    ///                    for role hosts, "hub" for the hub host).
    /// @param config      Parsed config (moved into the host).  Type is
    ///                    `script_host_traits<ApiT>::ConfigT`.  For
    ///                    `RoleAPIBase` this is `config::RoleConfig`
    ///                    (the role binary's sole owner of its config).
    ///                    For `HubAPI` (HEP-CORE-0033 Phase 7 Option E)
    ///                    this is `std::monostate` — HubScriptRunner
    ///                    is a SUBSYSTEM under HubHost; HubHost is the
    ///                    sole owner of `HubConfig` and the runner reads
    ///                    fields it needs through the host backref it
    ///                    already holds.
    /// @param engine      Script engine (moved into the host).
    /// @param shutdown_flag  External shutdown signal shared with main;
    ///                       may be nullptr.
    ///
    /// Uid: extracted from @p config POST-move via the trait helper
    /// `script_host_traits<ApiT>::uid_from_config(config_)`.  Role-side
    /// returns `c.identity().uid` directly.  Hub-side (where ConfigT is
    /// `std::monostate`) returns empty — HubScriptRunner sets the real
    /// uid in its ctor body via @ref set_uid using its host backref.
    /// This pattern avoids the C++ argument-evaluation-order pitfall
    /// (`cfg.identity().uid` vs `std::move(cfg)` in the same call are
    /// indeterminately sequenced — g++ chose to move first, leaving a
    /// moved-from cfg for the uid read; D2.1 caught this in the test
    /// suite).  See `script_host_traits` docs above for the contract.
    EngineHost(std::string_view role_tag,
                ConfigT config,
                std::unique_ptr<ScriptEngine> engine,
                std::atomic<bool> *shutdown_flag = nullptr);

    /// Pure virtual — EngineHost is abstract. Every derived class must
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
    /// engine_host.cpp) is required and is always invoked by every
    /// derived destructor's implicit base-destructor call.
    /// The only way to skip this destructor is if `EngineHost`'s own
    /// constructor threw — but in that case no object exists, so there
    /// is nothing to clean up.
    ///
    /// **Contract enforcement**:
    /// The body checks `phase_` and calls `std::abort()` with a clear
    /// diagnostic if the host is destroyed while still in the
    /// `Running` phase.  No silent fallback — programmer error (a missed
    /// @ref shutdown_ call or an override that did not forward to
    /// `EngineHost::shutdown_()`) is surfaced loudly.
    virtual ~EngineHost() = 0;

    EngineHost(const EngineHost &)            = delete;
    EngineHost &operator=(const EngineHost &) = delete;
    EngineHost(EngineHost &&)                 = delete;
    EngineHost &operator=(EngineHost &&)      = delete;

    // ── Configuration ────────────────────────────────────────────────────

    /// Enable validate-only mode (parse + engine load; no broker / no loop).
    /// Must be called before @ref startup.
    void set_validate_only(bool v) { core_.set_validate_only(v); }

  protected:
    /// Override the uid captured by the trait extractor at construction.
    /// Used by hub-side derived (HubScriptRunner) to populate the real
    /// uid from `host_.config()` post base-construction — the trait
    /// returns "" for `ConfigT = std::monostate` because monostate has
    /// no fields to read.  Role-side derived never call this — the
    /// trait extractor produces the right value directly.
    ///
    /// MUST be called before @ref startup_ — uid_ is read during ApiT
    /// construction in startup_'s body.  Calling after startup_ has
    /// begun is a contract violation (uid would not propagate to ApiT,
    /// only to subsequent diagnostics).
    void set_uid(std::string uid) noexcept { uid_ = std::move(uid); }

  public:

    // ── Start / Stop  (not a LifecycleGuard module) ─────────────────────

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
    ///       EngineHost::shutdown_();  // joins worker, sets flag
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
    [[nodiscard]] const ConfigT            &config()   const noexcept { return config_; }
    [[nodiscard]] std::string_view          role_tag() const noexcept { return role_tag_; }

    /// Block until wakeup (shutdown, incoming message, or timeout).
    void wait_for_wakeup(int timeout_ms) { core_.wait_for_incoming(timeout_ms); }

  protected:
    // ── Accessors for derived class hooks ────────────────────────────────

    RoleHostCore         &core()          noexcept { return core_; }
    const RoleHostCore   &core()    const noexcept { return core_; }
    ScriptEngine         &engine()        noexcept { return *engine_; }
    ApiT                 &api()           noexcept { return *api_; }
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
    /// sequence (schema resolve → infra → engine_lifecycle_startup →
    /// on_init → broker register → ready_promise_.set_value → data loop →
    /// on_stop → finalize → teardown → thread_manager().drain()).
    ///
    /// MUST call `ready_promise().set_value(ok)` on every path out of the
    /// startup phase (success or failure) or the parent thread will block
    /// forever in @ref startup_.
    virtual void worker_main_() = 0;

  private:
    std::string                    role_tag_;
    /// Host instance uid — captured at construction (not derived from
    /// `config_`).  Used for diagnostics + as the uid arg to ApiT's
    /// ctor.  Storing it separately decouples EngineHost from any
    /// particular ConfigT shape: hub-side ConfigT is `std::monostate`
    /// (no `.identity().uid`); role-side derived classes extract
    /// `cfg.identity().uid` and pass it explicitly.  See HEP-CORE-0033
    /// Phase 7 Option E (single config owner per layer; subsystems read
    /// through the owner — runner reads HubConfig via host backref).
    std::string                    uid_;
    ConfigT                        config_;
    std::unique_ptr<ScriptEngine>  engine_;
    RoleHostCore                   core_;

    // Constructed in startup_() so the worker thread can live under this
    // api's ThreadManager. Dropped in shutdown_() → ThreadManager dtor
    // bounded-joins every host-scope thread.
    std::unique_ptr<ApiT>          api_;

    std::promise<bool>             ready_promise_;

    /// Start/stop run-state FSM.  Transitions are monotonic:
    /// `Constructed → Running → ShutDown`.  Once `ShutDown`, this
    /// instance is single-use — `startup_()` after `shutdown_()`
    /// panics.  A failed `startup_()` rolls back to `Constructed`
    /// (the throw path resets the phase) so the user may retry after
    /// fixing the underlying problem.
    ///
    /// CAS-based transitions in `startup_` / `shutdown_` make both
    /// methods race-free under concurrent calls; the contract still
    /// expects a single-driver caller, but accidental concurrent
    /// invocations resolve to "first wins, others are no-ops" rather
    /// than corrupting state.
    ///
    /// **Not related to `LifecycleGuard`.**  EngineHost is not a
    /// `LifecycleGuard` module; its start/stop is invoked directly by
    /// the binary's main (or by tests) — `LifecycleGuard` modules
    /// (Logger, FileLock, etc.) are independent and managed
    /// separately.  The name `phase_` is chosen to keep this
    /// distinction visible at every read/write site.
    enum class Phase : uint8_t
    {
        Constructed,
        Running,
        ShutDown,
    };
    std::atomic<Phase>             phase_{Phase::Constructed};
};

// ── Backward-compatible alias for the role-side instantiation ───────────────
//
// `RoleHostBase` was the pre-template class name; it remains the
// role-side reference type.  Three derived classes (ProducerRoleHost,
// ConsumerRoleHost, ProcessorRoleHost) inherit from this alias.  Any
// code still referring to `RoleHostBase` by name resolves to the
// template instantiation `EngineHost<RoleAPIBase>` without change.
//
// Hub side will add a parallel typedef in HEP-CORE-0033 Phase 7
// (Commit B):  `using HubScriptRunnerBase = EngineHost<HubAPI>;`,
// followed by `class HubScriptRunner : public HubScriptRunnerBase`.
// Per the §G1 retraction (lines 59-67 above), this is the SCRIPT-
// thread runtime — the OUTER `HubHost` container remains a plain
// concrete class (not derived from EngineHost).
using RoleHostBase = EngineHost<RoleAPIBase>;

} // namespace pylabhub::scripting
