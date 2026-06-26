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
#include "utils/logger.hpp"       // LOGGER_SYSTEM_SYNC on GIL-stolen leak
#include "utils/role_api_base.hpp"
#include "utils/script_engine.hpp"   // engine().release_global_lock_during_wait()
#include "utils/thread_manager.hpp"  // api_->thread_manager() inside startup_
#include "utils/timeout_constants.hpp" // kMidTimeoutMs (do_role_teardown)

#include <atomic>
#include <chrono>     // detach safety gate grace period
#include <thread>     // sleep_for during detach grace poll
#include <typeinfo>   // typeid(ApiT).name() in destructor diagnostic
#include <utility>

namespace pylabhub::scripting
{

template <typename ApiT>
EngineHost<ApiT>::EngineHost(std::string_view short_tag,
                              ConfigT config,
                              std::atomic<bool> *shutdown_flag)
    : short_tag_(short_tag)
    , config_(std::move(config))
    , engine_(nullptr)  // constructed in derived's worker_main_ Step 0
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
            "shutdown_() was never called.  short_tag='{}' uid='{}'.  "
            "Either the derived destructor did not call shutdown_() as "
            "its first statement, or an override of shutdown_() failed "
            "to call EngineHost::shutdown_().  The worker thread may "
            "still reference now-destroyed derived members; aborting "
            "to avoid silent use-after-free.",
            typeid(ApiT).name(),
            short_tag_, uid_);
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
            "this host instance is single-use.  short_tag='{}' uid='{}'.  "
            "Construct a new EngineHost instance to start fresh.",
            typeid(ApiT).name(),
            short_tag_, uid_);
    }

    try
    {
        ready_promise_ = std::promise<bool>{};
        auto ready_future = ready_promise_.get_future();

        // Construct api_ here (not in ctor) so short_tag + uid are
        // available and the worker thread can be spawned under this
        // api's ThreadManager.  short_tag_ is the short form
        // ("prod"/"cons"/"proc" for role side; "hub" for hub side)
        // used by the ApiT-owned ThreadManager name + log prefixes.
        // uid_ is the host instance uid carried separately from
        // ConfigT (Phase 7 Option E — see engine_host.hpp ctor docs).
        api_ = std::make_unique<ApiT>(
            core_, std::string(short_tag_), uid_);

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

    // Shutdown ordering — HEP-CORE-0011 step 14.
    //
    // The worker thread exits its main loop on `shutdown_requested_`
    // (above) and then runs its post-loop cleanup which still calls
    // `api().dispatch_event(...)` / `api().dispatch_tick()` /
    // `engine().invoke_on_stop()` etc.  Those calls read `*api_` —
    // i.e. the unique_ptr's internal pointer — at the start of every
    // dispatch.
    //
    // We therefore must NOT use `api_.reset()` directly here.  The
    // libstdc++ implementation of `unique_ptr::reset()` writes the
    // internal pointer to nullptr BEFORE invoking the deleter (which
    // is what runs `~ApiT → ~ThreadManager → join(worker)`).  During
    // the blocking join, the worker is still alive and can read
    // `api()` between two `dispatch_event` calls — and that read
    // returns a null reference, causing a SIGSEGV in
    // `HubAPI::dispatch_event(this=0x0)`.  This race actually fires
    // in practice (~25% of HubLuaIntegrationTest runs).
    //
    // Correct order:
    //   1. Explicitly drain ApiT's ThreadManager while api_ is still
    //      a valid live unique_ptr (the worker reads api() through
    //      the still-non-null internal pointer until it exits).
    //   2. Only after every worker / ctrl / custom thread has joined
    //      is it safe to nullify api_.  No surviving thread can
    //      observe the null window because no surviving thread
    //      exists.
    api_->thread_manager().drain();

    // Layer 2 — bounded shutdown diagnostic.  ThreadManager::drain()
    // already ran a per-thread bounded join with detach-on-timeout
    // (default 5 s = `kMidTimeoutMs`) and emitted its own ERROR log.
    // If the worker leaked AND the engine had opted into
    // `release_global_lock_during_wait`, surface a CRITICAL message
    // pointing at the most likely cause so the operator can find it
    // quickly without having to correlate the generic ThreadManager
    // log with the per-engine config.
    //
    // **CPython context** (HEP-CORE-0011 §"Engine Thread Affinity"):
    // CPython's `take_gil` (called by `PyEval_RestoreThread` during
    // the optional's dtor) wakes every `sys.setswitchinterval()`
    // (default 5 ms in 3.10+) and signals the holder to drop the
    // GIL.  Pure Python code yields within ~5 ms.  Only a script-
    // spawned C extension that holds the GIL without yielding (long
    // NumPy op without `with nogil:`, native module with a long
    // pure-C path, etc.) can wedge the reacquire.  This is exactly
    // the case the message below is calling out.
    if (api_ && api_->thread_manager().detached_count_last_drain() > 0
        && has_engine() && engine().release_global_lock_during_wait())
    {
        // SYSTEM_SYNC — bypass the async log queue.  This message is
        // emitted on the shutdown path; the async logger is still
        // alive at this point but its queue may not flush before the
        // process exits if a downstream lifecycle module also wedges.
        // Sync delivery guarantees the operator-diagnostic message
        // reaches the sink (file / stderr) before any further teardown.
        LOGGER_SYSTEM_SYNC(
            "[EngineHost:{}] worker thread did not exit within "
            "ThreadManager's bounded join AND this engine has "
            "`script.release_global_lock_during_wait` enabled.  Most "
            "likely cause: a script-spawned thread is holding the "
            "Python GIL across the worker's reacquire.  Look for "
            "non-yielding C extensions in the script's sub-threads "
            "(NumPy/SciPy ops without `with nogil:`, native modules "
            "with long pure-C paths, or `time.sleep` from a C "
            "library that does not drop the GIL).  Pure Python code "
            "yields the GIL every ~5 ms (sys.setswitchinterval) and "
            "would NOT wedge here.  The worker thread has been "
            "DETACHED; the OS will reap it on process exit.  Some "
            "engine-owned resources (Python objects, sockets) may "
            "leak until then.",
            short_tag_);
    }

    // Detach safety gate (HEP-CORE-0031 §4.2; post-MD1.5 — bugs pinned
    // 2026-05-13).  If ANY thread was detached on timeout, the
    // process still holds at least one live thread that owns a
    // pointer into `*api_` (typically through `RoleAPIBase::pImpl`).
    // Resetting `api_` here would free that storage out from under
    // the detached thread, which then triggers SIGSEGV on its next
    // memory access — the exact bug the gdb investigation pinned to
    // `pthread_mutex_lock+0x4` inside `deregister_from_broker`.
    //
    // Best-effort recovery before falling back to leak: drain retains
    // each detached slot's `done` shared_ptr, so we can poll
    // `all_detached_done()` for a bounded grace period.  If every
    // runaway thread returns within the grace window we can safely
    // call `api_.reset()` and let `~ApiT` run normally.  Otherwise we
    // release ownership (leak) rather than UAF; the OS reaps the
    // detached thread + its allocations at process exit.
    if (api_ && api_->thread_manager().detached_count_last_drain() > 0)
    {
        constexpr auto kDetachGrace = std::chrono::seconds{10};
        const auto      deadline    =
            std::chrono::steady_clock::now() + kDetachGrace;
        while (!api_->thread_manager().all_detached_done() &&
               std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{50});
        }

        if (api_->thread_manager().all_detached_done())
        {
            // Every detached thread has returned; their wrapper
            // lambdas are done writing to retained state.  Safe to
            // reset normally.
            LOGGER_WARN(
                "[EngineHost:{}] {} thread(s) were detached during "
                "shutdown drain but all returned within the grace "
                "window — proceeding with normal `api_.reset()`.",
                short_tag_,
                api_->thread_manager().detached_count_last_drain());
            api_.reset();
            return;
        }

        // Grace expired with at least one runaway still active.
        // Leak as last resort — see comment block above.
        LOGGER_ERROR(
            "[EngineHost:{}] {} thread(s) were detached during shutdown "
            "drain AND at least one is still running after a {}s grace "
            "period.  Leaking `api_` rather than calling `api_.reset()` "
            "— resetting would free state still owned by the runaway "
            "thread (deterministic UAF; see HEP-CORE-0031 §4.2).  The "
            "OS will reap the detached thread(s) and their allocations "
            "at process exit.",
            short_tag_,
            api_->thread_manager().detached_count_last_drain(),
            kDetachGrace.count());
        (void)api_.release();  // drop ownership without running ~ApiT
        return;
    }

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

