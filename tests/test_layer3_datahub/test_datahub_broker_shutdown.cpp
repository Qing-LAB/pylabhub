/**
 * @file test_datahub_broker_shutdown.cpp
 * @brief Tests for the two-tier graceful shutdown protocol.
 *
 * Suite: BrokerShutdownTest
 *
 * Tests the broker's channel shutdown sequence (HEP-CORE-0007 §12.7 Sequence B):
 *   Tier 1 — CHANNEL_CLOSING_NOTIFY: queued cooperative shutdown
 *   Tier 2 — FORCE_SHUTDOWN: bypasses message queue after grace period
 *
 * Test scenarios:
 *   1. Graceful shutdown: close → notify → producer deregisters → channel removed
 *   2. Force shutdown:   close → notify → grace expires → FORCE_SHUTDOWN → removed
 *   3. Early cleanup:    close → all members deregister → channel removed early
 *   4. Force with consumer: close → nobody deregisters → force on all members
 *   5. Closing status visible in admin snapshot
 *   6. Zero grace: immediate force shutdown
 *
 * All tests use in-process LocalBrokerHandle + BrcHandle pattern.
 * Channel names are PID-suffixed for CTest parallelism.
 */
#include "plh_datahub.hpp"
#include "plh_service.hpp"
#include "utils/broker_request_comm.hpp"
#include "utils/broker_service.hpp"
#include "utils/hub_state.hpp"
#include "test_sync_utils.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
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

// ── LocalBrokerHandle ───────────────────────────────────────────────────────

struct LocalBrokerHandle
{
    std::unique_ptr<pylabhub::hub::HubState> hub_state;
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

    auto state     = std::make_unique<pylabhub::hub::HubState>();
    auto svc     = std::make_unique<BrokerService>(std::move(cfg), *state);
    auto raw_ptr = svc.get();
    std::thread t([raw_ptr] { raw_ptr->run(); });

    auto info = future.get();

    LocalBrokerHandle h;
    h.hub_state  = std::move(state);
    h.service  = std::move(svc);
    h.thread   = std::move(t);
    h.endpoint = info.first;
    h.pubkey   = info.second;
    return h;
}

std::string pid_chan(const std::string &base)
{
    return base + ".pid" + std::to_string(getpid());
}

// ── BrcHandle — BrokerRequestComm with poll thread ──────────────────────────

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

// ── Helpers ─────────────────────────────────────────────────────────────────

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

json make_cons_opts(const std::string &channel, const std::string &consumer_uid)
{
    json opts;
    opts["channel_name"]  = channel;
    opts["consumer_uid"]  = consumer_uid;
    opts["consumer_name"] = "test_consumer";
    opts["consumer_pid"]  = ::getpid();
    return opts;
}

/// Thread-safe flag with condition variable for waiting on callbacks.
struct SignalFlag
{
    std::mutex              mtx;
    std::condition_variable cv;
    bool                    fired{false};

    void set()
    {
        {
            std::lock_guard<std::mutex> lock(mtx);
            fired = true;
        }
        cv.notify_all();
    }

    bool wait(int timeout_ms = 5000)
    {
        std::unique_lock<std::mutex> lock(mtx);
        return cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                           [&] { return fired; });
    }
};

} // anonymous namespace

// ============================================================================
// BrokerShutdownTest fixture — configurable grace period
// ============================================================================

class BrokerShutdownTest : public ::testing::Test
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
    void start_broker(std::chrono::seconds grace)
    {
        BrokerService::Config cfg;
        cfg.endpoint               = "tcp://127.0.0.1:0";
        cfg.schema_search_dirs     = {};
        cfg.grace_override = std::chrono::duration_cast<std::chrono::milliseconds>(grace);
        broker_.emplace(start_local_broker(std::move(cfg)));
    }

    const std::string &ep() const { return broker_->endpoint; }
    const std::string &pk() const { return broker_->pubkey; }
    BrokerService     &svc() { return *broker_->service; }

    std::optional<LocalBrokerHandle> broker_;

