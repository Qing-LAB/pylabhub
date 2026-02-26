/**
 * @file test_actor_embedded_mode.cpp
 * @brief Layer 4 tests for the embedded-mode Producer/Consumer API (HEP-CORE-0010 Phase 2).
 *
 * ## What is tested
 *
 * `start_embedded()`, `peer_ctrl_socket_handle()`, `data_zmq_socket_handle()`,
 * `ctrl_zmq_socket_handle()`, `handle_peer_events_nowait()`, and
 * `handle_ctrl_events_nowait()` — the API surface introduced by Phase 2 that
 * lets an external zmq_thread_ own all ZMQ socket I/O.
 *
 * ## Test infrastructure
 *
 * An in-process `BrokerService` (no CURVE encryption) is started in
 * `SetUpTestSuite` alongside a minimal ZMQ context.  No Logger lifecycle is
 * needed: the shared-library Logger checks its initialised state before
 * enqueuing and silently drops messages when not initialised — so LOGGER_*
 * calls inside BrokerService / Messenger are safe no-ops here.
 *
 * All broker/messenger/socket cleanup happens inside each test via explicit
 * stop() and close() calls, or RAII from the optional<> destructor.
 *
 * ## What is NOT tested here
 *
 * Full Python script lifecycle (on_init / on_iteration / on_stop) and
 * multi-threaded zmq_thread_ + loop_thread_ interaction are integration-level
 * concerns and are exercised by the Layer 3 datahub tests.
 */

#include "utils/broker_service.hpp"
#include "utils/hub_consumer.hpp"
#include "utils/hub_producer.hpp"
#include "utils/messenger.hpp"
#include "utils/zmq_context.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h> // getpid()

using namespace pylabhub::hub;
using pylabhub::broker::BrokerService;

// ============================================================================
// In-process broker handle (RAII)
// ============================================================================

namespace
{

struct BrokerHandle
{
    std::unique_ptr<BrokerService> service;
    std::thread                    thread;
    std::string                    endpoint;

    ~BrokerHandle()
    {
        if (service)
            service->stop();
        if (thread.joinable())
            thread.join();
    }

    BrokerHandle()                                 = default;
    BrokerHandle(const BrokerHandle &)             = delete;
    BrokerHandle &operator=(const BrokerHandle &)  = delete;
    BrokerHandle(BrokerHandle &&)                  = default;
    BrokerHandle &operator=(BrokerHandle &&)       = default;
};

/// Start an in-process broker on a random TCP port (no encryption).
/// Returns once the broker is ready to accept connections.
BrokerHandle start_local_broker()
{
    auto promise = std::make_shared<std::promise<std::string>>();
    auto future  = promise->get_future();

    BrokerService::Config cfg;
    cfg.endpoint  = "tcp://127.0.0.1:0"; // OS picks a free port
    cfg.use_curve = false;               // No crypto needed in unit tests
    cfg.on_ready  = [promise](const std::string &ep, const std::string & /*pk*/) {
        promise->set_value(ep);
    };

    auto service = std::make_unique<BrokerService>(std::move(cfg));
    auto *raw    = service.get();
    std::thread t([raw]() { raw->run(); });

    // Wait until the broker is listening (short timeout to fail fast in CI).
    auto status = future.wait_for(std::chrono::seconds(10));
    if (status != std::future_status::ready)
        throw std::runtime_error("test broker failed to start within 10s");

    std::string ep = future.get();
    BrokerHandle bh;
    bh.service  = std::move(service);
    bh.thread   = std::move(t);
    bh.endpoint = std::move(ep);
    return bh;
}

/// Make a channel name unique across parallel CTest processes.
std::string unique_channel(const char *base)
{
    return std::string(base) + "." + std::to_string(::getpid());
}

} // namespace

// ============================================================================
// Test fixture — shared broker + ZMQ context
// ============================================================================

class ActorEmbeddedModeTest : public ::testing::Test
{
  protected:
    static void SetUpTestSuite()
    {
        // Initialize the shared ZMQ context.  Logger is not started — Logger
        // calls inside the shared library silently return false (no crash).
        pylabhub::hub::zmq_context_startup();

        broker_ = new BrokerHandle(start_local_broker());
    }

    static void TearDownTestSuite()
    {
        delete broker_;
        broker_ = nullptr;
        pylabhub::hub::zmq_context_shutdown();
    }

    // Helper: create a Messenger connected to the test broker.
    static Messenger make_messenger()
    {
        Messenger m;
        bool ok = m.connect(broker_->endpoint); // no encryption
        (void)ok; // connection failure logged; tests will see nullopt from create()
        return m;
    }

    static BrokerHandle *broker_;
};

BrokerHandle *ActorEmbeddedModeTest::broker_ = nullptr;

