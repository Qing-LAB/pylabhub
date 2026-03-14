#include "utils/zmq_context.hpp"
#include "utils/lifecycle.hpp"
#include "utils/logger.hpp"
#include <atomic>

namespace pylabhub::hub
{

namespace
{
constexpr std::chrono::milliseconds kZMQContextShutdownTimeoutMs{2000};
// Use std::atomic for formal memory-model correctness: get_zmq_context() is callable
// from any thread; startup/shutdown run under lifecycle ordering, but without atomics
// the C++ memory model formally requires synchronization even when happens-before is
// ensured by external mechanisms.
std::atomic<zmq::context_t *> g_context{nullptr};

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
    auto *ctx = g_context.load(std::memory_order_acquire);
    assert(ctx != nullptr && "ZMQ context not initialized");
    return *ctx;
}

void zmq_context_startup()
{
    if (g_context.load(std::memory_order_acquire) == nullptr)
    {
        auto *ctx = new zmq::context_t(1);
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
        zmq_ctx_set(ctx->handle(), ZMQ_BLOCKY, 0);
        g_context.store(ctx, std::memory_order_release);
        LOGGER_INFO("ZMQContext: ZeroMQ context created.");
    }
}

void zmq_context_shutdown()
{
    auto *ctx = g_context.load(std::memory_order_acquire);
    if (ctx == nullptr)
    {
        return; // idempotent: safe if called twice (e.g. both GetZMQContextModule and GetLifecycleModule registered)
    }
    // Belt-and-suspenders: signal any blocking socket operations to return with
    // ETERM before calling zmq_ctx_term().  With ZMQ_BLOCKY=0 (set at creation),
    // all sockets already default to LINGER=0, so socket closes are instant and
    // zmq_ctx_term() normally returns quickly.  shutdown() handles the edge case
    // where a socket was explicitly set to LINGER > 0 after creation.
    ctx->shutdown();
    // [REVIEW-22] Store nullptr BEFORE delete so no other thread can observe a
    // valid-looking pointer to freed memory between delete and the store.
    g_context.store(nullptr, std::memory_order_release);
    delete ctx;
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