private:
    static std::unique_ptr<LifecycleGuard> s_lifecycle_;
};

std::unique_ptr<LifecycleGuard> BrokerShutdownTest::s_lifecycle_;

// ============================================================================
// Test 1: Graceful shutdown — producer deregisters after CHANNEL_CLOSING_NOTIFY
// ============================================================================

TEST_F(BrokerShutdownTest, GracefulShutdown_ProducerDeregisters)
{
    start_broker(std::chrono::seconds(10));
    const std::string channel = pid_chan("shutdown.graceful.prod");
    const std::string uid     = "prod." + channel;

    BrcHandle bh;
    auto closing_flag = std::make_shared<SignalFlag>();
    bh.brc.on_notification([closing_flag](const std::string &msg_type, const json &)
    {
        if (msg_type == "CHANNEL_CLOSING_NOTIFY")
            closing_flag->set();
    });
    bh.start(ep(), pk(), uid);

    auto reg = bh.brc.register_channel(make_reg_opts(channel, uid), 3000);
    ASSERT_TRUE(reg.has_value()) << "register_channel failed";

    // Request close
    svc().request_close_channel(channel);

    // Wait for CHANNEL_CLOSING_NOTIFY
    ASSERT_TRUE(closing_flag->wait(3000)) << "CHANNEL_CLOSING_NOTIFY not received";

    // Verify Closing status
    ChannelSnapshot snap = svc().query_channel_snapshot();
    bool found_closing = false;
    for (const auto &ch : snap.channels)
        if (ch.name == channel)
            found_closing = (ch.status == "Closing");
    EXPECT_TRUE(found_closing);

    // Producer deregisters (graceful)
    EXPECT_TRUE(bh.brc.deregister_channel(channel));

    // Poll until broker has removed the channel from its registry.  The
    // deregister_channel() call returns on ACK; the actual registry
    // cleanup is async and can take a poll cycle on a loaded CI box.
    using pylabhub::tests::helper::poll_until;
    auto channel_gone = [&] {
        auto s = svc().query_channel_snapshot();
        for (const auto &ch : s.channels)
            if (ch.name == channel) return false;
        return true;
    };
    EXPECT_TRUE(poll_until(channel_gone, std::chrono::seconds(3)))
        << "Channel not removed from broker registry within 3s after deregister";

    bh.stop();
}

// ============================================================================
// Test 2: Force shutdown — producer does NOT deregister, grace expires
// ============================================================================

TEST_F(BrokerShutdownTest, ForceShutdown_GraceExpires)
{
    start_broker(std::chrono::seconds(1));
    const std::string channel = pid_chan("shutdown.force.expire");
    const std::string uid     = "prod." + channel;

    BrcHandle bh;
    auto closing_flag = std::make_shared<SignalFlag>();
    auto force_flag   = std::make_shared<SignalFlag>();
    bh.brc.on_notification([closing_flag, force_flag](const std::string &msg_type, const json &)
    {
        if (msg_type == "CHANNEL_CLOSING_NOTIFY")
            closing_flag->set();
        else if (msg_type == "FORCE_SHUTDOWN")
            force_flag->set();
    });
    bh.start(ep(), pk(), uid);

    auto reg = bh.brc.register_channel(make_reg_opts(channel, uid), 3000);
    ASSERT_TRUE(reg.has_value());

    svc().request_close_channel(channel);

    ASSERT_TRUE(closing_flag->wait(3000)) << "CHANNEL_CLOSING_NOTIFY not received";

    // Do NOT deregister — let grace expire
    ASSERT_TRUE(force_flag->wait(5000)) << "FORCE_SHUTDOWN not received after grace";

    // FORCE_SHUTDOWN was sent; poll until broker completes registry removal.
    using pylabhub::tests::helper::poll_until;
    auto channel_gone = [&] {
        auto s = svc().query_channel_snapshot();
        for (const auto &ch : s.channels)
            if (ch.name == channel) return false;
        return true;
    };
    EXPECT_TRUE(poll_until(channel_gone, std::chrono::seconds(3)))
        << "Channel not removed after FORCE_SHUTDOWN within 3s";

    bh.stop();
}

