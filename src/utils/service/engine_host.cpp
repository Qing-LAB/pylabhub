/**
 * @file engine_host.cpp
 * @brief EngineHost<ApiT> — shared start/stop scaffolding for script
 *        hosts.  Not a `LifecycleGuard` module: this file owns the
 *        host's own start/stop FSM (Constructed → Running → ShutDown);
 *        `LifecycleGuard` modules (Logger, ZMQContext, …) are
 *        independent and managed by the binary's main.
 *
 * The worker-thread body (`worker_main_`) is host-specific and lives in
 * each derived class; this file owns only the setup/teardown pattern
 * around it — lazy construction of ApiT, worker spawn via the host's
 * own ThreadManager, shutdown contract enforcement.
 *
 * The template is instantiated explicitly at the bottom of this file
 * for each ApiT the project supports.  Today: `RoleAPIBase` only.
 * When HEP-CORE-0033 Phase 8 adds `HubAPI`, a second explicit
 * instantiation lands here alongside the role one.
 */
#include "utils/engine_host.hpp"
#include "utils/debug_info.hpp"   // PLH_PANIC
#include "utils/role_api_base.hpp"
#include "utils/thread_manager.hpp"  // api_->thread_manager() inside startup_

#include <atomic>
#include <typeinfo>   // typeid(ApiT).name() in destructor diagnostic
#include <utility>

