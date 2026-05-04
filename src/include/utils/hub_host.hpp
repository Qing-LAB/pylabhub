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
 *   - AdminService       (unique_ptr; HEP-0033 §11; built only when
 *                         `cfg.admin().enabled` is true)
 *   - HubScriptRunner    (unique_ptr; HEP-0033 §12 / Phase 7; built
 *                         only when an engine is injected at ctor and
 *                         `cfg.script().path` is non-empty.  Owns the
 *                         injected ScriptEngine + a worker thread for
 *                         event/tick dispatch.)
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
 *   - Main thread runs `run_main_loop()` which polls the shutdown
 *     atomic at 100 ms tick (timed `wait_for` on a condition
 *     variable; flag is the source of truth, checked at loop top).
 *     Mirrors the role-side `run_role_main_loop` pattern.
 *   - Broker thread is spawned via the owned ThreadManager (named
 *     "broker"); runs `BrokerService::run()`.
 *   - Admin thread (when `admin.enabled`) is spawned via the same
 *     ThreadManager (named "admin"); runs `AdminService::run()`.
 *   - HubScriptRunner worker thread (when scripts are enabled) is
 *     spawned via its own owned ThreadManager (inherited from the
 *     EngineHost<HubAPI> instantiation); runs the event/tick loop.
 *   - `shutdown()` is synchronous and ordered per HEP-CORE-0033 §4.2
 *     step 2: stops the runner FIRST (drains in-flight script
 *     callbacks + runs on_stop + joins worker), then admin, then
 *     broker, then drains the host's ThreadManager.  After
 *     `shutdown()`, no protocol traffic is being processed and no
 *     in-flight handlers can run.
 *   - `~HubHost` calls `shutdown()` if the caller didn't, then
 *     destroys subsystems in reverse spawn order (thread_mgr →
 *     runner → admin → broker → state → cfg).
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

namespace pylabhub::config    { class HubConfig; }
namespace pylabhub::broker    { class BrokerService; }
namespace pylabhub::hub       { class HubState; }
namespace pylabhub::admin     { class AdminService; }
namespace pylabhub::scripting { class ScriptEngine;
                                class HubScriptRunner; }

namespace pylabhub::hub_host
{

class PYLABHUB_UTILS_EXPORT HubHost
{
public:
    /// Construct around an already-loaded HubConfig (script-disabled
    /// path — equivalent to `HubHost(std::move(cfg), nullptr)`).  The
    /// caller is responsible for unlocking the vault if CURVE auth is
    /// desired (call `cfg.load_keypair(password)` before passing the
    /// cfg here).  HubHost inspects `cfg.auth().client_pubkey` at
    /// startup to decide whether to enable CURVE on the broker.
    explicit HubHost(config::HubConfig cfg);

    /// Construct around an already-loaded HubConfig with an injected
    /// script engine (HEP-CORE-0033 Phase 7 — Option E).  When @p
    /// engine is non-null, `cfg.script().path` MUST be non-empty and
    /// HubHost::startup() will build a HubScriptRunner that owns the
    /// engine.  When @p engine is null, the runner is not built and
    /// HubHost behaves as in the script-disabled ctor above.
    ///
    /// The engine is INJECTED (not constructed by HubHost) because
    /// engine factories live in `pylabhub-scripting` (Python/Lua),
    /// which is layered ABOVE `pylabhub-utils` where HubHost lives.
    /// Callers (binary mains, test fixtures) build the engine via
    /// `make_engine_from_script_config(cfg.script())` and move it in.
    HubHost(config::HubConfig cfg,
            std::unique_ptr<scripting::ScriptEngine> engine);

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

    /// Synchronous shutdown — ordered per HEP-CORE-0033 §4.2 step 2:
    ///   1. flip the shared shutdown atomic + wake `run_main_loop`
    ///   2. `runner->shutdown_()` (drain script events, run on_stop,
    ///      join the runner's worker thread) when scripts are enabled
    ///   3. `admin->stop()` (close REP socket; in-flight RPCs finish)
    ///   4. `broker->stop()` (break broker poll loop)
    ///   5. `thread_mgr->drain()` (bounded join of broker + admin)
    /// Idempotent: a second call is a no-op.  After `shutdown()`
    /// returns: `is_running()` is false, `broker()` is unsafe to
    /// call, no protocol traffic is being served, no script
    /// callbacks can fire, and all sockets are closed.
    void shutdown();

    /// Async shutdown signal — safe from any thread.  Mirrors the
    /// role-side `RoleHostCore::notify_incoming` shape: flips the
    /// shared shutdown atomic + notifies `run_main_loop`'s CV.
    /// Returns immediately.  Does NOT actively stop subsystems —
    /// the synchronous, ordered teardown happens exclusively in
    /// `shutdown()` on the main thread, preserving HEP-CORE-0033
    /// §4.2 step 2 ordering (runner first, then admin, then broker).
    /// Typical use: signal handler / admin RPC / broker error path
    /// → call this → `run_main_loop()` returns on the main thread →
    /// main thread drives `shutdown()` for synchronous wind-down.
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
