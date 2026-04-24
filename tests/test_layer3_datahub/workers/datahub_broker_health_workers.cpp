// tests/test_layer3_datahub/workers/datahub_broker_health_workers.cpp
//
// Broker health and notification tests via BrokerRequestComm.
#include "datahub_broker_health_workers.h"
#include "test_entrypoint.h"
#include "test_process_utils.h"
#include "shared_test_helpers.h"

#include "utils/broker_request_comm.hpp"
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
#include <string>
#include <thread>

using namespace pylabhub::tests::helper;
using namespace pylabhub::hub;
using pylabhub::broker::BrokerService;
using pylabhub::broker::ChannelSnapshot;

namespace pylabhub::tests::worker::broker_health
{

static auto logger_module() { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto crypto_module() { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto zmq_module()    { return ::pylabhub::hub::GetZMQContextModule(); }

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

/// BRC wrapper with poll thread — same as test helper BrcHandle pattern.
struct BrcHandle
{
    BrokerRequestComm brc;
    std::atomic<bool> running{true};
    std::thread       thread;

    void start(const std::string &ep, const std::string &pk, const std::string &uid)
    {
        BrokerRequestComm::Config cfg;
        cfg.broker_endpoint = ep;
        cfg.broker_pubkey   = pk;
        cfg.role_uid        = uid;
        ASSERT_TRUE(brc.connect(cfg));
        thread = std::thread([this] { brc.run_poll_loop([this] { return running.load(); }); });
    }

    void stop()
    {
        running.store(false);
        brc.stop();
        if (thread.joinable())
            thread.join();
        brc.disconnect();
    }

    ~BrcHandle()
    {
        if (thread.joinable())
            stop();
    }
};

nlohmann::json make_reg_opts(const std::string &channel, const std::string &role_uid)
{
    nlohmann::json opts;
    opts["channel_name"]      = channel;
    opts["pattern"]           = "PubSub";
    opts["has_shared_memory"] = false;
    opts["producer_pid"]      = ::getpid();
    opts["zmq_ctrl_endpoint"] = "tcp://127.0.0.1:0";
    opts["zmq_data_endpoint"] = "tcp://127.0.0.1:0";
    opts["zmq_pubkey"]        = "";
    opts["role_uid"]          = role_uid;
    opts["role_name"]         = "test_producer";
    return opts;
}

nlohmann::json make_cons_opts(const std::string &channel, const std::string &consumer_uid)
{
    nlohmann::json opts;
    opts["channel_name"]  = channel;
    opts["consumer_uid"]  = consumer_uid;
    opts["consumer_name"] = "test_consumer";
    opts["consumer_pid"]  = ::getpid();
    return opts;
}

} // anonymous namespace

// ============================================================================
// producer_gets_closing_notify
// ============================================================================

int producer_gets_closing_notify(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            BrokerService::Config cfg;
            cfg.endpoint        = "tcp://127.0.0.1:0";
            cfg.use_curve       = true;
            // Fast reclaim for test: total ~1s window.
            cfg.ready_timeout_override   = std::chrono::milliseconds(500);
            cfg.pending_timeout_override = std::chrono::milliseconds(500);
            cfg.consumer_liveness_check_interval = std::chrono::seconds(0);
            auto broker = start_broker_with_cfg(std::move(cfg));

            const std::string ch_name = make_test_channel_name("health.closing_notify");
            const std::string uid     = "prod." + ch_name;

            std::atomic<bool> closing_fired{false};

            BrcHandle bh;
            bh.brc.on_notification([&](const std::string &type, const nlohmann::json &)
            {
                if (type == "CHANNEL_CLOSING_NOTIFY")
                    closing_fired.store(true);
            });
            bh.start(broker.endpoint, broker.pubkey, uid);

            auto reg = bh.brc.register_channel(make_reg_opts(ch_name, uid), 3000);
            ASSERT_TRUE(reg.has_value()) << "register_channel failed";

            // Send one heartbeat to mark Ready, then stop sending.
            bh.brc.send_heartbeat(ch_name, {});

            // Wait ~1s for state machine to demote Ready -> Pending -> deregister + CHANNEL_CLOSING_NOTIFY.
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!closing_fired.load() &&
                   std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            EXPECT_TRUE(closing_fired.load())
                << "CHANNEL_CLOSING_NOTIFY was not received within 5s";

            bh.stop();
            broker.stop_and_join();
        },
        "broker_health.producer_gets_closing_notify",
        logger_module(), crypto_module(), zmq_module());
}