// ── do_role_teardown — worker-loop epilogue helper ──────────────────────────
//
// Moved here from `role_host_lifecycle.{hpp,cpp}` on 2026-05-20.  The
// old filename collided with the framework `Lifecycle` module
// (`src/include/utils/lifecycle.hpp`, the module-init/teardown
// registry) — two unrelated subsystems sharing one word.  This
// helper IS the worker-loop epilogue for EngineHost-derived classes;
// it's not related to the Lifecycle module at all.
//
// Body unchanged from Phase 5c (commit e9a79bc) except for the
// docstring scrub: removed obsolete "legacy mode / set_broker_comm /
// broker_channel fallback" references that became dead with Wave-B
// M4f (commit 6da5837).
void do_role_teardown(
    ScriptEngine          &engine,
    RoleAPIBase           &api,
    RoleHostCore          &core,
    bool                   has_api,
    std::function<void()>  teardown_infrastructure)
{
    // Step 9: stop accepting invoke from non-owner threads.
    engine.stop_accepting();

    // Step 9a: Explicitly deregister from broker (ctrl thread still running).
    // Skip when no API was ever wired (validate-only or aborted-startup paths).
    // The deregister walks `handler_->presences()` and dispatches DEREG
    // for each presence whose atomic `registration_state` is Registered
    // or RegRequestPending (S1+O4, 2026-05-17).
    if (has_api)
        api.deregister_from_broker();

    // Step 10: last script callback — ctrl thread still alive so the
    // script can perform final I/O (flush metrics, send summary, etc.).
    engine.invoke_on_stop();

    // Step 11: finalize engine (free script resources).
    engine.finalize();

    // Step 12: signal every connection's BRC poll loop to exit
    // (non-destructive — sets stop flag + wakes poll, does NOT close
    // sockets).  Handler-mode only post-Wave-B M4f (commit 6da5837);
    // no legacy single-broker_channel fallback remains.
    api.stop_ctrl_for_teardown();
    core.set_running(false);
    core.notify_incoming();

    // Step 12.5: honor the Thread Shutdown Contract (HEP-CORE-0031 §4.1).
    // Flip every peer's per-slot shutdown_requested (master is skipped
    // by `request_shutdown_all`), then block until every managed thread
    // is outside its `with_active_loop` bracket.  This synchronization
    // point prevents the BRC ctrl thread's pImpl access from racing
    // the Step 13 destruction of handler-mode BRCs (the UAF root cause
    // MD1 fixes).
    api.thread_manager().request_shutdown_all();
    (void)api.thread_manager().wait_for_quiescence(
        std::chrono::milliseconds{pylabhub::kMidTimeoutMs});

    // Step 13: teardown infrastructure (role-specific — disconnect broker,
    // close inbox/queues).  Safe to destroy handler-mode BRCs here
    // because Step 12.5 confirmed no managed thread is inside its
    // active-loop bracket.  Handler-mode hosts call
    // `api.stop_handler_threads()` here to disconnect + release the
    // handler's BRCs.
    if (teardown_infrastructure)
        teardown_infrastructure();

    // Step 14: return.  The worker thread is itself a managed slot,
    // so it MUST NOT call `thread_manager().drain()` here — that
    // would walk every slot including this one's, find its own `done`
    // false (set only after this body returns), time out, detach
    // itself, and bump `process_detached_count`.  The single
    // coordinated drain happens on the MAIN thread in
    // `EngineHost::shutdown_()` after this worker returns and
    // `run_role_main_loop` observes `core.is_running() == false`.
    // Peers are already signaled (Step 12.5); the master (ctrl) is
    // signaled by main's drain Phase 3.  See HEP-CORE-0031 §4.2.
}

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
