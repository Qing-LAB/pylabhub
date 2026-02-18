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
        LOGGER_INFO("ZMQContext: ZeroMQ context created.");
    }
}

void zmq_context_shutdown()
{
    if (g_context == nullptr)
    {
        return; // idempotent: safe if called twice (e.g. both GetZMQContextModule and GetLifecycleModule registered)
    }
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