// ============================================================================
// consumer_auto_deregisters
// ============================================================================

int consumer_auto_deregisters(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            auto broker = start_broker();

            const std::string ch_name  = make_test_channel_name("health.consumer_dereg");
            const std::string prod_uid = "prod." + ch_name;
            const std::string cons_uid = "cons." + ch_name;

            // Register producer
            BrcHandle prod_bh;
            prod_bh.start(broker.endpoint, broker.pubkey, prod_uid);
            auto reg = prod_bh.brc.register_channel(make_reg_opts(ch_name, prod_uid), 3000);
            ASSERT_TRUE(reg.has_value());

            prod_bh.brc.send_heartbeat(ch_name, {});
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            // Register consumer
            BrcHandle cons_bh;
            cons_bh.start(broker.endpoint, broker.pubkey, cons_uid);
            auto cons_reg = cons_bh.brc.register_consumer(make_cons_opts(ch_name, cons_uid), 3000);
            ASSERT_TRUE(cons_reg.has_value());

            // Consumer deregisters
            EXPECT_TRUE(cons_bh.brc.deregister_consumer(ch_name));
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            // Verify consumer_count = 0 via admin snapshot
            ChannelSnapshot snap = broker.service->query_channel_snapshot();
            for (const auto &ch : snap.channels)
            {
                if (ch.name == ch_name)
                {
                    EXPECT_EQ(ch.consumer_count, 0)
                        << "Expected consumer_count=0 after deregister";
                }
            }

            cons_bh.stop();
            prod_bh.stop();
            broker.stop_and_join();
        },
        "broker_health.consumer_auto_deregisters",
        logger_module(), crypto_module(), zmq_module());
}

// ============================================================================
// producer_auto_deregisters
// ============================================================================

