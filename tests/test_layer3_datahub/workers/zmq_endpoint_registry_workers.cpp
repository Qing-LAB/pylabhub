/**
 * @file zmq_endpoint_registry_workers.cpp
 * @brief Worker bodies for broker ZMQ endpoint registry tests
 *        (HEP-CORE-0021; Pattern 3).  Migrated 2026-05-13 from
 *        in-process `SetUpTestSuite`-owned `LifecycleGuard`.
 *
 * Each scenario stands up a single in-process `BrokerService` via
 * `start_local_broker` (raw HubState + thread; matches the original
 * fixture's LocalBrokerHandle shape — not the HEP-CORE-0033 HubHost
 * composite used by broker_consumer/broker_schema).
 */

#include "zmq_endpoint_registry_workers.h"

#include "plh_datahub.hpp"
#include "plh_service.hpp"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"
#include "utils/broker_request_comm.hpp"
#include "utils/broker_service.hpp"
#include "utils/hub_state.hpp"
#include "utils/logger.hpp"

#include <atomic>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <unistd.h>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace pylabhub::utils;
using namespace pylabhub::hub;
using namespace pylabhub::broker;
using pylabhub::tests::helper::run_gtest_worker;
using json = nlohmann::json;

namespace pylabhub::tests::worker
{
namespace zmq_endpoint_registry
{

namespace
{

struct LocalBrokerHandle
{
    std::unique_ptr<pylabhub::hub::HubState> hub_state;
    std::unique_ptr<BrokerService>           service;
    std::thread                              thread;
    std::string                              endpoint;
    std::string                              pubkey;

    LocalBrokerHandle()                                          = default;
    LocalBrokerHandle(LocalBrokerHandle &&) noexcept             = default;
    LocalBrokerHandle &operator=(LocalBrokerHandle &&) noexcept  = default;
    ~LocalBrokerHandle() { stop_and_join(); }

    void stop_and_join()
    {
        if (service)
        {
            service->stop();
            if (thread.joinable())
                thread.join();
        }
    }
};

LocalBrokerHandle start_local_broker(BrokerService::Config cfg)
{
    using ReadyInfo = std::pair<std::string, std::string>;
    auto promise = std::make_shared<std::promise<ReadyInfo>>();
    auto future  = promise->get_future();

    cfg.on_ready = [promise](const std::string &ep, const std::string &pk)
    { promise->set_value({ep, pk}); };

    auto state   = std::make_unique<pylabhub::hub::HubState>();
    auto svc     = std::make_unique<BrokerService>(std::move(cfg), *state);
    auto raw_ptr = svc.get();
    std::thread t([raw_ptr] { raw_ptr->run(); });

    auto info = future.get();

    LocalBrokerHandle h;
    h.hub_state = std::move(state);
    h.service   = std::move(svc);
    h.thread    = std::move(t);
    h.endpoint  = info.first;
    h.pubkey    = info.second;
    return h;
}

struct BrcHandle
{
    BrokerRequestComm brc;
    std::atomic<bool> running{true};
    std::thread       thread;

