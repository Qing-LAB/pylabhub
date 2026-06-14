#pragma once
/**
 * @file zap_router.hpp
 * @brief Process-wide ZAP handler — caller-pumped, no internal thread.
 *
 * **Design contract (HEP-CORE-0036 §7.1 + design doc §5.2).**  ZAP
 * (RFC 27) requires one REP socket bound at `inproc://zeromq.zap.01`
 * per `zmq::context_t` to answer libzmq's CURVE-handshake admission
 * RPCs.  This class owns that socket + a `DomainRoutingTable` (a
 * reusable utility that encapsulates the zap_domain → PeerAdmission
 * map + shared_mutex + lock-bounded callback contract — see
 * `utils/security/domain_routing_table.hpp`).  It does NOT own a
 * thread.  The
 * caller pumps `pump_one(timeout)` from an existing event loop —
 * matching the locked HEP's prescription that the handler "runs on
 * the BRC poll thread" (for `plh_role`) or the broker main poll
 * thread (for `plh_hub`).
 *
 * **Lifecycle (HEP-CORE-0001 hybrid model).**  Registered as a
 * persistent dynamic LifecycleManager module on first
 * `register_domain` call.  Startup binds the inproc REP socket.
 * Shutdown closes it.  Persistent flag avoids reload churn across
 * ref_count==0 transitions; module unloads only at LifecycleGuard
 * finalize, in reverse-topo order before ZMQContext teardown.
 *
 * Critically: **startup does not spawn any thread.**  The first cut
 * of this module deadlocked because creating a `ThreadManager` inside
 * the startup callback re-enters `LifecycleManager::register_dynamic_module`
 * from within a locked startup chain — exactly what
 * HEP-CORE-0001's `RecursionGuard` prohibits.  The corrected design
 * has no such re-entrancy because it has no thread to manage.
 *
 * **Threading model.**
 *   - `register_domain` / `unregister_domain_` — any thread; serialized
 *     by `std::shared_mutex` on the routing table.
 *   - `pump_one` — exactly ONE thread per process at a time, enforced
 *     at runtime by the atomic-counter PANIC documented under
 *     "Runtime-enforced invariants" (3) below.  libzmq requires
 *     single-thread access to the inproc REP socket; concurrent
 *     pumpers would silently corrupt its FSM.  Production wires
 *     this on the broker's main poll thread (plh_hub) and the BRC
 *     dispatch thread (plh_role, post-AUTH-2 #162); tests/demos use
 *     `ZapPumpThread` (a tiny RAII helper, defined alongside).
 *
 * **Runtime-enforced invariants (task #215).**  The "exactly ONE
 * pumper" contract and the admission-pointer lifetime are enforced
 * at the call sites, not just documented:
 *
 *   1. `pump_one` holds `registered_mu` in shared mode across the
 *      `admission->is_peer_allowed(...)` call.  `~ZapDomainHandle`
 *      takes the same mutex in unique mode, so the destructor
 *      blocks until any in-flight admission decision completes —
 *      closes the UAF window between the lookup and the call that
 *      fires the moment AUTH-2 (#162) puts pump_one on the BRC
 *      poll thread.
 *   2. `pump_one` pushes a `RecursionGuard` keyed to the router
 *      instance (per `recursion_guard.hpp` — thread-local, RAII).
 *      `register_domain` refuses + logs + returns an inactive handle
 *      when the same thread is already inside an admission decision;
 *      `unregister_domain_` PLH_PANICs (a destructor can't react to
 *      a refusal — leaving a dangling map entry would UAF the next
 *      pump).  See `peer_admission.hpp::is_peer_allowed` reentrance
 *      contract.
 *   3. `pump_one` increments an atomic counter on entry; a second
 *      concurrent entry PLH_PANICs with the post-increment count.
 *      Catches accidental two-pumper configurations (e.g. a
 *      `ZapPumpThread` left in scope while the BRC pump is also
 *      running) before they corrupt the REP socket FSM.
 *
 * **Failure mode — "registered but unpumped" (design §7.4).**  If a
 * binary calls `register_domain` and never pumps, every CURVE
 * handshake on any socket using that `zap_domain` hangs at libzmq.
 * There is NO watchdog.  Prevention is contractual via Phase D
 * integration tests and `ZapPumpThread` RAII for tests/demos.
 *
 * @see HEP-CORE-0001 (lifecycle hybrid model, persistent dynamic
 *      pattern; same shape as `SignalHandler`)
 * @see HEP-CORE-0036 §7.1 (handler runs on BRC poll thread)
 * @see docs/archive/transient-2026-06-02/peer_admission_architecture_design.md §5.2 + §7.2 + §7.4
 * @see RFC 27 (ZAP) — https://rfc.zeromq.org/spec/27/
 */

