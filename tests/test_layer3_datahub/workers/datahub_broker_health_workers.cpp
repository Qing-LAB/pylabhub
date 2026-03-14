// tests/test_layer3_datahub/workers/datahub_broker_health_workers.cpp
//
// Broker/Producer/Consumer health and notification tests.
// Uses a real BrokerService in a background thread + Messenger singleton.
#include "datahub_broker_health_workers.h"
#include "test_entrypoint.h"
#include "test_process_utils.h"
#include "shared_test_helpers.h"

#include "utils/broker_service.hpp"
#include "plh_datahub.hpp"

#include <gtest/gtest.h>
#include <fmt/core.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

using namespace pylabhub::tests::helper;
using namespace pylabhub::hub;
using pylabhub::broker::BrokerService;

namespace pylabhub::tests::worker::broker_health
{

static auto logger_module() { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto crypto_module() { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto hub_module() { return ::pylabhub::hub::GetLifecycleModule(); }

// ============================================================================
// Shared helpers
// ============================================================================

namespace
{

struct BrokerHandle
{
    std::unique_ptr<BrokerService> service;
    std::thread                    thread;
    std::string                    endpoint;
    std::string                    pubkey;

    ~BrokerHandle()
    {
        if (thread.joinable())
        {
            if (service)
                service->stop();
            thread.join();
        }
    }

    BrokerHandle()                               = default;
    BrokerHandle(const BrokerHandle &)            = delete;
    BrokerHandle &operator=(const BrokerHandle &) = delete;
    BrokerHandle(BrokerHandle &&)                 = default;
    BrokerHandle &operator=(BrokerHandle &&)      = default;

    void stop_and_join()
    {
        if (service)
            service->stop();
        if (thread.joinable())
            thread.join();
    }
};

BrokerHandle start_broker_with_cfg(BrokerService::Config cfg)
{
    using ReadyInfo = std::pair<std::string, std::string>;
    auto promise = std::make_shared<std::promise<ReadyInfo>>();
    auto future  = promise->get_future();

    cfg.on_ready = [promise](const std::string &ep, const std::string &pk) {
        promise->set_value({ep, pk});
    };

    auto service = std::make_unique<BrokerService>(std::move(cfg));
    auto *raw    = service.get();
    std::thread t([raw]() { raw->run(); });

    auto info = future.get();

    BrokerHandle h;
    h.service  = std::move(service);
    h.thread   = std::move(t);
    h.endpoint = info.first;
    h.pubkey   = info.second;
    return h;
}

BrokerHandle start_broker()
{
    BrokerService::Config cfg;
    cfg.endpoint  = "tcp://127.0.0.1:0";
    cfg.use_curve = true;
    return start_broker_with_cfg(std::move(cfg));
}

} // anonymous namespace

// ============================================================================
// producer_gets_closing_notify
// ============================================================================

int producer_gets_closing_notify(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            // Broker: heartbeat timeout = 1s.
            BrokerService::Config cfg;
            cfg.endpoint        = "tcp://127.0.0.1:0";
            cfg.use_curve       = true;
            cfg.channel_timeout = std::chrono::seconds(1);
            cfg.consumer_liveness_check_interval = std::chrono::seconds(0); // disable
            auto broker = start_broker_with_cfg(std::move(cfg));

            Messenger &messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(broker.endpoint, broker.pubkey));

            const std::string ch_name = make_test_channel_name("health.closing_notify");

            ProducerOptions opts;
            opts.channel_name = ch_name;
            opts.pattern      = ChannelPattern::PubSub;
            opts.has_shm      = false;
            opts.timeout_ms   = 3000;

            auto producer = Producer::create(messenger, opts);
            ASSERT_TRUE(producer.has_value()) << "Producer::create failed";

            std::atomic<bool> closing_fired{false};
            producer->on_channel_closing([&]() { closing_fired.store(true); });

            // Start producer (begins sending heartbeats). Then stop it so heartbeats cease.
            ASSERT_TRUE(producer->start());
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            producer->stop(); // stop heartbeats; channel will time out in ~1s

            // Wait up to 4s for CHANNEL_CLOSING_NOTIFY.
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(4);
            while (!closing_fired.load() &&
                   std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            EXPECT_TRUE(closing_fired.load())
                << "CHANNEL_CLOSING_NOTIFY was not received within 4s";

            messenger.disconnect();
            broker.stop_and_join();
        },
        "broker_health.producer_gets_closing_notify",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// consumer_auto_deregisters
// ============================================================================

int consumer_auto_deregisters(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            auto broker = start_broker();

            Messenger &messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(broker.endpoint, broker.pubkey));

            const std::string ch_name = make_test_channel_name("health.consumer_dereg");

            ProducerOptions popts;
            popts.channel_name = ch_name;
            popts.pattern      = ChannelPattern::PubSub;
            popts.has_shm      = false;
            popts.timeout_ms   = 3000;

            auto producer = Producer::create(messenger, popts);
            ASSERT_TRUE(producer.has_value()) << "Producer::create failed";

            ConsumerOptions copts;
            copts.channel_name = ch_name;
            copts.timeout_ms   = 3000;

            auto consumer = Consumer::connect(messenger, copts);
            ASSERT_TRUE(consumer.has_value()) << "Consumer::connect failed";

            // Consumer::close() must send CONSUMER_DEREG_REQ to broker.
            consumer->close();

            // Allow broker to process the deregistration.
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            // Discover the channel again — consumer_count should be 0.
            auto info = messenger.discover_producer(ch_name, /*timeout_ms=*/2000);
            ASSERT_TRUE(info.has_value()) << "discover_producer failed after consumer close";
            EXPECT_EQ(info->consumer_count, 0u)
                << "Expected consumer_count=0 after Consumer::close()";

            producer->close();
            messenger.disconnect();
            broker.stop_and_join();
        },
        "broker_health.consumer_auto_deregisters",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// producer_auto_deregisters
// ============================================================================

int producer_auto_deregisters(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            // Broker with long timeout — we must not rely on timeout for re-registration.
            BrokerService::Config cfg;
            cfg.endpoint        = "tcp://127.0.0.1:0";
            cfg.use_curve       = true;
            cfg.channel_timeout = std::chrono::seconds(30); // very long
            cfg.consumer_liveness_check_interval = std::chrono::seconds(0); // disable
            auto broker = start_broker_with_cfg(std::move(cfg));

            Messenger &messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(broker.endpoint, broker.pubkey));

            const std::string ch_name = make_test_channel_name("health.producer_dereg");

            {
                ProducerOptions opts;
                opts.channel_name = ch_name;
                opts.pattern      = ChannelPattern::PubSub;
                opts.has_shm      = false;
                opts.timeout_ms   = 3000;

                auto producer_a = Producer::create(messenger, opts);
                ASSERT_TRUE(producer_a.has_value()) << "Producer A create failed";

                // Explicitly close: sends DEREG_REQ to broker immediately.
                producer_a->close();

                // Small delay for broker to process DEREG.
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }

            // Now a second producer should be able to register the same channel immediately.
            {
                ProducerOptions opts;
                opts.channel_name = ch_name;
                opts.pattern      = ChannelPattern::PubSub;
                opts.has_shm      = false;
                opts.timeout_ms   = 3000;

                auto producer_b = Producer::create(messenger, opts);
                EXPECT_TRUE(producer_b.has_value())
                    << "Producer B failed to register — DEREG_REQ was not processed";

                if (producer_b.has_value())
                    producer_b->close();
            }

            messenger.disconnect();
            broker.stop_and_join();
        },
        "broker_health.producer_auto_deregisters",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// dead_consumer_orchestrator
// ============================================================================

int dead_consumer_orchestrator(int argc, char **argv)
{
    if (argc < 3)
    {
        fmt::print(stderr, "ERROR: dead_consumer_orchestrator requires argv[2]: temp_file\n");
        return 1;
    }
    const std::string tmp_file = argv[2];

    return run_gtest_worker(
        [&tmp_file]() {
            // Broker with 1s liveness check interval.
            BrokerService::Config cfg;
            cfg.endpoint                         = "tcp://127.0.0.1:0";
            cfg.use_curve                        = true;
            cfg.channel_timeout                  = std::chrono::seconds(30); // long
            cfg.consumer_liveness_check_interval = std::chrono::seconds(1);  // check every 1s
            auto broker = start_broker_with_cfg(std::move(cfg));

            Messenger &messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(broker.endpoint, broker.pubkey));

            const std::string ch_name = make_test_channel_name("health.dead_consumer");

            ProducerOptions popts;
            popts.channel_name = ch_name;
            popts.pattern      = ChannelPattern::PubSub;
            popts.has_shm      = false;
            popts.timeout_ms   = 3000;

            auto producer = Producer::create(messenger, popts);
            ASSERT_TRUE(producer.has_value()) << "Producer::create failed";
            ASSERT_TRUE(producer->start());

            std::atomic<bool> consumer_died{false};
            producer->on_consumer_died([&](uint64_t /*pid*/, const std::string & /*reason*/) {
                consumer_died.store(true);
            });

            // Write endpoint + pubkey + channel_name to temp file for the exiter.
            {
                std::ofstream f(tmp_file);
                ASSERT_TRUE(f.is_open()) << "Failed to open temp file: " << tmp_file;
                f << broker.endpoint << "\n"
                  << broker.pubkey   << "\n"
                  << ch_name         << "\n";
            }

            // Signal the parent test that we are ready for the exiter to be spawned.
            signal_test_ready();

            // The exiter connects via Consumer::connect() (synchronous CONSUMER_REG_REQ)
            // then calls _exit(0). ZMQ HELLO to the producer peer_thread may not arrive
            // (process exits before delivery), but broker DOES register via REG_REQ.
            // Allow up to 2s for the exiter to have connected and died.
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));

