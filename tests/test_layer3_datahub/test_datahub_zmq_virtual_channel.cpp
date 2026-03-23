/**
 * @file test_datahub_zmq_virtual_channel.cpp
 * @brief ZMQ Virtual Channel Node protocol tests (HEP-CORE-0021).
 *
 * Suite: ZmqVirtualChannelTest
 *
 * Tests the full round-trip of the ZMQ Virtual Channel Node feature:
 *   - REG_REQ with data_transport="zmq" + zmq_node_endpoint stored by broker
 *   - DISC_ACK returns data_transport + zmq_node_endpoint to consumer
 *   - ConsumerInfo carries the discovered transport fields
 *   - ChannelHandle::data_transport() / zmq_node_endpoint() accessors
 *   - hub::Producer ProducerOptions::data_transport/zmq_node_endpoint round-trip
 *   - hub::Consumer::data_transport() / zmq_node_endpoint() accessors
 *   - Default transport ("shm") when fields are omitted at REG_REQ
 *   - ZMQ transport consumer can discover endpoint without config-file hardcoding
 *
 * Tests 1–12 use the in-process LocalBrokerHandle pattern, protocol-level only.
 * Tests 13–17 create actual ZMQ PUSH/PULL sockets via hub::Producer/Consumer.
 *
 * CTest safety: all channel names are suffixed with the test process PID.
 */
#include "plh_datahub.hpp"
#include "plh_service.hpp"
#include "utils/broker_service.hpp"
#include "utils/hub_consumer.hpp"
#include "utils/hub_producer.hpp"
#include "utils/messenger.hpp"

#include <array>
#include <cstring>
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

// ── LocalBrokerHandle — in-process broker ────────────────────────────────────

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

std::string pid_chan(const std::string &base)
{
    return base + "." + std::to_string(getpid());
}

} // anonymous namespace

// ============================================================================
// ZmqVirtualChannelTest — in-process broker, protocol-level tests only
// ============================================================================

class ZmqVirtualChannelTest : public ::testing::Test
{
  public:
    static void SetUpTestSuite()
    {
        s_lifecycle_ = std::make_unique<pylabhub::utils::LifecycleGuard>(
            pylabhub::utils::MakeModDefList(
                pylabhub::utils::Logger::GetLifecycleModule(),
                pylabhub::crypto::GetLifecycleModule(),
                pylabhub::hub::GetLifecycleModule()), std::source_location::current());
    }
    static void TearDownTestSuite() { s_lifecycle_.reset(); }

  protected:
    void SetUp() override
    {
        BrokerService::Config cfg;
        cfg.endpoint           = "tcp://127.0.0.1:0"; // ephemeral port
        cfg.schema_search_dirs = {};
        broker_.emplace(start_local_broker(std::move(cfg)));
    }

    const std::string &ep() const { return broker_->endpoint; }
    const std::string &pk() const { return broker_->pubkey; }

    std::optional<LocalBrokerHandle> broker_;