// ============================================================================
// Producer embedded-mode tests
// ============================================================================

TEST_F(ActorEmbeddedModeTest, ProducerStartEmbeddedSetsRunning)
{
    Messenger m = make_messenger();

    ProducerOptions opts;
    opts.channel_name = unique_channel("test.embedded.p1");
    opts.timeout_ms   = 2000;

    auto maybe = Producer::create(m, opts);
    ASSERT_TRUE(maybe.has_value()) << "Producer::create() failed — broker not ready?";

    EXPECT_TRUE(maybe->is_valid()); // is_valid() before start_embedded

    // start_embedded() sets running=true WITHOUT launching internal threads.
    EXPECT_TRUE(maybe->start_embedded());
    EXPECT_TRUE(maybe->is_running());
    EXPECT_TRUE(maybe->is_valid()); // is_valid() unchanged after start_embedded

    maybe->stop();
    maybe->close();
}

TEST_F(ActorEmbeddedModeTest, ProducerStartEmbeddedIdempotent)
{
    Messenger m = make_messenger();

    ProducerOptions opts;
    opts.channel_name = unique_channel("test.embedded.p2");
    opts.timeout_ms   = 2000;

    auto maybe = Producer::create(m, opts);
    ASSERT_TRUE(maybe.has_value());

    EXPECT_TRUE(maybe->start_embedded());
    // Second call: already running → must return false (not restart).
    EXPECT_FALSE(maybe->start_embedded());
    EXPECT_TRUE(maybe->is_running()); // still running

    maybe->stop();
    maybe->close();
}

TEST_F(ActorEmbeddedModeTest, ProducerPeerSocketHandleNonNull)
{
    Messenger m = make_messenger();

    ProducerOptions opts;
    opts.channel_name = unique_channel("test.embedded.p3");
    opts.timeout_ms   = 2000;

    auto maybe = Producer::create(m, opts);
    ASSERT_TRUE(maybe.has_value());
    EXPECT_TRUE(maybe->start_embedded());

    // The ROUTER ctrl socket must be non-null after successful create().
    // This is the socket that the actor's zmq_thread_ adds to zmq_pollitem_t.
    EXPECT_NE(maybe->peer_ctrl_socket_handle(), nullptr);

    maybe->stop();
    maybe->close();
}

TEST_F(ActorEmbeddedModeTest, ProducerHandlePeerEventsNowaitNoOpWhenNotRunning)
{
    Messenger m = make_messenger();

    ProducerOptions opts;
    opts.channel_name = unique_channel("test.embedded.p4");
    opts.timeout_ms   = 2000;

    auto maybe = Producer::create(m, opts);
    ASSERT_TRUE(maybe.has_value());
    // Do NOT call start_embedded() — must be a no-op and not crash.
    EXPECT_NO_THROW(maybe->handle_peer_events_nowait());

    maybe->close();
}

// ============================================================================
// Consumer embedded-mode tests
// ============================================================================

TEST_F(ActorEmbeddedModeTest, ConsumerStartEmbeddedSetsRunning)
{
    Messenger mp = make_messenger();
    Messenger mc = make_messenger();

    // Producer must exist first so Consumer::connect() can discover it.
    ProducerOptions popts;
    popts.channel_name = unique_channel("test.embedded.c1");
    popts.timeout_ms   = 2000;
    auto prod = Producer::create(mp, popts);
    ASSERT_TRUE(prod.has_value());

    ConsumerOptions copts;
    copts.channel_name = unique_channel("test.embedded.c1");
    copts.timeout_ms   = 2000;
    auto cons = Consumer::connect(mc, copts);
    ASSERT_TRUE(cons.has_value()) << "Consumer::connect() failed";

    EXPECT_TRUE(cons->is_valid());

    // start_embedded() sets running=true WITHOUT launching internal threads.
    EXPECT_TRUE(cons->start_embedded());
    EXPECT_TRUE(cons->is_running());
    EXPECT_TRUE(cons->is_valid()); // is_valid() unchanged

    cons->stop();
    cons->close();
    prod->stop();
    prod->close();
}

TEST_F(ActorEmbeddedModeTest, ConsumerStartEmbeddedIdempotent)
{
    Messenger mp = make_messenger();
    Messenger mc = make_messenger();

    ProducerOptions popts;
    popts.channel_name = unique_channel("test.embedded.c2");
    popts.timeout_ms   = 2000;
    auto prod = Producer::create(mp, popts);
    ASSERT_TRUE(prod.has_value());

    ConsumerOptions copts;
    copts.channel_name = unique_channel("test.embedded.c2");
    copts.timeout_ms   = 2000;
    auto cons = Consumer::connect(mc, copts);
    ASSERT_TRUE(cons.has_value());

    EXPECT_TRUE(cons->start_embedded());
    // Second call: already running → false.
    EXPECT_FALSE(cons->start_embedded());
    EXPECT_TRUE(cons->is_running());

    cons->stop();
    cons->close();
    prod->stop();
    prod->close();
}

