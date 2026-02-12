// tests/test_layer3_datahub/workers/messagehub_workers.cpp
// Phase C – MessageHub unit tests (no broker required).
#include "messagehub_workers.h"
#include "test_entrypoint.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>
#include <fmt/core.h>

using namespace pylabhub::hub;
using namespace pylabhub::tests::helper;

namespace pylabhub::tests::worker::messagehub
{

static auto logger_module() { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto crypto_module() { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto hub_module() { return ::pylabhub::hub::GetLifecycleModule(); }

int lifecycle_initialized_follows_state()
{
    return run_gtest_worker(
        []()
        {
            EXPECT_TRUE(pylabhub::hub::lifecycle_initialized());
        },
        "messagehub.lifecycle_initialized_follows_state",
        logger_module(), crypto_module(), hub_module());
}

int send_message_when_not_connected_returns_nullopt()
{
    return run_gtest_worker(
        []()
        {
            MessageHub &hub_ref = MessageHub::get_instance();
            std::optional<std::string> result = hub_ref.send_message("REG_REQ", "{}", 100);
            EXPECT_FALSE(result.has_value());
        },
        "messagehub.send_message_when_not_connected_returns_nullopt",
        logger_module(), crypto_module(), hub_module());
}

int receive_message_when_not_connected_returns_nullopt()
{
    return run_gtest_worker(
        []()
        {
            MessageHub &hub_ref = MessageHub::get_instance();
            std::optional<std::string> result = hub_ref.receive_message(50);
            EXPECT_FALSE(result.has_value());
        },
        "messagehub.receive_message_when_not_connected_returns_nullopt",
        logger_module(), crypto_module(), hub_module());
}

int register_producer_when_not_connected_returns_false()
{
    return run_gtest_worker(
        []()
        {
            MessageHub &hub_ref = MessageHub::get_instance();
            ProducerInfo info{};
            info.shm_name = "test_shm";
            info.producer_pid = 12345;
            info.schema_hash.assign(32, '\0');
            info.schema_version = 0;
            EXPECT_FALSE(hub_ref.register_producer("test_channel", info));
        },
        "messagehub.register_producer_when_not_connected_returns_false",
        logger_module(), crypto_module(), hub_module());
}

int discover_producer_when_not_connected_returns_nullopt()
{
    return run_gtest_worker(
        []()
        {
            MessageHub &hub_ref = MessageHub::get_instance();
            std::optional<ConsumerInfo> result = hub_ref.discover_producer("test_channel");
            EXPECT_FALSE(result.has_value());
        },
        "messagehub.discover_producer_when_not_connected_returns_nullopt",
        logger_module(), crypto_module(), hub_module());
}

int disconnect_when_not_connected_idempotent()
{
    return run_gtest_worker(
        []()
        {
            MessageHub &hub_ref = MessageHub::get_instance();
            hub_ref.disconnect();
            hub_ref.disconnect();
        },
        "messagehub.disconnect_when_not_connected_idempotent",
        logger_module(), crypto_module(), hub_module());
}

} // namespace pylabhub::tests::worker::messagehub

namespace
{
struct MessageHubWorkerRegistrar
{
    MessageHubWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos || mode.substr(0, dot) != "messagehub")
                    return -1;
                std::string scenario(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::messagehub;
                if (scenario == "lifecycle_initialized_follows_state")
                    return lifecycle_initialized_follows_state();
                if (scenario == "send_message_when_not_connected_returns_nullopt")
                    return send_message_when_not_connected_returns_nullopt();
                if (scenario == "receive_message_when_not_connected_returns_nullopt")
                    return receive_message_when_not_connected_returns_nullopt();
                if (scenario == "register_producer_when_not_connected_returns_false")
                    return register_producer_when_not_connected_returns_false();
                if (scenario == "discover_producer_when_not_connected_returns_nullopt")
                    return discover_producer_when_not_connected_returns_nullopt();
                if (scenario == "disconnect_when_not_connected_idempotent")
                    return disconnect_when_not_connected_idempotent();
                fmt::print(stderr, "ERROR: Unknown messagehub scenario '{}'\n", scenario);
                return 1;
            });
    }
};
static MessageHubWorkerRegistrar g_messagehub_registrar;
} // namespace
