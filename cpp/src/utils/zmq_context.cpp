#include "utils/zmq_context.hpp"
#include "plh_service.hpp"

namespace pylabhub::hub
{

namespace
{
constexpr std::chrono::milliseconds kZMQContextShutdownTimeoutMs{2000};
zmq::context_t *g_context = nullptr;

void do_zmq_context_startup(const char * /*arg*/)
{
    zmq_context_startup();
}

void do_zmq_context_shutdown(const char * /*arg*/)
{
    zmq_context_shutdown();
}
} // namespace

zmq::context_t &get_zmq_context()
{
    assert(g_context != nullptr && "ZMQ context not initialized");
    return *g_context;
}

void zmq_context_startup()
{
    if (g_context == nullptr)
    {
        g_context = new zmq::context_t(1);
        // Set ZMQ_BLOCKY=0 so all sockets created in this context default to
        // LINGER=0 (instead of the ZMQ default of LINGER=-1).
        //
        // With LINGER=-1, zmq_close() on a socket that has pending outgoing
        // messages (e.g. BYE, heartbeats, subscription events) blocks until
        // ZMQ's I/O thread delivers them — which can take indefinitely if the
        // peer is gone or the connection is congested.  This causes zmq_ctx_term()
        // to block in tests that shut down the context without a full lifecycle.
        //
        // In the normal production lifecycle, the ZMQContext module is shut down
        // LAST (after all Messengers and channels are closed), so all pending
        // messages have already been delivered by the time zmq_ctx_term() runs
        // — LINGER=0 is therefore safe.  In tests that call zmq_context_startup/
        // shutdown() directly, LINGER=0 prevents indefinite hangs in teardown.
        zmq_ctx_set(g_context->handle(), ZMQ_BLOCKY, 0);
        LOGGER_INFO("ZMQContext: ZeroMQ context created.");
    }
}

void zmq_context_shutdown()
{
    if (g_context == nullptr)
    {
        return; // idempotent: safe if called twice (e.g. both GetZMQContextModule and GetLifecycleModule registered)
    }
    // Belt-and-suspenders: signal any blocking socket operations to return with
    // ETERM before calling zmq_ctx_term().  With ZMQ_BLOCKY=0 (set at creation),
    // all sockets already default to LINGER=0, so socket closes are instant and
    // zmq_ctx_term() normally returns quickly.  shutdown() handles the edge case
    // where a socket was explicitly set to LINGER > 0 after creation.
    g_context->shutdown();
    delete g_context;
    g_context = nullptr;
    LOGGER_INFO("ZMQContext: ZeroMQ context destroyed.");
}

pylabhub::utils::ModuleDef GetZMQContextModule()
{
    pylabhub::utils::ModuleDef module("ZMQContext");
    module.add_dependency("pylabhub::utils::Logger");
    module.set_startup(&do_zmq_context_startup);
    module.set_shutdown(&do_zmq_context_shutdown, kZMQContextShutdownTimeoutMs);
    module.set_as_persistent(true);
    return module;
}

} // namespace pylabhub::hub