// ============================================================================
// Test 3: Early cleanup — all members deregister before grace expires
// ============================================================================

TEST_F(BrokerShutdownTest, EarlyCleanup_AllMembersDeregister)
{
    start_broker(std::chrono::seconds(10));
    const std::string channel  = pid_chan("shutdown.early.cleanup");
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;

    // Producer
    BrcHandle prod_bh;
    auto prod_closing = std::make_shared<SignalFlag>();
    prod_bh.brc.on_notification([prod_closing](const std::string &msg_type, const json &)
    {
        if (msg_type == "CHANNEL_CLOSING_NOTIFY")
            prod_closing->set();
    });
    prod_bh.start(ep(), pk(), prod_uid);
    auto reg = prod_bh.brc.register_channel(make_reg_opts(channel, prod_uid), 3000);
    ASSERT_TRUE(reg.has_value());

    // CONSUMER_REG_REQ does not gate on Ready; no heartbeat/sleep needed here
    // (HEP-CORE-0023 §2.2: consumer uses discover_channel if it needs connection info).

    // Consumer
    BrcHandle cons_bh;
    auto cons_closing = std::make_shared<SignalFlag>();
    cons_bh.brc.on_notification([cons_closing](const std::string &msg_type, const json &)
    {
        if (msg_type == "CHANNEL_CLOSING_NOTIFY")
            cons_closing->set();
    });
    cons_bh.start(ep(), pk(), cons_uid);
    auto cons_reg = cons_bh.brc.register_consumer(make_cons_opts(channel, cons_uid), 3000);
    ASSERT_TRUE(cons_reg.has_value());

    svc().request_close_channel(channel);

    ASSERT_TRUE(prod_closing->wait(3000)) << "Producer did not receive CHANNEL_CLOSING_NOTIFY";
    ASSERT_TRUE(cons_closing->wait(3000)) << "Consumer did not receive CHANNEL_CLOSING_NOTIFY";

    // Consumer deregisters.  No inter-dereg sleep: the test only
    // asserts the end-state (channel removed once both dereg) — there
    // is no intermediate state to observe between the two calls.
    cons_bh.brc.deregister_consumer(channel);

    // Producer deregisters.  Poll until the broker has removed the
    // channel (async registry cleanup after both members gone).
    prod_bh.brc.deregister_channel(channel);

    using pylabhub::tests::helper::poll_until;
    auto channel_gone = [&] {
        auto s = svc().query_channel_snapshot();
        for (const auto &ch : s.channels)
            if (ch.name == channel) return false;
        return true;
    };
    EXPECT_TRUE(poll_until(channel_gone, std::chrono::seconds(3)))
        << "Channel not removed within 3s after all members deregistered";

    cons_bh.stop();
    prod_bh.stop();
}

// ============================================================================
// Test 4: Force shutdown with consumer — nobody deregisters
// ============================================================================