namespace pylabhub::scripting
{

template <typename ApiT>
EngineHost<ApiT>::EngineHost(std::string_view role_tag,
                              ConfigT config,
                              std::unique_ptr<ScriptEngine> engine,
                              std::atomic<bool> *shutdown_flag)
    : role_tag_(role_tag)
    , config_(std::move(config))
    , engine_(std::move(engine))
{
    // Extract uid via the per-ApiT trait helper AFTER the move into
    // `config_`.  Reading through the trait + the moved-in `config_`
    // (rather than the source `config` parameter) avoids the C++
    // argument-evaluation-order pitfall where `cfg.identity().uid` and
    // `std::move(cfg)` in the same call site are indeterminately
    // sequenced — g++ chose to move first, leaving uid extraction
    // reading from a moved-from RoleConfig (caught by D2.1 testing).
    //
    // Hub-side specialization returns "" because ConfigT is
    // `std::monostate` (no fields).  HubScriptRunner's ctor body then
    // calls `set_uid(host_.config().identity().uid)` to populate the
    // real uid before any startup_() call reads it.
    uid_ = script_host_traits<ApiT>::uid_from_config(config_);
    core_.set_shutdown_flag(shutdown_flag);
}

// ─── Destructor — contract enforcement ───────────────────────────────────────
//
// C++ guarantees this destructor body always runs whenever a EngineHost-
// derived object is destroyed (provided EngineHost's own ctor completed).
// This is true for every lifetime path — normal scope exit, exception
// unwinding, `delete`, `unique_ptr::reset`, container teardown, static/
// thread-local destruction. The pure-virtual marker does NOT change that;
// the definition below is invoked by every derived destructor's implicit
// base-destructor call. Therefore the check below IS always performed.
//
// Contract: every path that reaches this destructor must have called
// @ref shutdown_ at least once. If not, the worker thread (owned by
// api_->thread_manager()) may still be alive and referencing derived-
// owned members that were just destroyed — a silent use-after-free.
// Rather than paper over that with a defensive cleanup, we surface the
// bug loudly via `std::abort()` with a diagnostic.
//
template <typename ApiT>
EngineHost<ApiT>::~EngineHost()
{
    // Contract: the host must be in `Constructed` (never started) or
    // `ShutDown` (started + properly shut down) by the time the
    // destructor runs.  If phase is `Running`, the worker thread is
    // still alive and may reference derived members that are about
    // to be destroyed — silent use-after-free.  Abort loudly with
    // diagnostic instead of papering over.
    const Phase p = phase_.load(std::memory_order_acquire);
    if (p == Phase::Running)
    {
        // The uid may be empty if config was never parsed; PLH_PANIC
        // formats unconditionally so the message layout stays stable.
        // `typeid(ApiT).name()` disambiguates which instantiation was
        // under the contract — EngineHost<RoleAPIBase> and
        // EngineHost<HubAPI> produce distinct mangled names, so a
        // reviewer reading the panic knows immediately whether the
        // failing host was role-side or hub-side.  Mangled name is
        // compiler-specific but always unambiguous within a single build.
        PLH_PANIC(
            "EngineHost<{}> destructor entered while in Running phase — "
            "shutdown_() was never called.  role_tag='{}' uid='{}'.  "
            "Either the derived destructor did not call shutdown_() as "
            "its first statement, or an override of shutdown_() failed "
            "to call EngineHost::shutdown_().  The worker thread may "
            "still reference now-destroyed derived members; aborting "
            "to avoid silent use-after-free.",
            typeid(ApiT).name(),
            role_tag_, uid_);
    }
}

template <typename ApiT>
void EngineHost<ApiT>::startup_()
{
    // Phase FSM (NOT a LifecycleGuard transition): Constructed → Running.
    // CAS rejects re-entry from Running (idempotent) and from ShutDown
    // (single-use after shutdown — construct a new host).
    Phase expected = Phase::Constructed;
    if (!phase_.compare_exchange_strong(expected, Phase::Running,
                                         std::memory_order_acq_rel))
    {
        if (expected == Phase::Running)
            return;  // idempotent: already running
        // expected == Phase::ShutDown
        PLH_PANIC(
            "EngineHost<{}>::startup_() called after shutdown_() — "
            "this host instance is single-use.  role_tag='{}' uid='{}'.  "
            "Construct a new EngineHost instance to start fresh.",
            typeid(ApiT).name(),
            role_tag_, uid_);
    }

    try
    {
        ready_promise_ = std::promise<bool>{};
        auto ready_future = ready_promise_.get_future();

        // Construct api_ here (not in ctor) so role_tag + uid are
        // available and the worker thread can be spawned under this
        // api's ThreadManager.  role_tag_ is the short form
        // ("prod"/"cons"/"proc" for role side; "hub" for hub side)
        // used by the ApiT-owned ThreadManager name + log prefixes.
        // uid_ is the host instance uid carried separately from
        // ConfigT (Phase 7 Option E — see engine_host.hpp ctor docs).
        api_ = std::make_unique<ApiT>(
            core_, std::string(role_tag_), uid_);

        api_->thread_manager().spawn("worker", [this] { worker_main_(); });

        const bool ok = ready_future.get();
        if (!ok)
        {
            // Worker signaled setup failure.  Run cleanup
            // (transitions Running → ShutDown so the destructor's
            // contract check passes; keeps host single-use).
            shutdown_();
        }
    }
    catch (...)
    {
        // Construction failure or worker spawn failure.  Roll back to
        // Constructed so the user can retry (e.g., after fixing the
        // config) without constructing a new host.  No partial state
        // remains: api_ is reset; no thread was successfully spawned
        // (or if it was, ready_future would have surfaced its result).
        api_.reset();
        phase_.store(Phase::Constructed, std::memory_order_release);
        throw;
    }
}

template <typename ApiT>
void EngineHost<ApiT>::shutdown_() noexcept
{
    // Phase FSM (NOT a LifecycleGuard transition): Running → ShutDown.
    // CAS makes the transition race-free; only the first effective
    // caller does the work.
    Phase expected = Phase::Running;
    if (!phase_.compare_exchange_strong(expected, Phase::ShutDown,
                                         std::memory_order_acq_rel))
    {
        // Either still Constructed (never started — nothing to do) or
        // already ShutDown (idempotent).  Either way, no-op.
        return;
    }

    // Underlying ops themselves tolerate repeated calls; the phase
    // transition above is the sole correctness gate.
    core_.request_stop();
    core_.notify_incoming();
    // api_.reset() → ApiT dtor → ThreadManager dtor →
    // bounded join of every host-scope thread (worker, ctrl, custom).
    api_.reset();
}

// ── Explicit instantiation ──────────────────────────────────────────────────
//
// Generate the template methods for each ApiT the library ships support
// for.  PYLABHUB_UTILS_EXPORT on the template class declaration causes
// these symbols to be exported from the shared library, so derived
// classes in consumer binaries (plh_role, plh_hub) link against them
// without recompiling the template body.
//
// To add a new host kind (e.g. a future binary or test fixture):
//   1. Ensure the new ApiT satisfies the constructor + thread_manager()
//      contract documented in engine_host.hpp.
//   2. Add a `script_host_traits<NewApiT>` specialization (mirrors
//      hub_api.hpp's specialization for HubAPI).
//   3. Add `template class EngineHost<NewApiT>;` below.
//   4. (Optional) Add a typedef `using NewHostBase = EngineHost<NewApiT>;`.
template class EngineHost<RoleAPIBase>;

} // namespace pylabhub::scripting

// ── HubAPI instantiation (HEP-CORE-0033 Phase 7 — Option E) ─────────────────
//
// The HubAPI specialization of EngineHost lives in pylabhub-utils.
// The instantiation must include hub_api.hpp so the trait specialization
// (`script_host_traits<HubAPI>::ConfigT = std::monostate`) is visible
// AND HubAPI's full type is available for the unique_ptr<ApiT>
// destructor.  HubConfig is NOT needed here (and was deliberately
// dropped from this TU during D2.1) — EngineHost is opaque to ConfigT,
// and the hub-side runner reads HubConfig fields through its host
// backref, not through `config()`.
#include "utils/hub_api.hpp"

namespace pylabhub::scripting
{
template class EngineHost<pylabhub::hub_host::HubAPI>;
} // namespace pylabhub::scripting
