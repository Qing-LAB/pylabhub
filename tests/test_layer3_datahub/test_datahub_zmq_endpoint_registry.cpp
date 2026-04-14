/**
 * @file test_datahub_zmq_endpoint_registry.cpp
 * @brief ZMQ endpoint registry tests — broker as directory for ZMQ peer endpoints (HEP-CORE-0021).
 *
 * Suite: ZmqEndpointRegistryTest
 *
 * Tests the broker's ZMQ endpoint registry (HEP-CORE-0021): producers register
 * their ZMQ PUSH endpoint with the broker; consumers discover it via DISC_REQ
 * and connect peer-to-peer. The broker stores the endpoint string but never
 * touches the data stream itself.
 *
 * Scenarios covered:
 *   - Default transport is "shm" when not specified
 *   - ZMQ endpoint fields stored in REG_REQ, returned in DISC_ACK
 *   - Multiple consumers discover the same endpoint
 *   - SHM-backed and ZMQ-backed registrations coexist on the same broker
 *   - Endpoint update (ENDPOINT_UPDATE_REQ) reflected in subsequent discoveries
 *
 * All tests use in-process LocalBrokerHandle + BrcHandle pattern.
 */
#include "plh_datahub.hpp"
#include "plh_service.hpp"
#include "utils/broker_request_comm.hpp"
#include "utils/broker_service.hpp"

#include <atomic>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace pylabhub::utils;
using namespace pylabhub::hub;
using namespace pylabhub::broker;
using json = nlohmann::json;

namespace
{

struct LocalBrokerHandle
{
    std::unique_ptr<BrokerService> service;
    std::thread                    thread;
    std::string                    endpoint;
    std::string                    pubkey;

    LocalBrokerHandle() = default;
    LocalBrokerHandle(LocalBrokerHandle &&) noexcept = default;
    LocalBrokerHandle &operator=(LocalBrokerHandle &&) noexcept = default;
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

    auto svc     = std::make_unique<BrokerService>(std::move(cfg));
    auto raw_ptr = svc.get();
    std::thread t([raw_ptr] { raw_ptr->run(); });

    auto info = future.get();

    LocalBrokerHandle h;
    h.service  = std::move(svc);
    h.thread   = std::move(t);
    h.endpoint = info.first;
    h.pubkey   = info.second;
    return h;
}

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

std::string pid_chan(const std::string &base)
{
    return base + "." + std::to_string(getpid());
}

json make_reg_opts(const std::string &channel, const std::string &role_uid)
{
    json opts;
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

} // anonymous namespace

// ============================================================================
// ZmqEndpointRegistryTest fixture
// ============================================================================

class ZmqEndpointRegistryTest : public ::testing::Test
{
public:
    static void SetUpTestSuite()
    {
        s_lifecycle_ = std::make_unique<LifecycleGuard>(MakeModDefList(
            Logger::GetLifecycleModule(), pylabhub::crypto::GetLifecycleModule(),
            pylabhub::hub::GetZMQContextModule()), std::source_location::current());
    }
    static void TearDownTestSuite() { s_lifecycle_.reset(); }

protected:
    void SetUp() override
    {
        BrokerService::Config cfg;
        cfg.endpoint               = "tcp://127.0.0.1:0";
        cfg.schema_search_dirs     = {};
        cfg.grace_override         = std::chrono::milliseconds(0);
        broker_.emplace(start_local_broker(std::move(cfg)));
    }

    const std::string &ep() const { return broker_->endpoint; }
    const std::string &pk() const { return broker_->pubkey; }
    BrokerService     &svc() { return *broker_->service; }

