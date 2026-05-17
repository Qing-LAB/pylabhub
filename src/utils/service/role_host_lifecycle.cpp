// HEP-CORE-0034 Phase 5c — role-host lifecycle helpers
//
// See `role_host_lifecycle.hpp` for the public surface.  The teardown
// sequence here mirrors the Steps 9-14 epilogue that producer /
// consumer / processor `worker_main_()` each ran inline pre-Phase-5c.

#include "utils/role_host_lifecycle.hpp"

#include "utils/broker_request_comm.hpp"
#include "utils/role_api_base.hpp"
#include "utils/role_host_core.hpp"
#include "utils/script_engine.hpp"
#include "utils/thread_manager.hpp"  // ThreadManager::drain / wait_for_quiescence
#include "utils/timeout_constants.hpp"

#include <chrono>
#include <utility>

namespace pylabhub::scripting
{

void do_role_teardown(
    ScriptEngine                       &engine,
    RoleAPIBase                        &api,
    RoleHostCore                       &core,
    bool                                has_api,
    std::function<void()>               teardown_infrastructure)
{
    // Step 9: stop accepting invoke from non-owner threads.
    engine.stop_accepting();

    // Step 9a: Explicitly deregister from broker (ctrl thread still running).
    // Skip when no API was ever wired (validate-only or aborted-startup paths).
    // The deregister flow uses `RoleAPIBase::Impl::Shared::take_*_channel`
    // (mutex-protected swap-with-empty) so the worker thread's pImpl
    // access here cannot race ctrl-thread paths that touch the same
    // registration state.
    if (has_api)
        api.deregister_from_broker();

    // Step 10: last script callback — ctrl thread still alive so the
    // script can perform final I/O (flush metrics, send summary, etc.).
    engine.invoke_on_stop();

    // Step 11: finalize engine (free script resources).
    engine.finalize();

    // Step 12: signal ctrl thread(s) poll loop(s) to exit (non-destructive
    // — sets stop flag + wakes poll, does NOT close sockets).  Mode-
    // aware via Wave-B M5 prep helper (F2): handler-mode signals every
    // connection's BRC, legacy mode signals the single broker_channel.
    api.stop_ctrl_for_teardown();
    core.set_running(false);
    core.notify_incoming();

    // Step 12.5: honor the Thread Shutdown Contract (HEP-CORE-0031 §4.1).
    // Flip every peer's per-slot shutdown_requested (master is skipped
    // by `request_shutdown_all`), then block until every managed thread
    // is outside its `with_active_loop` bracket (i.e. `active_loop_depth
    // == 0`).  This is the synchronization point that prevents the BRC
    // ctrl thread's pImpl access from racing the Step 13 destruction of
    // `broker_comm_` (the UAF root cause MD1 fixes).  Generic by design
    // — any thread spawned under this manager honors the same contract,
    // no per-thread-name special casing.
    //
    // Threads that never call `with_active_loop` (worker, broker svc
    // wrappers, etc.) stay at `active_loop_depth = 0` by default and
    // pass this wait instantly.  Class-level wake-up (broker_comm->stop()
    // above, core.notify_incoming(), and whatever the role-specific
    // teardown will issue) is what unblocks the threads currently
    // inside their brackets so they can exit.
    api.thread_manager().request_shutdown_all();
    (void)api.thread_manager().wait_for_quiescence(
        std::chrono::milliseconds{pylabhub::kMidTimeoutMs});

    // Step 13: teardown infrastructure (role-specific — disconnect broker,
    // close inbox/queues).  Safe to destroy `broker_comm_` here because
    // Step 12.5 confirmed no managed thread is inside its
    // active-loop bracket.
    if (teardown_infrastructure)
        teardown_infrastructure();

    // Step 14: return.  The worker thread is itself a managed slot,
    // so it MUST NOT call `thread_manager().drain()` here — that
    // would walk every slot including this one's, find its own `done`
    // false (set only after this body returns), time out, detach
    // itself, and bump `process_detached_count`.  The single
    // coordinated drain happens on the MAIN thread in
    // `EngineHost<ApiT>::shutdown_()` after this worker returns and
    // `run_role_main_loop` observes `core.is_running() == false`.
    // Peers are already signaled (Step 12.5); the master (ctrl) is
    // signaled by main's drain Phase 3.  See HEP-CORE-0031 §4.2.
}

} // namespace pylabhub::scripting
