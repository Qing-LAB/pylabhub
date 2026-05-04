#pragma once
/**
 * @file admin_service.hpp
 * @brief AdminService — structured RPC entry point for hub administration.
 *
 * Owns a ZMQ REP socket bound to `admin.endpoint` and serves a JSON
 * envelope `{method, token, params}` → `{status, result|error}` per
 * HEP-CORE-0033 §11.  Replaces the legacy Python-eval `AdminShell`
 * with a typed, token-validated method surface.
 *
 * Phase 6.2a (this header) ships the **skeleton only** — REP socket,
 * run loop, token gate, localhost-bind enforcement, ThreadManager-
 * compatible run/stop pair, and a single built-in `ping` method that
 * proves the round-trip.  All §11.2 query/control methods return
 * `{"status":"error", "error":{"code":"not_implemented"}}` until
 * Phase 6.2b/6.2c fill them in.
 *
 * Thread model:
 *   - Constructed by `HubHost` after `BrokerService` is up.
 *   - `run()` is invoked on a thread spawned via the host's
 *     ThreadManager (named `"admin"`).  It binds the REP socket and
 *     polls until `stop()` flips the internal atomic.
 *   - `stop()` is safe from any thread; idempotent.
 *
 * Authorization:
 *   - `admin.token_required: true` → request `"token"` must equal the
 *     vault's admin token (`HubVault::admin_token()`, plumbed in via
 *     `HubAdminConfig::admin_token`).  Mismatch →
 *     `{"status":"error", "error":{"code":"unauthorized"}}`.
 *   - `admin.token_required: false` → token field ignored, BUT the
 *     endpoint MUST resolve to `127.0.0.1` (or `localhost`).  This is
 *     enforced at construction time; non-loopback + token_required==
 *     false throws `std::invalid_argument`.
 *
 * Lifetime invariant:
 *   - `AdminService` borrows the ZMQ context (caller-owned, typically
 *     the process-wide `ZMQContext` LifecycleGuard module).
 *   - Holds a non-owning reference to `HubHost`; the host outlives
 *     the service per HEP-0033 §4.2 (admin stops at step 3, host
 *     destruction is much later).
 *
 * Files:
 *   - Header: `src/include/utils/admin_service.hpp` (this file).
 *   - Impl:   `src/utils/ipc/admin_service.cpp` (groups with
 *             `broker_service.cpp` — both are REP-socket-owning hub
 *             subsystems; see HEP-0033 §11.4).
 */

#include "pylabhub_utils_export.h"

#include <memory>
#include <string>
#include <string_view>

namespace zmq { class context_t; }

namespace pylabhub::config { struct HubAdminConfig; }
namespace pylabhub::hub_host { class HubHost; }

namespace pylabhub::admin
{

class PYLABHUB_UTILS_EXPORT AdminService
{
public:
    /// Construct around an already-loaded HubAdminConfig.  Validates
    /// the localhost-bind invariant when `token_required==false` (§11.3);
    /// throws `std::invalid_argument` on violation.  Does NOT bind the
    /// socket — that happens at the head of `run()` so bind failures
    /// are reported on the admin worker thread (consistent with
    /// `BrokerService::run()`).
    ///
    /// @param zmq_ctx       Process-wide ZMQ context (caller-owned;
    ///                      AdminService stores a non-owning pointer).
    /// @param cfg           Admin sub-config.  AdminService captures
    ///                      values it needs (endpoint, token, gates)
    ///                      at construction; subsequent edits to
    ///                      `cfg` are not observed.
    /// @param admin_token   64-hex admin token from the unlocked
    ///                      `HubVault` (already plumbed via
    ///                      `HubAdminConfig::admin_token`; passed
    ///                      explicitly for clarity at the boundary).
    /// @param host          Backref for method dispatchers.  Borrowed.
    AdminService(zmq::context_t          &zmq_ctx,
                 const config::HubAdminConfig &cfg,
                 std::string_view         admin_token,
                 hub_host::HubHost       &host);

    ~AdminService();

    AdminService(const AdminService &)            = delete;
    AdminService &operator=(const AdminService &) = delete;
    AdminService(AdminService &&)                 = delete;
    AdminService &operator=(AdminService &&)      = delete;

    /// Bind REP socket and serve until `stop()`.  Spawned by HubHost
    /// on a dedicated `"admin"` thread.  Throws on bind failure.
    void run();

    /// Async stop — safe from any thread.  Flips the internal atomic;
    /// the admin thread's poll loop exits on its next iteration
    /// (`kAdminPollIntervalMs`).  Idempotent.
    void stop() noexcept;

    /// Endpoint the REP socket is actually bound to (after `run()`
    /// has bound it; may differ from configured endpoint when an
    /// ephemeral port was requested).  Empty before bind.
    [[nodiscard]] const std::string &bound_endpoint() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pylabhub::admin