    void start(const std::string &ep, const std::string &pk,
               const std::string &uid)
    {
        BrokerRequestComm::Config cfg;
        cfg.broker_endpoint = ep;
        cfg.broker_pubkey   = pk;
        cfg.role_uid        = uid;
        ASSERT_TRUE(brc.connect(cfg));
        thread = std::thread([this] {
            brc.run_poll_loop([this] { return running.load(); });
        });
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

std::string pid_chan(const std::string &base)
{
    return base + ".pid" + std::to_string(::getpid());
}

json make_reg_opts(const std::string &channel, const std::string &role_uid)
{
    json opts;
    opts["channel_name"]      = channel;
    opts["pattern"]           = "PubSub";
    opts["has_shared_memory"] = false;
    opts["producer_pid"]      = ::getpid();
    opts["role_uid"]          = role_uid;
    opts["role_name"]         = "test_producer";
    return opts;
}

template <typename Body>
int run_with_broker(std::string_view worker_name, Body &&body)
{
    return run_gtest_worker(
        [body = std::forward<Body>(body)]() mutable {
            BrokerService::Config cfg;
            cfg.endpoint           = "tcp://127.0.0.1:0";
            cfg.schema_search_dirs = {};
            auto broker            = start_local_broker(std::move(cfg));
            body(broker);
        },
        std::string(worker_name).c_str(),
        Logger::GetLifecycleModule(),
        pylabhub::crypto::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

} // namespace

int default_transport_is_shm()
{
    return run_with_broker(
        "zmq_endpoint_registry::default_transport_is_shm",
        [](LocalBrokerHandle &b) {
            const std::string channel = pid_chan("zmqvc.default.shm");
            const std::string uid     = "prod.ep." + channel;

            BrcHandle bh;
            bh.start(b.endpoint, b.pubkey, uid);
            auto reg = bh.brc.register_channel(make_reg_opts(channel, uid), 3000);
            ASSERT_TRUE(reg.has_value());

            bh.brc.send_heartbeat(channel, uid, "producer", {});

            BrcHandle cons_bh;
            cons_bh.start(b.endpoint, b.pubkey, "cons.ep." + channel);
            auto disc = cons_bh.brc.discover_channel(channel, {}, 3000);
            ASSERT_TRUE(disc.has_value());
            EXPECT_EQ(disc->value("data_transport", ""), "shm");

            cons_bh.stop();
            bh.stop();
        });
}

int zmq_transport_round_trip()
{
    return run_with_broker(
        "zmq_endpoint_registry::zmq_transport_round_trip",
        [](LocalBrokerHandle &b) {
            const std::string channel = pid_chan("zmqvc.zmq.roundtrip");
            const std::string uid     = "prod.ep." + channel;
            const std::string zmq_ep  = "tcp://127.0.0.1:55555";

            BrcHandle bh;
            bh.start(b.endpoint, b.pubkey, uid);

            auto opts                 = make_reg_opts(channel, uid);
            opts["data_transport"]    = "zmq";
            opts["zmq_node_endpoint"] = zmq_ep;
            auto reg = bh.brc.register_channel(opts, 3000);
            ASSERT_TRUE(reg.has_value());

            bh.brc.send_heartbeat(channel, uid, "producer", {});

            BrcHandle cons_bh;
            cons_bh.start(b.endpoint, b.pubkey, "cons.ep." + channel);
            auto disc = cons_bh.brc.discover_channel(channel, {}, 3000);
            ASSERT_TRUE(disc.has_value());
            EXPECT_EQ(disc->value("data_transport", ""), "zmq");
            EXPECT_EQ(disc->value("zmq_node_endpoint", ""), zmq_ep);

            cons_bh.stop();
            bh.stop();
        });
}

int multiple_consumers_discover_same_endpoint()
{
    return run_with_broker(
        "zmq_endpoint_registry::multiple_consumers_discover_same_endpoint",
        [](LocalBrokerHandle &b) {
            const std::string channel = pid_chan("zmqvc.multi.disc");
            const std::string uid     = "prod.ep." + channel;
            const std::string zmq_ep  = "tcp://127.0.0.1:55556";

            BrcHandle bh;
            bh.start(b.endpoint, b.pubkey, uid);

            auto opts                 = make_reg_opts(channel, uid);
            opts["data_transport"]    = "zmq";
            opts["zmq_node_endpoint"] = zmq_ep;
            auto reg = bh.brc.register_channel(opts, 3000);
            ASSERT_TRUE(reg.has_value());

            bh.brc.send_heartbeat(channel, uid, "producer", {});

            BrcHandle c1, c2;
            c1.start(b.endpoint, b.pubkey, "CONS1-" + channel);
            c2.start(b.endpoint, b.pubkey, "CONS2-" + channel);

            auto d1 = c1.brc.discover_channel(channel, {}, 3000);
            auto d2 = c2.brc.discover_channel(channel, {}, 3000);
            ASSERT_TRUE(d1.has_value());
            ASSERT_TRUE(d2.has_value());
            EXPECT_EQ(d1->value("zmq_node_endpoint", ""), zmq_ep);
            EXPECT_EQ(d2->value("zmq_node_endpoint", ""), zmq_ep);

            c2.stop();
            c1.stop();
            bh.stop();
        });
}

int shm_and_zmq_coexist()
{
    return run_with_broker(
        "zmq_endpoint_registry::shm_and_zmq_coexist",
        [](LocalBrokerHandle &b) {
            const std::string shm_ch = pid_chan("zmqvc.coexist.shm");
            const std::string zmq_ch = pid_chan("zmqvc.coexist.zmq");

            BrcHandle shm_bh;
            shm_bh.start(b.endpoint, b.pubkey, "prod.shm." + shm_ch);
            auto shm_reg = shm_bh.brc.register_channel(
                make_reg_opts(shm_ch, "prod.shm." + shm_ch), 3000);
            ASSERT_TRUE(shm_reg.has_value());

            BrcHandle zmq_bh;
            zmq_bh.start(b.endpoint, b.pubkey, "prod." + zmq_ch);
            auto zmq_opts                 = make_reg_opts(zmq_ch, "prod." + zmq_ch);
            zmq_opts["data_transport"]    = "zmq";
            zmq_opts["zmq_node_endpoint"] = "tcp://127.0.0.1:55557";
            auto zmq_reg = zmq_bh.brc.register_channel(zmq_opts, 3000);
            ASSERT_TRUE(zmq_reg.has_value());

            ChannelSnapshot snap = b.service->query_channel_snapshot();
            bool found_shm = false, found_zmq = false;
            for (const auto &ch : snap.channels)
            {
                if (ch.name == shm_ch) found_shm = true;
                if (ch.name == zmq_ch) found_zmq = true;
            }
            EXPECT_TRUE(found_shm);
            EXPECT_TRUE(found_zmq);

            zmq_bh.stop();
            shm_bh.stop();
        });
}

int endpoint_update_reflected_in_discovery()
{
    return run_with_broker(
        "zmq_endpoint_registry::endpoint_update_reflected_in_discovery",
        [](LocalBrokerHandle &b) {
            const std::string channel    = pid_chan("zmqvc.ep.update");
            const std::string uid        = "prod.ep." + channel;
            const std::string updated_ep = "tcp://127.0.0.1:44444";

            BrcHandle bh;
            bh.start(b.endpoint, b.pubkey, uid);

            auto opts                 = make_reg_opts(channel, uid);
            opts["data_transport"]    = "zmq";
            opts["zmq_node_endpoint"] = "tcp://127.0.0.1:0";
            auto reg = bh.brc.register_channel(opts, 3000);
            ASSERT_TRUE(reg.has_value());

            bh.brc.send_heartbeat(channel, uid, "producer", {});
            bh.brc.send_endpoint_update(channel, "zmq_node", updated_ep);

            BrcHandle cons_bh;
            cons_bh.start(b.endpoint, b.pubkey, "cons.ep." + channel);
            auto disc = cons_bh.brc.discover_channel(channel, {}, 5000);
            ASSERT_TRUE(disc.has_value()) << "DISC_REQ timed out";
            EXPECT_EQ(disc->value("zmq_node_endpoint", ""), updated_ep);

            cons_bh.stop();
            bh.stop();
        });
}

} // namespace zmq_endpoint_registry
} // namespace pylabhub::tests::worker

// ── Dispatcher registrar ────────────────────────────────────────────────────

namespace
{

struct ZmqEndpointRegistryRegistrar
{
    ZmqEndpointRegistryRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2) return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "zmq_endpoint_registry")
                    return -1;
                std::string sc(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::zmq_endpoint_registry;

                if (sc == "default_transport_is_shm")
                    return default_transport_is_shm();
                if (sc == "zmq_transport_round_trip")
                    return zmq_transport_round_trip();
                if (sc == "multiple_consumers_discover_same_endpoint")
                    return multiple_consumers_discover_same_endpoint();
                if (sc == "shm_and_zmq_coexist")
                    return shm_and_zmq_coexist();
                if (sc == "endpoint_update_reflected_in_discovery")
                    return endpoint_update_reflected_in_discovery();
                return -1;
            });
    }
} g_registrar;

} // namespace