            // Wait for CONSUMER_DIED_NOTIFY (broker needs to detect dead PID, up to ~3s).
            const auto died_deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!consumer_died.load() &&
                   std::chrono::steady_clock::now() < died_deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            EXPECT_TRUE(consumer_died.load())
                << "CONSUMER_DIED_NOTIFY was not received within 5s after exiter died";

            producer->close();
            messenger.disconnect();
            broker.stop_and_join();
        },
        "broker_health.dead_consumer_orchestrator",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// dead_consumer_exiter
// ============================================================================

int dead_consumer_exiter(int argc, char **argv)
{
    if (argc < 3)
    {
        fmt::print(stderr, "ERROR: dead_consumer_exiter requires argv[2]: temp_file\n");
        return 1;
    }
    const std::string tmp_file = argv[2];

    return run_gtest_worker(
        [&tmp_file]() {
            // Read broker connection info from temp file.
            std::string endpoint;
            std::string pubkey;
            std::string ch_name;
            {
                std::ifstream f(tmp_file);
                ASSERT_TRUE(f.is_open()) << "Exiter: cannot open temp file: " << tmp_file;
                std::getline(f, endpoint);
                std::getline(f, pubkey);
                std::getline(f, ch_name);
            }
            ASSERT_FALSE(endpoint.empty()) << "Exiter: endpoint is empty";
            ASSERT_FALSE(ch_name.empty())  << "Exiter: channel name is empty";

            Messenger &messenger = Messenger::get_instance();
            ASSERT_TRUE(messenger.connect(endpoint, pubkey));

            ConsumerOptions copts;
            copts.channel_name = ch_name;
            copts.timeout_ms   = 5000;

            auto consumer = Consumer::connect(messenger, copts);
            ASSERT_TRUE(consumer.has_value()) << "Exiter: Consumer::connect failed";

            // Crash simulation: _exit(0) skips all C++ destructors.
            // No BYE, no CONSUMER_DEREG_REQ — broker must detect dead PID.
            _exit(0);
        },
        "broker_health.dead_consumer_exiter",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// schema_mismatch_notify
// ============================================================================

int schema_mismatch_notify(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            auto broker = start_broker();

            // Messenger A: lifecycle-managed singleton (owns channel).
            Messenger &messenger_a = Messenger::get_instance();
            ASSERT_TRUE(messenger_a.connect(broker.endpoint, broker.pubkey));

            const std::string ch_name = make_test_channel_name("health.schema_mismatch");

            ProducerOptions opts_a;
            opts_a.channel_name    = ch_name;
            opts_a.pattern         = ChannelPattern::PubSub;
            opts_a.has_shm         = false;
            opts_a.schema_hash     = "aabbccdd";
            opts_a.schema_version  = 1;
            opts_a.timeout_ms      = 3000;

            auto producer_a = Producer::create(messenger_a, opts_a);
            ASSERT_TRUE(producer_a.has_value()) << "Producer A create failed";

            std::atomic<bool> error_fired{false};
            producer_a->on_channel_error(
                [&](const std::string &event, const nlohmann::json & /*details*/) {
                    if (event == "schema_mismatch_attempt")
                        error_fired.store(true);
                });

            // Messenger B: a second manual Messenger instance.
            // Connects to same broker, attempts conflicting channel registration.
            Messenger messenger_b;
            ASSERT_TRUE(messenger_b.connect(broker.endpoint, broker.pubkey));

            ProducerOptions opts_b;
            opts_b.channel_name   = ch_name;     // same channel
            opts_b.pattern        = ChannelPattern::PubSub;
            opts_b.has_shm        = false;
            opts_b.schema_hash    = "11223344";   // DIFFERENT schema hash
            opts_b.schema_version = 1;
            opts_b.timeout_ms     = 3000;

            // This must fail (broker rejects conflicting registration).
            auto producer_b = Producer::create(messenger_b, opts_b);
            EXPECT_FALSE(producer_b.has_value())
                << "Producer B should have been rejected due to schema mismatch";

            // Wait up to 3s for CHANNEL_ERROR_NOTIFY to reach producer A.
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!error_fired.load() &&
                   std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            EXPECT_TRUE(error_fired.load())
                << "CHANNEL_ERROR_NOTIFY(schema_mismatch_attempt) was not received within 3s";

            producer_a->close();
            messenger_b.disconnect();
            messenger_a.disconnect();
            broker.stop_and_join();
        },
        "broker_health.schema_mismatch_notify",
        logger_module(), crypto_module(), hub_module());
}

} // namespace pylabhub::tests::worker::broker_health

// ============================================================================
// Worker dispatcher registrar
// ============================================================================

namespace
{

struct BrokerHealthWorkerRegistrar
{
    BrokerHealthWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                const auto       dot  = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "broker_health")
                {
                    return -1;
                }
                std::string scenario(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::broker_health;
                if (scenario == "producer_gets_closing_notify")
                    return producer_gets_closing_notify(argc, argv);
                if (scenario == "consumer_auto_deregisters")
                    return consumer_auto_deregisters(argc, argv);
                if (scenario == "producer_auto_deregisters")
                    return producer_auto_deregisters(argc, argv);
                if (scenario == "dead_consumer_orchestrator")
                    return dead_consumer_orchestrator(argc, argv);
                if (scenario == "dead_consumer_exiter")
                    return dead_consumer_exiter(argc, argv);
                if (scenario == "schema_mismatch_notify")
                    return schema_mismatch_notify(argc, argv);
                fmt::print(stderr, "ERROR: Unknown broker_health scenario '{}'\n", scenario);
                return 1;
            });
    }
};

static BrokerHealthWorkerRegistrar g_broker_health_registrar; // NOLINT(cert-err58-cpp)

} // anonymous namespace