int producer_auto_deregisters(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            BrokerService::Config cfg;
            cfg.endpoint        = "tcp://127.0.0.1:0";
            cfg.use_curve       = true;
            cfg.ready_timeout_override   = std::chrono::milliseconds(15000);
            cfg.pending_timeout_override = std::chrono::milliseconds(15000);
            cfg.consumer_liveness_check_interval = std::chrono::seconds(0);
            auto broker = start_broker_with_cfg(std::move(cfg));

            const std::string ch_name = make_test_channel_name("health.producer_dereg");

            // Register producer A
            {
                BrcHandle bh;
                bh.start(broker.endpoint, broker.pubkey, "prod.a." + ch_name);
                auto reg = bh.brc.register_channel(make_reg_opts(ch_name, "prod.a." + ch_name), 3000);
                ASSERT_TRUE(reg.has_value());

                // Deregister
                EXPECT_TRUE(bh.brc.deregister_channel(ch_name));
                bh.stop();

                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }

            // Producer B should register the same channel immediately
            {
                BrcHandle bh;
                bh.start(broker.endpoint, broker.pubkey, "prod.b." + ch_name);
                auto reg = bh.brc.register_channel(make_reg_opts(ch_name, "prod.b." + ch_name), 3000);
                EXPECT_TRUE(reg.has_value())
                    << "Producer B failed to register — DEREG_REQ was not processed";
                bh.stop();
            }

            broker.stop_and_join();
        },
        "broker_health.producer_auto_deregisters",
        logger_module(), crypto_module(), zmq_module());
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
            BrokerService::Config cfg;
            cfg.endpoint                         = "tcp://127.0.0.1:0";
            cfg.use_curve                        = true;
            cfg.ready_timeout_override           = std::chrono::milliseconds(15000);
            cfg.pending_timeout_override         = std::chrono::milliseconds(15000);
            cfg.consumer_liveness_check_interval = std::chrono::seconds(1);
            auto broker = start_broker_with_cfg(std::move(cfg));

            const std::string ch_name  = make_test_channel_name("health.dead_consumer");
            const std::string prod_uid = "prod." + ch_name;

            std::atomic<bool> consumer_died{false};

            BrcHandle bh;
            bh.brc.on_notification([&](const std::string &type, const nlohmann::json &)
            {
                if (type == "CONSUMER_DIED_NOTIFY")
                    consumer_died.store(true);
            });
            bh.start(broker.endpoint, broker.pubkey, prod_uid);

            auto reg = bh.brc.register_channel(make_reg_opts(ch_name, prod_uid), 3000);
            ASSERT_TRUE(reg.has_value());

            bh.brc.send_heartbeat(ch_name, {});
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            // Write connection info for the exiter
            {
                std::ofstream f(tmp_file);
                ASSERT_TRUE(f.is_open()) << "Failed to open temp file: " << tmp_file;
                f << broker.endpoint << "\n"
                  << broker.pubkey   << "\n"
                  << ch_name         << "\n";
            }

            signal_test_ready();

            // Wait for the exiter to connect and die
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));

            // Wait for CONSUMER_DIED_NOTIFY (broker detects dead PID)
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!consumer_died.load() &&
                   std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            EXPECT_TRUE(consumer_died.load())
                << "CONSUMER_DIED_NOTIFY was not received within 5s after exiter died";

            bh.stop();
            broker.stop_and_join();
        },
        "broker_health.dead_consumer_orchestrator",
        logger_module(), crypto_module(), zmq_module());
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
            ASSERT_FALSE(endpoint.empty());
            ASSERT_FALSE(ch_name.empty());

            const std::string cons_uid = "cons.exiter." + ch_name;

            BrcHandle bh;
            bh.start(endpoint, pubkey, cons_uid);
            auto reg = bh.brc.register_consumer(make_cons_opts(ch_name, cons_uid), 5000);
            ASSERT_TRUE(reg.has_value()) << "Exiter: register_consumer failed";

            // Crash simulation: _exit(0) skips all destructors.
            // No CONSUMER_DEREG_REQ — broker must detect dead PID.
            _exit(0);
        },
        "broker_health.dead_consumer_exiter",
        logger_module(), crypto_module(), zmq_module());
}

// ============================================================================
// schema_mismatch_notify
// ============================================================================

int schema_mismatch_notify(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            auto broker = start_broker();

            const std::string ch_name  = make_test_channel_name("health.schema_mismatch");
            const std::string uid_a    = "prod.a." + ch_name;
            const std::string uid_b    = "prod.b." + ch_name;
            const std::string hash_a   = std::string(64, 'a');
            const std::string hash_b   = std::string(64, 'b');

            std::atomic<bool> error_fired{false};

            // Producer A registers with schema_hash A
            BrcHandle bh_a;
            bh_a.brc.on_notification([&](const std::string &type, const nlohmann::json &)
            {
                if (type == "CHANNEL_ERROR_NOTIFY")
                    error_fired.store(true);
            });
            bh_a.start(broker.endpoint, broker.pubkey, uid_a);

            auto opts_a = make_reg_opts(ch_name, uid_a);
            opts_a["schema_hash"] = hash_a;
            auto reg_a = bh_a.brc.register_channel(opts_a, 3000);
            ASSERT_TRUE(reg_a.has_value());

            // Producer B tries same channel with different schema_hash → rejected
            BrcHandle bh_b;
            bh_b.start(broker.endpoint, broker.pubkey, uid_b);

            auto opts_b = make_reg_opts(ch_name, uid_b);
            opts_b["schema_hash"] = hash_b;
            auto reg_b = bh_b.brc.register_channel(opts_b, 3000);
            EXPECT_FALSE(reg_b.has_value())
                << "Producer B should have been rejected due to schema mismatch";

            // Wait for CHANNEL_EVENT_NOTIFY to reach producer A
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!error_fired.load() &&
                   std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            EXPECT_TRUE(error_fired.load())
                << "CHANNEL_EVENT_NOTIFY (schema mismatch) was not received within 3s";

            bh_b.stop();
            bh_a.stop();
            broker.stop_and_join();
        },
        "broker_health.schema_mismatch_notify",
        logger_module(), crypto_module(), zmq_module());
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
