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
#include "utils/thread_manager.hpp"  // ThreadManager::drain

#include <utility>

namespace pylabhub::scripting
{

void do_role_teardown(
    ScriptEngine                       &engine,
    RoleAPIBase                        &api,
    RoleHostCore                       &core,
    ::pylabhub::hub::BrokerRequestComm *broker_comm,
    bool                                has_api,
    std::function<void()>               teardown_infrastructure)
{
    // Step 9: stop accepting invoke from non-owner threads.
    engine.stop_accepting();

    // Step 9a: Explicitly deregister from broker (ctrl thread still running).
    // Skip when no API was ever wired (validate-only or aborted-startup paths).
    if (has_api)
        api.deregister_from_broker();

    // Step 10: last script callback — ctrl thread still alive so the
    // script can perform final I/O (flush metrics, send summary, etc.).
    engine.invoke_on_stop();

    // Step 11: finalize engine (free script resources).
    engine.finalize();

    // Step 12: signal ctrl thread's poll loop to exit (non-destructive —
    // sets stop flag + wakes poll, does NOT close sockets).
    if (broker_comm) broker_comm->stop();
    core.set_running(false);
    core.notify_incoming();

    // Step 13: teardown infrastructure (role-specific — disconnect broker,
    // close inbox/queues).  The role host supplies this callback.
    if (teardown_infrastructure)
        teardown_infrastructure();

    // Step 14: drain all managed threads — last.  Ctrl thread has already
    // exited its poll loop (signaled in step 12), so join is immediate.
    api.thread_manager().drain();
}

} // namespace pylabhub::scripting