TEST_F(ActorEmbeddedModeTest, ConsumerCtrlSocketHandleNonNull)
{
    Messenger mp = make_messenger();
    Messenger mc = make_messenger();

    ProducerOptions popts;
    popts.channel_name = unique_channel("test.embedded.c3");
    popts.timeout_ms   = 2000;
    auto prod = Producer::create(mp, popts);
    ASSERT_TRUE(prod.has_value());

    ConsumerOptions copts;
    copts.channel_name = unique_channel("test.embedded.c3");
    copts.timeout_ms   = 2000;
    auto cons = Consumer::connect(mc, copts);
    ASSERT_TRUE(cons.has_value());
    EXPECT_TRUE(cons->start_embedded());

    // The DEALER ctrl socket must be non-null for all patterns.
    // The actor's zmq_thread_ uses this for zmq_pollitem_t.
    EXPECT_NE(cons->ctrl_zmq_socket_handle(), nullptr);

    cons->stop();
    cons->close();
    prod->stop();
    prod->close();
}

TEST_F(ActorEmbeddedModeTest, ConsumerDataSocketHandleNullForBidir)
{
    Messenger mp = make_messenger();
    Messenger mc = make_messenger();

    // Bidir pattern: data arrives via the ctrl (DEALER) socket — no separate data socket.
    ProducerOptions popts;
    popts.channel_name = unique_channel("test.embedded.c4");
    popts.pattern      = ChannelPattern::Bidir;
    popts.timeout_ms   = 2000;
    auto prod = Producer::create(mp, popts);
    ASSERT_TRUE(prod.has_value());

    ConsumerOptions copts;
    copts.channel_name = unique_channel("test.embedded.c4");
    copts.timeout_ms   = 2000;
    auto cons = Consumer::connect(mc, copts);
    ASSERT_TRUE(cons.has_value());
    EXPECT_TRUE(cons->start_embedded());

    // Bidir: data_zmq_socket_handle() MUST be nullptr (no separate SUB/PULL socket).
    EXPECT_EQ(cons->data_zmq_socket_handle(), nullptr);
    // ctrl_zmq_socket_handle() must still be valid (DEALER carries both ctrl + data).
    EXPECT_NE(cons->ctrl_zmq_socket_handle(), nullptr);

    cons->stop();
    cons->close();
    prod->stop();
    prod->close();
}

TEST_F(ActorEmbeddedModeTest, ConsumerHandleDataEventsNowaitNoOpWhenNotRunning)
{
    Messenger mp = make_messenger();
    Messenger mc = make_messenger();

    ProducerOptions popts;
    popts.channel_name = unique_channel("test.embedded.c5");
    popts.timeout_ms   = 2000;
    auto prod = Producer::create(mp, popts);
    ASSERT_TRUE(prod.has_value());

    ConsumerOptions copts;
    copts.channel_name = unique_channel("test.embedded.c5");
    copts.timeout_ms   = 2000;
    auto cons = Consumer::connect(mc, copts);
    ASSERT_TRUE(cons.has_value());
    // Do NOT call start_embedded() — both handle_*_events_nowait() must be no-ops.
    EXPECT_NO_THROW(cons->handle_data_events_nowait());
    EXPECT_NO_THROW(cons->handle_ctrl_events_nowait());

    cons->close();
    prod->stop();
    prod->close();
}

TEST_F(ActorEmbeddedModeTest, ConsumerDataSocketHandleNonNullForPubSub)
{
    Messenger mp = make_messenger();
    Messenger mc = make_messenger();

    // PubSub (default): separate SUB data socket + DEALER ctrl socket.
    ProducerOptions popts;
    popts.channel_name = unique_channel("test.embedded.c6");
    popts.pattern      = ChannelPattern::PubSub;
    popts.timeout_ms   = 2000;
    auto prod = Producer::create(mp, popts);
    ASSERT_TRUE(prod.has_value());

    ConsumerOptions copts;
    copts.channel_name = unique_channel("test.embedded.c6");
    copts.timeout_ms   = 2000;
    auto cons = Consumer::connect(mc, copts);
    ASSERT_TRUE(cons.has_value());
    EXPECT_TRUE(cons->start_embedded());

    // PubSub: data_zmq_socket_handle() must be non-null (SUB socket).
    EXPECT_NE(cons->data_zmq_socket_handle(), nullptr);
    EXPECT_NE(cons->ctrl_zmq_socket_handle(), nullptr);

    cons->stop();
    cons->close();
    prod->stop();
    prod->close();
}
