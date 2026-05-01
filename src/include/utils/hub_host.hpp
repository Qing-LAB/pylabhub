#pragma once
/**
 * @file hub_host.hpp
 * @brief HubHost — top-level start/stop owner for the hub binary.
 *        Not a `LifecycleGuard` module; the binary's `LifecycleGuard`
 *        manages independent process-wide modules (Logger, ZMQContext,
 *        …) and must be active before HubHost is constructed.
 *
 * Per HEP-CORE-0033 §4, HubHost is a single concrete class (not a
 * template hierarchy — hubs are singletons, no polymorphism to abstract
 * over) that owns the hub's subsystems and sequences their start /
 * shutdown.  Owned subsystems:
 *
 *   - HubState           (value member; HEP-0033 §8)
 *   - BrokerService      (unique_ptr; HEP-0033 §9)
 *   - AdminService       (unique_ptr; HEP-0033 §11 — Phase 6.2)
 *   - ScriptEngine       (unique_ptr; HEP-0033 §12 — Phase 7, optional)
 *   - ThreadManager      (unique_ptr; auto-registers as the dynamic
 *                         lifecycle module "ThreadManager:HubHost:<uid>")
 *
 * HubHost has **no state-changing API of its own**.  Per HEP-0033 §11.3
 * + §12.3, all state changes go through `BrokerService`'s methods.
 * AdminService validates admin-side acceptance at its RPC entry point;
 * HubAPI gates script-side acceptance at its boundary; both then call
 * `broker.request_*` directly.  HubHost just exposes accessors so
 * subsystem wiring (admin RPC handlers, future script bindings) can
 * reach the broker and read-only HubState.
 *
 * Thread model:
 *   - Main thread runs `run_main_loop()` which blocks on the shutdown
 *     atomic via a condition variable.
 *   - Broker thread is spawned via the owned ThreadManager (named
 *     "broker"); runs `BrokerService::run()`.
 *   - Admin thread (Phase 6.2) is spawned via the same ThreadManager
 *     (named "admin"); runs `AdminService::run()`.
 *   - `shutdown()` is synchronous: it flips the shutdown flag, calls
 *     each subsystem's `stop()` to break its poll loop, then drains
 *     the ThreadManager so all tracked threads have actually exited
 *     before the call returns.  After `shutdown()`, no protocol
 *     traffic is being processed and no in-flight handlers can run.
 *   - `~HubHost` calls `shutdown()` if the caller didn't, then
 *     destroys subsystems in reverse spawn order (thread_mgr → broker
 *     → state → cfg).
 *
 * Preconditions for use:
 *   - A `LifecycleGuard` providing Logger, FileLock, JsonConfig,
 *     CryptoUtils, and ZMQContext modules MUST be active before
 *     `startup()` is called.  HubHost itself is **not** a
 *     `LifecycleGuard` module — it is an application-level
 *     start/stop owner, like `RoleHostBase` on the role side.  Its
 *     `startup()` / `shutdown()` are called directly by `main`, not
 *     dispatched by `LifecycleGuard`.
 *   - `cfg.load_keypair(password)` MUST be called by the caller
 *     BEFORE constructing HubHost if CURVE auth is desired (vault
 *     unlock is a config-time concern; HubHost reads the resulting
 *     pubkey/seckey from `cfg.auth()`).
 *   - Single-driver assumption: only one thread should call
 *     `startup()` / `shutdown()` / `run_main_loop()` per HubHost
 *     instance.  These methods are idempotent w.r.t. repeated calls
 *     from the SAME thread, but not safe under concurrent calls from
 *     different threads.  `request_shutdown()` is the one exception —
 *     it is safe from any thread (signal handler, admin RPC, broker
 *     error path).
 *
 * `HubHost` is non-copyable, non-movable, single-instance per hub
 * binary.  Use one of the factory-style sequences:
 *   HubHost host(std::move(cfg));
 *   host.startup();
 *   host.run_main_loop();   // blocks until request_shutdown / shutdown
 *   host.shutdown();        // optional; dtor runs it idempotently
 */

#include "pylabhub_utils_export.h"

#include <memory>
#include <string>