#include "pylabhub_utils_export.h"
#include "utils/module_def.hpp"  // ModuleDef
#include "utils/security/peer_admission.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace pylabhub::utils::security
{

class ZapDomainHandle;  // RAII handle, defined below.

/// Process-wide ZAP handler singleton.  Lazily registers itself as a
/// persistent dynamic LifecycleManager module on first
/// `register_domain`; passive otherwise.  Not constructible by
/// callers — use `ZapRouter::instance()`.
class PYLABHUB_UTILS_EXPORT ZapRouter
{
public:
    /// Process-wide accessor.  Idempotent; the underlying state is
    /// lazily constructed on first call but the LifecycleManager
    /// dynamic module is not registered until `register_domain` runs.
    static ZapRouter &instance();

    /// Module name as registered with LifecycleManager.  Exposed for
    /// test fixtures that want to query module state.  Stable for
    /// the life of the process.
    static const char *module_name() noexcept;

    /// Register @p admission as the PeerAdmission for ZAP requests
    /// carrying `zap_domain == @p domain`.  Thread-safe (internal
    /// mutex serializes mutations of the routing table).
    ///
    /// First call in the process registers `ZapRouter` with
    /// `LifecycleManager` as a persistent dynamic module + invokes
    /// `LoadModule` to bind the inproc REP socket.  Subsequent calls
    /// increment the LifecycleManager ref-count via additional
    /// `LoadModule` calls.  Persistent flag keeps the socket bound
    /// across ref_count==0 transitions; the only unload happens at
    /// LifecycleGuard finalize.
    ///
    /// Returns an RAII handle whose destructor calls `UnloadModule`
    /// + removes the domain from the routing table.
    ///
    /// Throws std::runtime_error if @p domain is already registered
    /// (per-domain uniqueness — a regression that double-registers
    /// must surface) or if @p domain is empty.  The non-null contract
    /// on `admission` is enforced by the reference type — callers
    /// cannot pass a null admission.
    [[nodiscard]] ZapDomainHandle
    register_domain(std::string domain, PeerAdmission &admission);

    /// **Pump one ZAP request from the inproc REP socket.**
    ///
    /// Called by the binary's existing event-loop thread.  Caller is
    /// responsible for ensuring at most ONE thread calls this at a
    /// time (libzmq enforces single-thread socket access; concurrent
    /// callers will surface as `EAGAIN`/`ETERM` errors logged as
    /// WARN and treated as no-ops).
    ///
    /// @param timeout `0ms` → non-blocking (return immediately if no
    ///   request is pending).  Otherwise wait up to @p timeout.
    /// @return `true` iff a request was successfully received and
    ///   replied to (whether ALLOW or DENY); `false` on timeout or
    ///   on the module-not-loaded path (caller registered nothing yet).
    ///
    /// Implementation: recv multipart, parse ZAP request frames per
    /// RFC 27, dispatch to the `DomainRoutingTable::with_admission`
    /// callback (which holds shared-lock + reentrance guard across
    /// the admission call), and `is_peer_allowed(PeerIdentity{"curve",
    /// z85(pubkey)})` returns allow/deny, then send 200 (allow) or
    /// 400 (deny) reply with the peer pubkey as `user_id`.
    [[nodiscard]] bool pump_one(std::chrono::milliseconds timeout);

    /// For tests: count of currently-registered domains.  Returns 0
    /// when the module is unloaded (i.e., not yet first-registered
    /// in this process).
    [[nodiscard]] std::size_t registered_domain_count_for_test() const;

    /// Observability — count of ZAP requests this `ZapRouter` has
    /// served with an ALLOW reply (status "200") since the module was
    /// loaded.  Lifetime-cumulative; resets only on module shutdown.
    /// Used by tests to pin that the allow PATH actually executed
    /// (vs. a delivery succeeding by coincidence); operators may also
    /// monitor it to verify CURVE handshakes are flowing.
    [[nodiscard]] std::uint64_t allowed_count() const noexcept;

    /// Observability — count of ZAP requests this `ZapRouter` has
    /// served with a DENY reply (status "400" — any of: malformed
    /// request, bad version, unsupported mechanism, bad credentials,
    /// unknown domain, peer not in allowlist).  Used by tests to pin
    /// that a deny PATH executed (vs. a timeout where nothing
    /// happened).  Lifetime-cumulative; resets only on module shutdown.
    [[nodiscard]] std::uint64_t denied_count() const noexcept;

    ZapRouter(const ZapRouter &)            = delete;
    ZapRouter &operator=(const ZapRouter &) = delete;
    ZapRouter(ZapRouter &&)                 = delete;
    ZapRouter &operator=(ZapRouter &&)      = delete;

private:
    ZapRouter();
    ~ZapRouter();

    friend class ZapDomainHandle;
    void unregister_domain_(const std::string &domain);

    static pylabhub::utils::ModuleDef make_module_def_();
    static void                       lifecycle_startup_thunk(const char *,
                                                               void *);
    static void                       lifecycle_shutdown_thunk(const char *,
                                                                void *);
    void                              on_module_startup_();
    void                              on_module_shutdown_();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// RAII registration handle returned by `ZapRouter::register_domain`.
/// Destructor removes the domain from the routing table AND calls
/// `UnloadModule` to decrement the LifecycleManager ref-count.
/// Movable; non-copyable.  Default-constructed handles are inactive
/// (post-move sentinel).
class PYLABHUB_UTILS_EXPORT ZapDomainHandle
{
public:
    /// Default-constructed: not registered to anything.
    ZapDomainHandle() = default;

    /// Movable; the source handle's destructor becomes a no-op.
    ZapDomainHandle(ZapDomainHandle &&other) noexcept;
    ZapDomainHandle &operator=(ZapDomainHandle &&other) noexcept;

    /// Destructor unregisters + UnloadModule.  No-op for default /
    /// moved-from handles.
    ~ZapDomainHandle();

    ZapDomainHandle(const ZapDomainHandle &)            = delete;
    ZapDomainHandle &operator=(const ZapDomainHandle &) = delete;

    /// True iff this handle currently owns a registration.
    [[nodiscard]] bool is_active() const noexcept
    {
        return router_ != nullptr && !domain_.empty();
    }

    [[nodiscard]] const std::string &domain() const noexcept
    {
        return domain_;
    }

private:
    friend class ZapRouter;
    ZapDomainHandle(ZapRouter *router, std::string domain) noexcept
        : router_(router), domain_(std::move(domain)) {}

    ZapRouter   *router_{nullptr};
    std::string  domain_;
};

/// RAII helper for tests + demos that don't already have a long-
/// running event loop.  Spawns a single `std::jthread` that loops
/// `ZapRouter::instance().pump_one(tick)` until the destructor's
/// `stop_token` is requested (`std::jthread`'s destructor handles
/// the stop + join automatically).  Note: the pump body blocks in
/// `recv_multipart` for up to `tick` per iteration, so worst-case
/// teardown wait is one `tick` interval (100 ms by default) before
/// the loop top observes the stop request.
///
/// **`ZapPumpThread` is intentionally NOT registered with a
/// `ThreadManager`** (see `utils/thread_manager.hpp`).  It's a
/// test/demo helper whose lifetime is bounded by the enclosing scope
/// (a subprocess in Pattern-3 tests, a `main()` frame in a demo);
/// bounded-shutdown + per-thread diagnostics are not load-bearing
/// for this use case.  **Production threads that host
/// `pump_one`** — e.g. the BRC poll thread post-AUTH-2 (#162), the
/// broker's main poll thread — MUST be `ThreadManager`-spawned for
/// bounded shutdown + diagnostics + lifecycle ordering.  The
/// `jthread` body shape adopted here is itself a valid pattern for
/// future "loop until stop" threads inside a `ThreadManager` slot;
/// see HEP-CORE-0031 §4.1 for how `SlotContext::shutdown_requested()`
/// composes with the cooperative `stop_token` idiom.
///
/// Production binaries (`plh_hub`, `plh_role`) do NOT use this —
/// they integrate `pump_one` into their existing main event loops
/// (Phase D).  Using both at once would violate the single-pumping-
/// thread invariant.
///
/// Construct AFTER `LifecycleGuard` is up AND at least one
/// `register_domain` call has been made (so the inproc REP socket
/// exists).  Destroy BEFORE `LifecycleGuard` exits its scope so the
/// pump thread joins while the ZMQ context is still alive.
class PYLABHUB_UTILS_EXPORT ZapPumpThread
{
public:
    /// @param tick How long each `pump_one` call waits for a ZAP
    ///   request before returning to the loop top to check the stop
    ///   flag.  100ms is plenty for any realistic peer-connect rate;
    ///   short enough that test teardown joins quickly.
    explicit ZapPumpThread(
        std::chrono::milliseconds tick = std::chrono::milliseconds(100));

    /// Stops the loop and joins the thread.  Idempotent.
    ~ZapPumpThread();

    ZapPumpThread(const ZapPumpThread &)            = delete;
    ZapPumpThread &operator=(const ZapPumpThread &) = delete;
    ZapPumpThread(ZapPumpThread &&)                 = delete;
    ZapPumpThread &operator=(ZapPumpThread &&)      = delete;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pylabhub::utils::security