  private:
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    static std::unique_ptr<pylabhub::utils::LifecycleGuard> s_lifecycle_;
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::unique_ptr<pylabhub::utils::LifecycleGuard> ZmqVirtualChannelTest::s_lifecycle_;

// ── Test 1: Default transport is "shm" when fields are omitted ───────────────

TEST_F(ZmqVirtualChannelTest, DefaultTransport_IsShm)
{
    // Register a channel without specifying data_transport.
    // The consumer's discover_producer must return data_transport="shm".
    const std::string channel = pid_chan("zmqvc.default.shm");

    Messenger prod_m;
    ASSERT_TRUE(prod_m.connect(ep(), pk()));
    auto handle = prod_m.create_channel(channel, {.timeout_ms = 3000});
    ASSERT_TRUE(handle.has_value()) << "create_channel failed";

    // Consumer discovers transport.
    Messenger cons_m;
    ASSERT_TRUE(cons_m.connect(ep(), pk()));
    auto info = cons_m.discover_producer(channel, 3000);
    ASSERT_TRUE(info.has_value()) << "discover_producer returned nullopt";

    EXPECT_EQ(info->data_transport, "shm");
    EXPECT_TRUE(info->zmq_node_endpoint.empty());
}

// ── Test 2: ZMQ transport stored in REG_REQ and returned in DISC_ACK ─────────

TEST_F(ZmqVirtualChannelTest, ZmqTransport_RoundTrip_DiscoverProducer)
{
    const std::string channel  = pid_chan("zmqvc.zmq.roundtrip");
    const std::string endpoint = "tcp://127.0.0.1:5590";

    Messenger prod_m;
    ASSERT_TRUE(prod_m.connect(ep(), pk()));
    auto handle = prod_m.create_channel(channel,
                                        {.timeout_ms = 3000,
                                         .data_transport = "zmq",
                                         .zmq_node_endpoint = endpoint});
    ASSERT_TRUE(handle.has_value()) << "create_channel failed";

    // Consumer discovers transport fields from broker.
    Messenger cons_m;
    ASSERT_TRUE(cons_m.connect(ep(), pk()));
    auto info = cons_m.discover_producer(channel, 3000);
    ASSERT_TRUE(info.has_value()) << "discover_producer returned nullopt";

    EXPECT_EQ(info->data_transport, "zmq") << "data_transport not stored/returned";
    EXPECT_EQ(info->zmq_node_endpoint, endpoint) << "zmq_node_endpoint not stored/returned";
}

// ── Test 3: ChannelHandle accessors don't crash on producer-side handle ───────

TEST_F(ZmqVirtualChannelTest, ChannelHandle_ZmqTransport_Accessors)
{
    const std::string channel  = pid_chan("zmqvc.handle.accessors");
    const std::string endpoint = "tcp://127.0.0.1:5591";

    Messenger m;
    ASSERT_TRUE(m.connect(ep(), pk()));
    auto handle = m.create_channel(channel,
                                   {.timeout_ms = 3000,
                                    .data_transport = "zmq",
                                    .zmq_node_endpoint = endpoint});
    ASSERT_TRUE(handle.has_value()) << "create_channel failed";

    // Producer-side handle accessors must not crash and return valid strings.
    EXPECT_NO_THROW((void)handle->data_transport());
    EXPECT_NO_THROW((void)handle->zmq_node_endpoint());
}

// ── Test 4: Consumer-side ChannelHandle carries discovered ZMQ transport ──────

TEST_F(ZmqVirtualChannelTest, ConsumerHandle_ZmqTransport_FromDisc)
{
    const std::string channel  = pid_chan("zmqvc.cons.handle");
    const std::string endpoint = "tcp://127.0.0.1:5592";

    // Producer registers with ZMQ transport.
    Messenger prod_m;
    ASSERT_TRUE(prod_m.connect(ep(), pk()));
    auto prod_handle = prod_m.create_channel(channel,
                                             {.timeout_ms = 3000,
                                              .data_transport = "zmq",
                                              .zmq_node_endpoint = endpoint});
    ASSERT_TRUE(prod_handle.has_value()) << "Producer create_channel failed";

    // Consumer connects: broker DISC_ACK carries transport fields → ChannelHandle.
    Messenger cons_m;
    ASSERT_TRUE(cons_m.connect(ep(), pk()));
    auto cons_handle = cons_m.connect_channel(channel,
                                              /*timeout_ms=*/3000,
                                              /*schema_hash=*/{},
                                              /*consumer_uid=*/"TEST-CONS-ZMQVC-01",
                                              /*consumer_name=*/"test-zmqvc-consumer");
    ASSERT_TRUE(cons_handle.has_value()) << "Consumer connect_channel failed";

    EXPECT_EQ(cons_handle->data_transport(), "zmq")
        << "Consumer ChannelHandle must carry broker-discovered data_transport";
    EXPECT_EQ(cons_handle->zmq_node_endpoint(), endpoint)
        << "Consumer ChannelHandle must carry broker-discovered zmq_node_endpoint";
}

// ── Test 5: Consumer-side ChannelHandle for SHM transport ────────────────────

TEST_F(ZmqVirtualChannelTest, ConsumerHandle_ShmTransport_FromDisc)
{
    const std::string channel = pid_chan("zmqvc.cons.shm");

    // Producer registers with default (SHM) transport.
    Messenger prod_m;
    ASSERT_TRUE(prod_m.connect(ep(), pk()));
    auto prod_handle = prod_m.create_channel(channel, {.timeout_ms = 3000});
    ASSERT_TRUE(prod_handle.has_value()) << "Producer create_channel failed";

    Messenger cons_m;
    ASSERT_TRUE(cons_m.connect(ep(), pk()));
    auto cons_handle = cons_m.connect_channel(channel,
                                              /*timeout_ms=*/3000,
                                              /*schema_hash=*/{},
                                              /*consumer_uid=*/"TEST-CONS-ZMQVC-02",
                                              /*consumer_name=*/"test-zmqvc-shm-consumer");
    ASSERT_TRUE(cons_handle.has_value()) << "Consumer connect_channel failed";

    EXPECT_EQ(cons_handle->data_transport(), "shm")
        << "Default transport should be 'shm' on consumer handle";
    EXPECT_TRUE(cons_handle->zmq_node_endpoint().empty())
        << "SHM transport has no zmq_node_endpoint";
}

// ── Test 6: hub::Producer ProducerOptions transport fields round-trip ─────────

TEST_F(ZmqVirtualChannelTest, HubProducer_ZmqTransport_RoundTrip)
{
    const std::string channel  = pid_chan("zmqvc.hubprod.zmq");
    const std::string endpoint = "tcp://127.0.0.1:5593";

    Messenger prod_m;
    ASSERT_TRUE(prod_m.connect(ep(), pk()));

    ProducerOptions opts;
    opts.channel_name      = channel;
    opts.role_uid         = "PROD-ZMQVC-00000001";
    opts.role_name        = "zmqvc-producer";
    opts.has_shm           = false;
    opts.data_transport    = "zmq";
    opts.zmq_node_endpoint = endpoint;
    opts.zmq_schema        = {{"bytes", 1, 8}};

    auto prod = Producer::create(prod_m, opts);
    ASSERT_TRUE(prod.has_value()) << "Producer::create failed";

    // Consumer discovers transport via broker.
    Messenger cons_m;
    ASSERT_TRUE(cons_m.connect(ep(), pk()));
    auto info = cons_m.discover_producer(channel, 3000);
    ASSERT_TRUE(info.has_value()) << "discover_producer returned nullopt";

    EXPECT_EQ(info->data_transport, "zmq")
        << "Producer ZMQ transport must propagate to broker";
    EXPECT_EQ(info->zmq_node_endpoint, endpoint)
        << "Producer zmq_node_endpoint must propagate to broker";
}

// ── Test 7: hub::Consumer data_transport() / zmq_node_endpoint() accessors ────

TEST_F(ZmqVirtualChannelTest, HubConsumer_ZmqTransport_Accessors)
{
    const std::string channel  = pid_chan("zmqvc.hubcons.accessors");
    const std::string endpoint = "tcp://127.0.0.1:5594";

    // Register ZMQ channel via Messenger directly.
    Messenger prod_m;
    ASSERT_TRUE(prod_m.connect(ep(), pk()));
    auto prod_handle = prod_m.create_channel(channel,
                                             {.timeout_ms = 3000,
                                              .data_transport = "zmq",
                                              .zmq_node_endpoint = endpoint});
    ASSERT_TRUE(prod_handle.has_value()) << "create_channel failed";

    // Consumer connects via hub::Consumer.
    Messenger cons_m;
    ASSERT_TRUE(cons_m.connect(ep(), pk()));

    ConsumerOptions co;
    co.channel_name  = channel;
    co.consumer_uid  = "CONS-ZMQVC-00000001";
    co.consumer_name = "zmqvc-consumer";
    co.zmq_schema    = {{"bytes", 1, 8}};

    auto consumer = Consumer::connect(cons_m, co);
    ASSERT_TRUE(consumer.has_value()) << "Consumer::connect failed";

    EXPECT_EQ(consumer->data_transport(), "zmq")
        << "hub::Consumer::data_transport() must return broker-discovered transport";
    EXPECT_EQ(consumer->zmq_node_endpoint(), endpoint)
        << "hub::Consumer::zmq_node_endpoint() must return broker-discovered endpoint";
}

// ── Test 8: hub::Consumer defaults to "shm" when producer uses SHM ───────────

TEST_F(ZmqVirtualChannelTest, HubConsumer_DefaultTransport_IsShm)
{
    const std::string channel = pid_chan("zmqvc.hubcons.shm");

    Messenger prod_m;
    ASSERT_TRUE(prod_m.connect(ep(), pk()));
    auto prod_handle = prod_m.create_channel(channel, {.timeout_ms = 3000});
    ASSERT_TRUE(prod_handle.has_value()) << "create_channel failed";

    Messenger cons_m;
    ASSERT_TRUE(cons_m.connect(ep(), pk()));

    ConsumerOptions co;
    co.channel_name  = channel;
    co.consumer_uid  = "CONS-ZMQVC-00000002";
    co.consumer_name = "zmqvc-consumer-shm";

    auto consumer = Consumer::connect(cons_m, co);
    ASSERT_TRUE(consumer.has_value()) << "Consumer::connect failed";

    EXPECT_EQ(consumer->data_transport(), "shm")
        << "Default transport should be 'shm'";
    EXPECT_TRUE(consumer->zmq_node_endpoint().empty())
        << "SHM transport: no zmq_node_endpoint";
}

// ── Test 9: Two consumers discover same ZMQ endpoint ─────────────────────────

TEST_F(ZmqVirtualChannelTest, MultipleConsumers_DiscoverSameEndpoint)
{
    const std::string channel  = pid_chan("zmqvc.multi.cons");
    const std::string endpoint = "tcp://127.0.0.1:5595";

    Messenger prod_m;
    ASSERT_TRUE(prod_m.connect(ep(), pk()));
    auto prod_handle = prod_m.create_channel(channel,
                                             {.timeout_ms = 3000,
                                              .data_transport = "zmq",
                                              .zmq_node_endpoint = endpoint});
    ASSERT_TRUE(prod_handle.has_value()) << "create_channel failed";

    // Two independent consumers discover the same endpoint.
    for (int i = 0; i < 2; ++i)
    {
        Messenger cons_m;
        ASSERT_TRUE(cons_m.connect(ep(), pk()));
        auto info = cons_m.discover_producer(channel, 3000);
        ASSERT_TRUE(info.has_value()) << "discover_producer[" << i << "] returned nullopt";
        EXPECT_EQ(info->data_transport, "zmq") << "consumer[" << i << "]: wrong transport";
        EXPECT_EQ(info->zmq_node_endpoint, endpoint)
            << "consumer[" << i << "]: wrong endpoint";
    }
}

// ── Test 10: ZMQ transport with empty endpoint is stored as-is ───────────────

TEST_F(ZmqVirtualChannelTest, ZmqTransport_EmptyEndpoint_StoredAsIs)
{
    // Registering with data_transport="zmq" and empty endpoint is protocol-valid
    // (the ProcessorScriptHost validates this at application level, not broker).
    const std::string channel = pid_chan("zmqvc.empty.endpoint");

    Messenger prod_m;
    ASSERT_TRUE(prod_m.connect(ep(), pk()));
    auto handle = prod_m.create_channel(channel,
                                        {.timeout_ms = 3000, .data_transport = "zmq"});
    ASSERT_TRUE(handle.has_value()) << "create_channel with empty ZMQ endpoint should succeed";

    Messenger cons_m;
    ASSERT_TRUE(cons_m.connect(ep(), pk()));
    auto info = cons_m.discover_producer(channel, 3000);
    ASSERT_TRUE(info.has_value()) << "discover_producer returned nullopt";

    EXPECT_EQ(info->data_transport, "zmq");
    EXPECT_TRUE(info->zmq_node_endpoint.empty());
}

// ── Test 11: SHM and ZMQ channels coexist on same broker ─────────────────────

TEST_F(ZmqVirtualChannelTest, ShmAndZmq_ChannelsCoexist)
{
    const std::string shm_channel = pid_chan("zmqvc.coexist.shm");
    const std::string zmq_channel = pid_chan("zmqvc.coexist.zmq");
    const std::string endpoint    = "tcp://127.0.0.1:5597";

    Messenger m;
    ASSERT_TRUE(m.connect(ep(), pk()));

    // Register SHM channel.
    auto shm_handle = m.create_channel(shm_channel, {.timeout_ms = 3000});
    ASSERT_TRUE(shm_handle.has_value()) << "SHM create_channel failed";

    // Register ZMQ channel.
    auto zmq_handle = m.create_channel(zmq_channel,
                                       {.timeout_ms = 3000,
                                        .data_transport = "zmq",
                                        .zmq_node_endpoint = endpoint});
    ASSERT_TRUE(zmq_handle.has_value()) << "ZMQ create_channel failed";

    // Both channels discoverable with correct transports.
    Messenger cons_m;
    ASSERT_TRUE(cons_m.connect(ep(), pk()));

    auto shm_info = cons_m.discover_producer(shm_channel, 3000);
    ASSERT_TRUE(shm_info.has_value());
    EXPECT_EQ(shm_info->data_transport, "shm");
    EXPECT_TRUE(shm_info->zmq_node_endpoint.empty());

    auto zmq_info = cons_m.discover_producer(zmq_channel, 3000);
    ASSERT_TRUE(zmq_info.has_value());
    EXPECT_EQ(zmq_info->data_transport, "zmq");
    EXPECT_EQ(zmq_info->zmq_node_endpoint, endpoint);
}

// ── Test 12: channel_list includes channels with ZMQ transport ────────────────

TEST_F(ZmqVirtualChannelTest, ChannelList_IncludesZmqChannel)
{
    const std::string channel  = pid_chan("zmqvc.list.zmq");
    const std::string endpoint = "tcp://127.0.0.1:5598";

    Messenger m;
    ASSERT_TRUE(m.connect(ep(), pk()));
    auto handle = m.create_channel(channel,
                                   {.timeout_ms = 3000,
                                    .data_transport = "zmq",
                                    .zmq_node_endpoint = endpoint});
    ASSERT_TRUE(handle.has_value()) << "create_channel failed";

    // list_channels returns vector<json>; each entry has a "channel" field.
    auto channels = m.list_channels(3000);
    EXPECT_FALSE(channels.empty()) << "list_channels returned empty vector";

    bool found = false;
    for (const auto &entry : channels)
    {
        if (entry.value("name", "") == channel)
        {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "ZMQ channel '" << channel << "' not in channel list";
}

// ── Test 13: Producer::queue() non-null when data_transport=zmq ──────────────

TEST_F(ZmqVirtualChannelTest, HubProducer_ZmqTransport_QueueNonNull)
{
    const std::string channel  = pid_chan("zmqvc.prod.q.nonnull");
    const std::string endpoint = "tcp://127.0.0.1:15580";

    Messenger prod_m;
    ASSERT_TRUE(prod_m.connect(ep(), pk()));

    ProducerOptions opts;
    opts.channel_name      = channel;
    opts.role_uid         = "PROD-ZMQVC-00000002";
    opts.has_shm           = false;
    opts.data_transport    = "zmq";
    opts.zmq_node_endpoint = endpoint;
    opts.zmq_schema = {{"bytes", 1, 8}};

    auto prod = Producer::create(prod_m, opts);
    ASSERT_TRUE(prod.has_value()) << "Producer::create failed";

    EXPECT_NE(prod->queue(), nullptr)
        << "Producer::queue() must return non-null when data_transport=zmq";
}

// ── Test 14: Producer::queue() null when data_transport=shm ──────────────────

TEST_F(ZmqVirtualChannelTest, HubProducer_ShmTransport_QueueNull)
{
    const std::string channel = pid_chan("zmqvc.prod.q.null");

    Messenger prod_m;
    ASSERT_TRUE(prod_m.connect(ep(), pk()));

    ProducerOptions opts;
    opts.channel_name   = channel;
    opts.role_uid      = "PROD-ZMQVC-00000003";
    opts.has_shm        = false;
    opts.data_transport = "shm";

    auto prod = Producer::create(prod_m, opts);
    ASSERT_TRUE(prod.has_value()) << "Producer::create failed";

    EXPECT_EQ(prod->queue(), nullptr)
        << "Producer::queue() must be null when data_transport=shm";
}

// ── Test 15: Consumer::queue() non-null when data_transport=zmq ──────────────

TEST_F(ZmqVirtualChannelTest, HubConsumer_ZmqTransport_QueueNonNull)
{
    const std::string channel  = pid_chan("zmqvc.cons.q.nonnull");
    const std::string endpoint = "tcp://127.0.0.1:15581";

    Messenger prod_m;
    ASSERT_TRUE(prod_m.connect(ep(), pk()));
    auto prod_handle = prod_m.create_channel(channel,
                                             {.timeout_ms = 3000,
                                              .data_transport = "zmq",
                                              .zmq_node_endpoint = endpoint});
    ASSERT_TRUE(prod_handle.has_value()) << "create_channel failed";

    Messenger cons_m;
    ASSERT_TRUE(cons_m.connect(ep(), pk()));

    ConsumerOptions co;
    co.channel_name  = channel;
    co.consumer_uid  = "CONS-ZMQVC-00000003";
    co.consumer_name = "zmqvc-cons-q-nonnull";
    co.zmq_schema = {{"bytes", 1, 8}};

    auto consumer = Consumer::connect(cons_m, co);
    ASSERT_TRUE(consumer.has_value()) << "Consumer::connect failed";

    EXPECT_NE(consumer->queue(), nullptr)
        << "Consumer::queue() must return non-null when data_transport=zmq";
}

// ── Test 16: Consumer::queue() null when data_transport=shm ──────────────────

TEST_F(ZmqVirtualChannelTest, HubConsumer_ShmTransport_QueueNull)
{
    const std::string channel = pid_chan("zmqvc.cons.q.null");

    Messenger prod_m;
    ASSERT_TRUE(prod_m.connect(ep(), pk()));
    auto prod_handle = prod_m.create_channel(channel, {.timeout_ms = 3000});
    ASSERT_TRUE(prod_handle.has_value()) << "create_channel failed";

    Messenger cons_m;
    ASSERT_TRUE(cons_m.connect(ep(), pk()));

    ConsumerOptions co;
    co.channel_name  = channel;
    co.consumer_uid  = "CONS-ZMQVC-00000004";
    co.consumer_name = "zmqvc-cons-q-null";

    auto consumer = Consumer::connect(cons_m, co);
    ASSERT_TRUE(consumer.has_value()) << "Consumer::connect failed";

    EXPECT_EQ(consumer->queue(), nullptr)
        << "Consumer::queue() must be null when data_transport=shm";
}

// ── Test 17: ZMQ data flows Producer PUSH → Consumer PULL (end-to-end) ───────

TEST_F(ZmqVirtualChannelTest, ZmqTransport_ProducerToConsumer_DataFlow)
{
    const std::string channel  = pid_chan("zmqvc.e2e.flow");
    const std::string endpoint = "tcp://127.0.0.1:15582";
    constexpr size_t  kItemSz  = 8;

    // Producer creates ZMQ PUSH socket at endpoint.
    Messenger prod_m;
    ASSERT_TRUE(prod_m.connect(ep(), pk()));

    ProducerOptions po;
    po.channel_name      = channel;
    po.role_uid         = "PROD-ZMQVC-E2E-0001";
    po.has_shm           = false;
    po.data_transport    = "zmq";
    po.zmq_node_endpoint = endpoint;
    po.zmq_schema = {{"bytes", 1, static_cast<uint32_t>(kItemSz)}};

    auto prod = Producer::create(prod_m, po);
    ASSERT_TRUE(prod.has_value()) << "Producer::create failed";
    ASSERT_NE(prod->queue(), nullptr) << "Producer ZmqQueue must be non-null";

    // Consumer discovers endpoint via broker, creates ZMQ PULL socket.
    Messenger cons_m;
    ASSERT_TRUE(cons_m.connect(ep(), pk()));

    ConsumerOptions co;
    co.channel_name  = channel;
    co.consumer_uid  = "CONS-ZMQVC-E2E-0001";
    co.consumer_name = "zmqvc-e2e-consumer";
    co.zmq_schema = {{"bytes", 1, static_cast<uint32_t>(kItemSz)}};

    auto consumer = Consumer::connect(cons_m, co);
    ASSERT_TRUE(consumer.has_value()) << "Consumer::connect failed";
    ASSERT_NE(consumer->queue(), nullptr) << "Consumer ZmqQueue must be non-null";

    // Let ZMQ TCP connection establish before sending.
    std::this_thread::sleep_for(std::chrono::milliseconds{100});

    // Write 8 bytes via producer PUSH.
    ZmqQueue *push_q = prod->queue();
    void *wbuf = push_q->write_acquire(std::chrono::milliseconds{100});
    ASSERT_NE(wbuf, nullptr) << "write_acquire returned null";
    const std::array<uint8_t, kItemSz> payload{1, 2, 3, 4, 5, 6, 7, 8};
    std::memcpy(wbuf, payload.data(), kItemSz);
    push_q->write_commit();

    // Read via consumer PULL (up to 3 s for ZMQ connection establishment).
    ZmqQueue *pull_q = consumer->queue();
    const void *rbuf = pull_q->read_acquire(std::chrono::milliseconds{3000});
    ASSERT_NE(rbuf, nullptr) << "read_acquire timed out — data did not arrive";

    std::array<uint8_t, kItemSz> received{};
    std::memcpy(received.data(), rbuf, kItemSz);
    pull_q->read_release();

    EXPECT_EQ(received, payload) << "Received data differs from sent data";
}