    std::optional<LocalBrokerHandle> broker_;

private:
    static std::unique_ptr<LifecycleGuard> s_lifecycle_;
};

std::unique_ptr<LifecycleGuard> ZmqEndpointRegistryTest::s_lifecycle_;

// ── Default transport is "shm" ──────────────────────────────────────────────

TEST_F(ZmqEndpointRegistryTest, DefaultTransport_IsShm)
{
    const std::string channel = pid_chan("zmqvc.default.shm");
    const std::string uid     = "PROD-" + channel;

    BrcHandle bh;
    bh.start(ep(), pk(), uid);
    auto reg = bh.brc.register_channel(make_reg_opts(channel, uid), 3000);
    ASSERT_TRUE(reg.has_value());

    // Producer must heartbeat for channel to become Ready.
    // discover_channel retries on DISC_PENDING (HEP-CORE-0023 §2.2),
    // so the consumer does not need to wait here.
    bh.brc.send_heartbeat(channel, {});

    BrcHandle cons_bh;
    cons_bh.start(ep(), pk(), "CONS-" + channel);
    auto disc = cons_bh.brc.discover_channel(channel, {}, 3000);
    ASSERT_TRUE(disc.has_value());
    EXPECT_EQ(disc->value("data_transport", ""), "shm");

    cons_bh.stop();
    bh.stop();
}

// ── ZMQ transport round-trip through broker ─────────────────────────────────

TEST_F(ZmqEndpointRegistryTest, ZmqTransport_RoundTrip)
{
    const std::string channel = pid_chan("zmqvc.zmq.roundtrip");
    const std::string uid     = "PROD-" + channel;
    const std::string zmq_ep  = "tcp://127.0.0.1:55555";

    BrcHandle bh;
    bh.start(ep(), pk(), uid);

    auto opts = make_reg_opts(channel, uid);
    opts["data_transport"]    = "zmq";
    opts["zmq_node_endpoint"] = zmq_ep;
    auto reg = bh.brc.register_channel(opts, 3000);
    ASSERT_TRUE(reg.has_value());

    // Producer must heartbeat for channel to become Ready.
    // discover_channel retries on DISC_PENDING (HEP-CORE-0023 §2.2),
    // so the consumer does not need to wait here.
    bh.brc.send_heartbeat(channel, {});

    BrcHandle cons_bh;
    cons_bh.start(ep(), pk(), "CONS-" + channel);
    auto disc = cons_bh.brc.discover_channel(channel, {}, 3000);
    ASSERT_TRUE(disc.has_value());
    EXPECT_EQ(disc->value("data_transport", ""), "zmq");
    EXPECT_EQ(disc->value("zmq_node_endpoint", ""), zmq_ep);

    cons_bh.stop();
    bh.stop();
}

// ── Multiple consumers discover same endpoint ───────────────────────────────

TEST_F(ZmqEndpointRegistryTest, MultipleConsumers_DiscoverSameEndpoint)
{
    const std::string channel = pid_chan("zmqvc.multi.disc");
    const std::string uid     = "PROD-" + channel;
    const std::string zmq_ep  = "tcp://127.0.0.1:55556";

    BrcHandle bh;
    bh.start(ep(), pk(), uid);

    auto opts = make_reg_opts(channel, uid);
    opts["data_transport"]    = "zmq";
    opts["zmq_node_endpoint"] = zmq_ep;
    auto reg = bh.brc.register_channel(opts, 3000);
    ASSERT_TRUE(reg.has_value());

    // Heartbeat to trigger Ready transition; discover_channel retries on DISC_PENDING.
    bh.brc.send_heartbeat(channel, {});

    BrcHandle c1, c2;
    c1.start(ep(), pk(), "CONS1-" + channel);
    c2.start(ep(), pk(), "CONS2-" + channel);

    auto d1 = c1.brc.discover_channel(channel, {}, 3000);
    auto d2 = c2.brc.discover_channel(channel, {}, 3000);
    ASSERT_TRUE(d1.has_value());
    ASSERT_TRUE(d2.has_value());
    EXPECT_EQ(d1->value("zmq_node_endpoint", ""), zmq_ep);
    EXPECT_EQ(d2->value("zmq_node_endpoint", ""), zmq_ep);

    c2.stop();
    c1.stop();
    bh.stop();
}

// ── SHM and ZMQ registrations coexist ───────────────────────────────────────

TEST_F(ZmqEndpointRegistryTest, ShmAndZmq_Coexist)
{
    const std::string shm_ch = pid_chan("zmqvc.coexist.shm");
    const std::string zmq_ch = pid_chan("zmqvc.coexist.zmq");

    BrcHandle shm_bh;
    shm_bh.start(ep(), pk(), "PROD-" + shm_ch);
    auto shm_reg = shm_bh.brc.register_channel(make_reg_opts(shm_ch, "PROD-" + shm_ch), 3000);
    ASSERT_TRUE(shm_reg.has_value());

    BrcHandle zmq_bh;
    zmq_bh.start(ep(), pk(), "PROD-" + zmq_ch);
    auto zmq_opts = make_reg_opts(zmq_ch, "PROD-" + zmq_ch);
    zmq_opts["data_transport"]    = "zmq";
    zmq_opts["zmq_node_endpoint"] = "tcp://127.0.0.1:55557";
    auto zmq_reg = zmq_bh.brc.register_channel(zmq_opts, 3000);
    ASSERT_TRUE(zmq_reg.has_value());

    ChannelSnapshot snap = svc().query_channel_snapshot();
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
}

// ── Endpoint update reflected in discovery ──────────────────────────────────

TEST_F(ZmqEndpointRegistryTest, EndpointUpdate_ReflectedInDiscovery)
{
    const std::string channel    = pid_chan("zmqvc.ep.update");
    const std::string uid        = "PROD-" + channel;
    const std::string updated_ep = "tcp://127.0.0.1:44444";

    BrcHandle bh;
    bh.start(ep(), pk(), uid);

    auto opts = make_reg_opts(channel, uid);
    opts["data_transport"]    = "zmq";
    opts["zmq_node_endpoint"] = "tcp://127.0.0.1:0";
    auto reg = bh.brc.register_channel(opts, 3000);
    ASSERT_TRUE(reg.has_value());

    // Heartbeat → Ready, then send endpoint update.
    // Both are fire-and-forget; the broker processes them in order from the
    // ROUTER socket since they come from the same DEALER identity.
    bh.brc.send_heartbeat(channel, {});
    bh.brc.send_endpoint_update(channel, "zmq_node", updated_ep);

    // discover_channel retries on DISC_PENDING per HEP-CORE-0023 §2.2.
    // Since ENDPOINT_UPDATE_REQ was sent before this DISC, FIFO ordering on
    // the broker's ROUTER socket guarantees it is processed first.
    // However, the channel doesn't become Ready until after the first heartbeat
    // is processed. If discover_channel hits the broker BEFORE heartbeat,
    // it gets DISC_PENDING and retries — so the result still reflects the
    // updated endpoint.
    BrcHandle cons_bh;
    cons_bh.start(ep(), pk(), "CONS-" + channel);
    auto disc = cons_bh.brc.discover_channel(channel, {}, 5000);
    ASSERT_TRUE(disc.has_value()) << "DISC_REQ timed out";
    EXPECT_EQ(disc->value("zmq_node_endpoint", ""), updated_ep);

    cons_bh.stop();
    bh.stop();
}
