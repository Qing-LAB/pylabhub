#include "plh_service.hpp"
#include "broker_service.hpp"

#include <csignal>

namespace
{
pylabhub::broker::BrokerService* g_broker = nullptr; // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

void signal_handler(int /*sig*/)
{
    if (g_broker != nullptr)
    {
        g_broker->stop();
    }
}
} // namespace

int main(int argc, char* argv[])
{
    pylabhub::utils::LifecycleGuard lifecycle(pylabhub::utils::MakeModDefList(
        pylabhub::utils::Logger::GetLifecycleModule()));

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    pylabhub::broker::BrokerService::Config cfg;
    if (argc >= 2) // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    {
        cfg.endpoint = argv[1]; // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    }

    pylabhub::broker::BrokerService broker(cfg);
    g_broker = &broker;

    LOGGER_INFO("pylabhub-broker starting on {}", cfg.endpoint);
    broker.run();

    return 0;
}