namespace pylabhub::config { class HubConfig; }
namespace pylabhub::broker { class BrokerService; }
namespace pylabhub::hub    { class HubState; }
namespace pylabhub::admin  { class AdminService; }

namespace pylabhub::hub_host
{

class PYLABHUB_UTILS_EXPORT HubHost
{
public:
    /// Construct around an already-loaded HubConfig.  The caller is
    /// responsible for unlocking the vault if CURVE auth is desired
    /// (call `cfg.load_keypair(password)` before passing the cfg here).
    /// HubHost inspects `cfg.auth().client_pubkey` at startup to decide
    /// whether to enable CURVE on the broker.
    explicit HubHost(config::HubConfig cfg);

    ~HubHost();

    HubHost(const HubHost &)            = delete;
    HubHost &operator=(const HubHost &) = delete;
    HubHost(HubHost &&)                 = delete;
    HubHost &operator=(HubHost &&)      = delete;

    // ── Start / Stop  (not a LifecycleGuard module) ───────────────

    /// Build subsystems and spawn their threads via the owned
    /// ThreadManager.  Idempotent: a second call after success is a
    /// no-op; throws on actual construction failure.
    void startup();

    /// Block on the shutdown atomic until `request_shutdown()` /
    /// `shutdown()` is called.  Typically the main thread's last call.
    void run_main_loop();

    /// Synchronous shutdown.  Flips the shutdown flag, calls
    /// `broker.stop()` (and, in Phase 6.2, `admin.stop()`), then
    /// drains the ThreadManager so the broker thread has actually
    /// exited before this returns.  Idempotent: a second call is a
    /// no-op.  After `shutdown()` returns: `is_running()` is false,
    /// `broker()` is unsafe to call, no protocol traffic is being
    /// served, and the broker socket is closed.
    void shutdown();

    /// Async shutdown signal — safe from any thread.  Flips the
    /// shutdown flag, calls `broker.stop()` to break its poll loop,
    /// and notifies `run_main_loop()`'s condition variable.  Returns
    /// immediately; ThreadManager join is deferred to a subsequent
    /// `shutdown()` call (or to `~HubHost`).  Typical use: signal
    /// handler / admin RPC / broker error path → call this →
    /// `run_main_loop()` returns on the main thread → main thread
    /// drives `shutdown()` for synchronous wind-down.
    void request_shutdown() noexcept;

    /// True between successful `startup()` and the start of `shutdown()`.
    [[nodiscard]] bool is_running() const noexcept;

    // ── Subsystem accessors (non-owning) ───────────────────────────

    [[nodiscard]] const config::HubConfig &config() const noexcept;

    /// The broker.  Only valid between `startup()` and `shutdown()`;
    /// dereference outside that window is undefined behaviour.
    [[nodiscard]] broker::BrokerService &broker() noexcept;

    /// HubState — read-only public surface.  State changes go through
    /// `broker()`'s methods; const reference here makes that explicit
    /// at compile time.
    [[nodiscard]] const hub::HubState &state() const noexcept;

    /// Bound broker endpoint (e.g. `tcp://127.0.0.1:5570`).  Empty
    /// before `startup()` returns.  After successful startup, this
    /// reflects the actual bound address — useful when the configured
    /// endpoint requested an ephemeral port (`tcp://127.0.0.1:0`).
    /// Survives `shutdown()` for diagnostic/log purposes.
    [[nodiscard]] const std::string &broker_endpoint() const noexcept;

    /// Broker's CURVE public key (Z85, 40 chars), or empty if CURVE is
    /// disabled.  Same lifetime semantics as `broker_endpoint()`.
    [[nodiscard]] const std::string &broker_pubkey() const noexcept;

    /// AdminService pointer, or nullptr if `admin.enabled=false` in
    /// the config (or before `startup()` / after `shutdown()`).  When
    /// non-null, only valid between `startup()` and `shutdown()` —
    /// dereference outside that window is undefined behaviour.  Used
    /// primarily by tests to check the bound endpoint and round-trip
    /// the REP socket.  Production code rarely touches this directly;
    /// AdminService dispatches RPCs into HubHost via its own backref.
    [[nodiscard]] admin::AdminService *admin() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pylabhub::hub_host
