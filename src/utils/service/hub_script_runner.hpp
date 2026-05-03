#pragma once
/**
 * @file hub_script_runner.hpp
 * @brief HubScriptRunner — hub-side script-thread runtime (HEP-CORE-0033 Phase 7).
 *
 * **Private header.**  Lives under `src/utils/service/`, NOT
 * `src/include/utils/`, because nothing outside `pylabhub-utils` needs
 * to construct or inspect this class — HubHost owns it via
 * `unique_ptr` and exposes hub-script operations through wrapper
 * methods (per the Phase 7 placement decision: encapsulation over
 * exposure when there's no real reach-through use case).
 *
 * Differs from AdminService's exposure pattern (which IS public):
 *   - AdminService gets `host.admin()` because tests construct it
 *     directly + need to inspect bound endpoints.
 *   - HubScriptRunner has no such surface — broker fires events INTO
 *     it via HubState subscriptions; nothing reaches IN from outside.
 *     Tests verify it through HubHost.startup() + observable script
 *     side effects.
 *
 * Inherits from `EngineHost<HubAPI>`:
 *   - phase FSM (Constructed → Running → ShutDown), startup_/shutdown_,
 *     thread spawn, ready_promise — all reused from the role-side
 *     template instantiation.
 *   - Worker thread implements `worker_main_()` with the hub-side
 *     event-and-tick loop driven by `cfg.timing().timing_params()`.
 *
 * Cross-thread event delivery (Commit C decision C-Q1):
 *   - Reuses `RoleHostCore`'s `IncomingMessage` queue + `notify_incoming` /
 *     `wait_for_incoming` machinery — no new infrastructure.
 *   - Worker subscribes to all 11 HubState events at startup.  Each
 *     subscription handler captures the typed payload, serializes via
 *     Phase 6.2b's `channel_to_json` / `role_to_json` / etc., wraps
 *     in an `IncomingMessage{event, sender, details}`, enqueues, and
 *     wakes the worker.
 *   - Worker thread drains the queue and forwards each event to
 *     `api_->dispatch_event(msg)` which translates to
 *     `engine_->invoke("on_<event>", details)`.
 *   - Periodic `on_tick` fires when the LoopTimingPolicy deadline
 *     elapses (not on every loop iteration — the queue's
 *     `wait_for_incoming(timeout_ms)` is the single blocking primitive
 *     that wakes on either an event arrival OR the tick deadline).
 *
 * Lifetime:
 *   - Constructed by HubHost::startup() if `cfg.script().path` is set.
 *   - Stopped by HubHost::shutdown() at HEP §4.2 step 2 (BEFORE admin
 *     and broker — ensures in-flight script callbacks complete and
 *     no new callbacks fire while subsystems tear down).
 */

#include "utils/engine_host.hpp"
#include "utils/hub_api.hpp"
//
// HubConfig is intentionally NOT included here under Phase 7 Option E.
// `script_host_traits<HubAPI>::ConfigT = std::monostate` (see
// hub_api.hpp), so EngineHost<HubAPI> stores nothing of HubConfig
// shape.  HubConfig fields (timing, identity) are read through the
// `host_` backref inside `worker_main_()` — that is a .cpp-only
// dependency, not a header dependency.  Keeping it out of this header
// preserves single-config-owner discipline at the type level.

#include <atomic>
#include <memory>

namespace pylabhub::hub_host { class HubHost; }

namespace pylabhub::scripting
{

/// Convenience alias mirroring role-side `RoleHostBase`.
/// `HubScriptRunner` is the only direct derivation today.
using HubScriptRunnerBase = EngineHost<pylabhub::hub_host::HubAPI>;

/// Hub-side script-thread runtime.  See file header for design notes.
class HubScriptRunner final : public HubScriptRunnerBase
{
public:
    /// @param host        HubHost backref — outlives the runner per
    ///                    HEP-0033 §4.2 step 2.  Set on the HubAPI
    ///                    after EngineHost lazy-constructs it.  Per
    ///                    Phase 7 Option E, the runner does NOT own a
    ///                    HubConfig — it reads timing/identity through
    ///                    `host.config()` inside `worker_main_()`.
    /// @param engine      ScriptEngine instance — caller (HubHost)
    ///                    creates via `make_engine_from_script_config`
    ///                    and moves in.
    /// @param shutdown_flag  External shutdown atomic shared with main
    ///                       and HubHost; nullptr ok in tests.
    HubScriptRunner(pylabhub::hub_host::HubHost  &host,
                    std::unique_ptr<ScriptEngine> engine,
                    std::atomic<bool>            *shutdown_flag = nullptr);

    /// Destructor calls shutdown_() (per EngineHost contract — must be
    /// the FIRST statement, see EngineHost::~EngineHost docs for the
    /// abort-on-Running-phase rationale).
    ~HubScriptRunner() override;

    HubScriptRunner(const HubScriptRunner &)            = delete;
    HubScriptRunner &operator=(const HubScriptRunner &) = delete;

    /// Forward to the owned `ScriptEngine::eval(code)`.  HubHost calls
    /// this through `HubHost::eval_in_script(code)` for the admin
    /// `exec_python` RPC tail (Phase 7 Commit E).  Public-on-the-runner
    /// rather than friending HubHost: keeps the access surface narrow
    /// (just `eval`, not the whole engine) and parallel to how the
    /// admin RPC dispatches to other host methods.
    ///
    /// Thread-safety caveat: see `HubHost::eval_in_script` docs —
    /// the engine has the runner's worker thread as a concurrent
    /// caller.  Serialization mechanism arrives in Commit E.
    [[nodiscard]] InvokeResponse eval(const std::string &code);

protected:
    /// EngineHost worker hook.  Subscribes to all 11 HubState events
    /// at startup, then runs the event-poll-or-tick loop until
    /// shutdown.  `ready_promise().set_value(true)` fires after
    /// subscriptions are wired (or `set_value(false)` on any
    /// startup-phase error) so the main thread's startup_() returns
    /// promptly with a definite outcome.
    void worker_main_() override;

private:
    pylabhub::hub_host::HubHost &host_;
};

} // namespace pylabhub::scripting
