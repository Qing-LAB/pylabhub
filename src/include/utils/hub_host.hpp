#pragma once
/**
 * @file hub_host.hpp
 * @brief HubHost — top-level lifecycle owner for the hub binary.
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
 *   - On shutdown, the flag is flipped, subsystem `stop()` methods
 *     break their poll loops, ThreadManager destructor joins all
 *     tracked threads with a bounded timeout.
 *
 * `HubHost` is non-copyable, non-movable, single-instance per hub
 * binary.  Use one of the factory-style sequences:
 *   HubHost host(std::move(cfg));
 *   host.startup();
 *   host.run_main_loop();   // blocks
 *   host.shutdown();        // optional; dtor runs it idempotently
 */

#include "pylabhub_utils_export.h"

#include <memory>
#include <string>

namespace pylabhub::config { class HubConfig; }
namespace pylabhub::broker { class BrokerService; }
namespace pylabhub::hub    { class HubState; }

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

    // ── Lifecycle ──────────────────────────────────────────────────

    /// Build subsystems and spawn their threads via the owned
    /// ThreadManager.  Idempotent: a second call after success is a
    /// no-op; throws on actual construction failure.
    void startup();

    /// Block on the shutdown atomic until `request_shutdown()` /
    /// `shutdown()` is called.  Typically the main thread's last call.
    void run_main_loop();

    /// Stop subsystems (broker.stop()), flip the shutdown flag, wake
    /// `run_main_loop()`.  Idempotent; threads are joined when
    /// `~HubHost` runs (ThreadManager destructor with bounded join).
    void shutdown();

    /// Thread-safe shutdown signal — flips the atomic + notifies the
    /// run-loop's condition variable.  Safe from any thread (admin
    /// RPC handler, signal handler, broker error path).
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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pylabhub::hub_host