TEST_F(BrokerShutdownTest, ForceShutdown_ConsumerDoesNotDeregister)
{
    start_broker(std::chrono::seconds(1));
    const std::string channel  = pid_chan("shutdown.force.consumer");
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;

    BrcHandle prod_bh;
    auto prod_force = std::make_shared<SignalFlag>();
    prod_bh.brc.on_notification([prod_force](const std::string &msg_type, const json &)
    {
        if (msg_type == "FORCE_SHUTDOWN")
            prod_force->set();
    });
    prod_bh.start(ep(), pk(), prod_uid);
    auto reg = prod_bh.brc.register_channel(make_reg_opts(channel, prod_uid), 3000);
    ASSERT_TRUE(reg.has_value());

    // CONSUMER_REG_REQ does not gate on Ready (HEP-CORE-0023 §2.2).
    BrcHandle cons_bh;
    auto cons_force = std::make_shared<SignalFlag>();
    cons_bh.brc.on_notification([cons_force](const std::string &msg_type, const json &)
    {
        if (msg_type == "FORCE_SHUTDOWN")
            cons_force->set();
    });
    cons_bh.start(ep(), pk(), cons_uid);
    auto cons_reg = cons_bh.brc.register_consumer(make_cons_opts(channel, cons_uid), 3000);
    ASSERT_TRUE(cons_reg.has_value());

    svc().request_close_channel(channel);

    ASSERT_TRUE(prod_force->wait(5000)) << "Producer did not receive FORCE_SHUTDOWN";
    ASSERT_TRUE(cons_force->wait(5000)) << "Consumer did not receive FORCE_SHUTDOWN";

    using pylabhub::tests::helper::poll_until;
    auto channel_gone = [&] {
        auto s = svc().query_channel_snapshot();
        for (const auto &ch : s.channels)
            if (ch.name == channel) return false;
        return true;
    };
    EXPECT_TRUE(poll_until(channel_gone, std::chrono::seconds(3)))
        << "Channel not removed after FORCE_SHUTDOWN within 3s";

    cons_bh.stop();
    prod_bh.stop();
}

// ============================================================================
// Test 5: Closing status visible in snapshot
// ============================================================================

TEST_F(BrokerShutdownTest, ClosingStatus_InSnapshot)
{
    start_broker(std::chrono::seconds(10));
    const std::string channel = pid_chan("shutdown.status.closing");
    const std::string uid     = "prod." + channel;

    BrcHandle bh;
    auto closing_flag = std::make_shared<SignalFlag>();
    bh.brc.on_notification([closing_flag](const std::string &msg_type, const json &)
    {
        if (msg_type == "CHANNEL_CLOSING_NOTIFY")
            closing_flag->set();
    });
    bh.start(ep(), pk(), uid);

    auto reg = bh.brc.register_channel(make_reg_opts(channel, uid), 3000);
    ASSERT_TRUE(reg.has_value());

    svc().request_close_channel(channel);
    ASSERT_TRUE(closing_flag->wait(3000));

    ChannelSnapshot snap = svc().query_channel_snapshot();
    const ChannelSnapshotEntry *entry = nullptr;
    for (const auto &ch : snap.channels)
        if (ch.name == channel)
            entry = &ch;
    ASSERT_NE(entry, nullptr) << "Channel not in snapshot";
    EXPECT_EQ(entry->status, "Closing");

    // Cleanup
    bh.brc.deregister_channel(channel);
    bh.stop();
}

// ============================================================================
// Test 6: Zero grace — immediate force shutdown
// ============================================================================

TEST_F(BrokerShutdownTest, ZeroGrace_ImmediateForceShutdown)
{
    start_broker(std::chrono::seconds(0));
    const std::string channel = pid_chan("shutdown.zerograce");
    const std::string uid     = "prod." + channel;

    BrcHandle bh;
    bh.start(ep(), pk(), uid);
    auto reg = bh.brc.register_channel(make_reg_opts(channel, uid), 3000);
    ASSERT_TRUE(reg.has_value());

    svc().request_close_channel(channel);

    // grace=0 → force fires on next poll cycle.  No notification callback
    // is registered in this test, so we poll for the observable end-state
    // (channel gone from snapshot).  Timeout well above the broker's
    // worst-case poll cycle for headroom.
    using pylabhub::tests::helper::poll_until;
    auto channel_gone = [&] {
        auto s = svc().query_channel_snapshot();
        for (const auto &ch : s.channels)
            if (ch.name == channel) return false;
        return true;
    };
    EXPECT_TRUE(poll_until(channel_gone, std::chrono::seconds(3)))
        << "Channel should be removed on first broker poll cycle when grace=0";

    bh.stop();
}
