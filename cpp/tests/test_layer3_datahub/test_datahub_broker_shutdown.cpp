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
 *   1. Graceful shutdown: close → notify → client deregisters → channel removed
 *   2. Force shutdown:   close → notify → grace expires → FORCE_SHUTDOWN → removed
 *   3. Early cleanup:    close → all consumers deregister → channel removed early
 *   4. Mixed:            close → some deregister, some don't → force on stragglers
 *
 * All tests use the in-process LocalBrokerHandle pattern with Messenger instances.
 * Channel names are PID-suffixed for CTest parallelism.
 */
#include "plh_datahub.hpp"
#include "plh_service.hpp"
#include "utils/broker_service.hpp"
#include "utils/messenger.hpp"

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

// ── LocalBrokerHandle — same pattern as other L3 tests ───────────────────────

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

    bool is_set()
    {
        std::lock_guard<std::mutex> lock(mtx);
        return fired;
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
            pylabhub::hub::GetLifecycleModule()));
    }
    static void TearDownTestSuite() { s_lifecycle_.reset(); }

protected:
    /// Start broker with a specific grace period.
    void start_broker(std::chrono::seconds grace)
    {
        BrokerService::Config cfg;
        cfg.endpoint               = "tcp://127.0.0.1:0";
        cfg.schema_search_dirs     = {};
        cfg.channel_shutdown_grace = grace;
        broker_.emplace(start_local_broker(std::move(cfg)));
    }

    const std::string &ep() const { return broker_->endpoint; }
    const std::string &pk() const { return broker_->pubkey; }
    BrokerService     &svc() { return *broker_->service; }

    std::optional<LocalBrokerHandle> broker_;

private:
    static std::unique_ptr<LifecycleGuard> s_lifecycle_;
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::unique_ptr<LifecycleGuard> BrokerShutdownTest::s_lifecycle_;

// ============================================================================
// Test 1: Graceful shutdown — client deregisters after CHANNEL_CLOSING_NOTIFY
// ============================================================================

TEST_F(BrokerShutdownTest, GracefulShutdown_ProducerDeregisters)
{
    start_broker(std::chrono::seconds(10)); // long grace — we deregister before it expires
    const std::string channel = pid_chan("shutdown.graceful.prod");

    // Create producer
    Messenger prod;
    ASSERT_TRUE(prod.connect(ep(), pk()));
    auto prod_handle = prod.create_channel(channel, {.timeout_ms = 3000});
    ASSERT_TRUE(prod_handle.has_value());

    // Register closing callback
    auto closing_flag = std::make_shared<SignalFlag>();
    prod.on_channel_closing(channel, [closing_flag]() { closing_flag->set(); });

    // Request close
    svc().request_close_channel(channel);

    // Wait for CHANNEL_CLOSING_NOTIFY
    ASSERT_TRUE(closing_flag->wait(3000)) << "CHANNEL_CLOSING_NOTIFY not received";

    // Channel should be in Closing status
    ChannelSnapshot snap = svc().query_channel_snapshot();
    bool found_closing = false;
    for (const auto &ch : snap.channels)
    {
        if (ch.name == channel)
        {
            found_closing = (ch.status == "Closing");
            break;
        }
    }
    EXPECT_TRUE(found_closing) << "Channel should be in Closing status";

    // Producer deregisters (graceful)
    prod.unregister_channel(channel);

    // Give broker a poll cycle to process the deregistration
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // Channel should be gone
    snap = svc().query_channel_snapshot();
    bool still_present = false;
    for (const auto &ch : snap.channels)
    {
        if (ch.name == channel)
        {
            still_present = true;
            break;
        }
    }
    EXPECT_FALSE(still_present) << "Channel should be removed after producer deregisters";
}

// ============================================================================
// Test 2: Force shutdown — client does NOT deregister, grace expires
// ============================================================================

TEST_F(BrokerShutdownTest, ForceShutdown_GraceExpires)
{
    // Short grace period — 1 second
    start_broker(std::chrono::seconds(1));
    const std::string channel = pid_chan("shutdown.force.expire");

    // Create producer
    Messenger prod;
    ASSERT_TRUE(prod.connect(ep(), pk()));
    auto prod_handle = prod.create_channel(channel, {.timeout_ms = 3000});
    ASSERT_TRUE(prod_handle.has_value());

    // Register callbacks
    auto closing_flag = std::make_shared<SignalFlag>();
    auto force_flag   = std::make_shared<SignalFlag>();
    prod.on_channel_closing(channel, [closing_flag]() { closing_flag->set(); });
    prod.on_force_shutdown(channel, [force_flag]() { force_flag->set(); });

    // Request close
    svc().request_close_channel(channel);

    // Wait for CHANNEL_CLOSING_NOTIFY
    ASSERT_TRUE(closing_flag->wait(3000)) << "CHANNEL_CLOSING_NOTIFY not received";

    // Do NOT deregister — let grace period expire

    // Wait for FORCE_SHUTDOWN (should arrive after ~1 second grace)
    ASSERT_TRUE(force_flag->wait(5000)) << "FORCE_SHUTDOWN not received after grace period";

    // Give broker a poll cycle to deregister
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // Channel should be gone
    ChannelSnapshot snap = svc().query_channel_snapshot();
    bool still_present = false;
    for (const auto &ch : snap.channels)
    {
        if (ch.name == channel)
        {
            still_present = true;
            break;
        }
    }
    EXPECT_FALSE(still_present) << "Channel should be removed after FORCE_SHUTDOWN";
}

// ============================================================================
// Test 3: Early cleanup — all consumers deregister before grace expires
// ============================================================================

TEST_F(BrokerShutdownTest, EarlyCleanup_AllConsumersDeregister)
{
    start_broker(std::chrono::seconds(10)); // long grace — channel removed before it expires
    const std::string channel = pid_chan("shutdown.early.cleanup");

    // Create producer
    Messenger prod;
    ASSERT_TRUE(prod.connect(ep(), pk()));
    auto prod_handle = prod.create_channel(channel, {.timeout_ms = 3000});
    ASSERT_TRUE(prod_handle.has_value());

    // Connect consumer
    Messenger cons;
    ASSERT_TRUE(cons.connect(ep(), pk()));
    auto cons_handle = cons.connect_channel(channel, /*timeout_ms=*/3000);
    ASSERT_TRUE(cons_handle.has_value());

    // Register closing callbacks
    auto prod_closing = std::make_shared<SignalFlag>();
    auto cons_closing = std::make_shared<SignalFlag>();
    prod.on_channel_closing(channel, [prod_closing]() { prod_closing->set(); });
    cons.on_channel_closing(channel, [cons_closing]() { cons_closing->set(); });

    // Request close
    svc().request_close_channel(channel);

    // Wait for both to receive CHANNEL_CLOSING_NOTIFY
    ASSERT_TRUE(prod_closing->wait(3000)) << "Producer did not receive CHANNEL_CLOSING_NOTIFY";
    ASSERT_TRUE(cons_closing->wait(3000)) << "Consumer did not receive CHANNEL_CLOSING_NOTIFY";

    // Consumer deregisters first
    cons.deregister_consumer(channel);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Producer deregisters
    prod.unregister_channel(channel);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // Channel should be removed early (no need to wait for grace period)
    ChannelSnapshot snap = svc().query_channel_snapshot();
    bool still_present = false;
    for (const auto &ch : snap.channels)
    {
        if (ch.name == channel)
        {
            still_present = true;
            break;
        }
    }
    EXPECT_FALSE(still_present) << "Channel should be removed after all members deregister";
}

// ============================================================================
// Test 4: Force shutdown with consumer — consumer doesn't deregister
// ============================================================================

TEST_F(BrokerShutdownTest, ForceShutdown_ConsumerDoesNotDeregister)
{
    start_broker(std::chrono::seconds(1)); // short grace
    const std::string channel = pid_chan("shutdown.force.consumer");

    // Create producer
    Messenger prod;
    ASSERT_TRUE(prod.connect(ep(), pk()));
    auto prod_handle = prod.create_channel(channel, {.timeout_ms = 3000});
    ASSERT_TRUE(prod_handle.has_value());

    // Connect consumer
    Messenger cons;
    ASSERT_TRUE(cons.connect(ep(), pk()));
    auto cons_handle = cons.connect_channel(channel, /*timeout_ms=*/3000);
    ASSERT_TRUE(cons_handle.has_value());

    // Register force shutdown callbacks on both
    auto prod_force = std::make_shared<SignalFlag>();
    auto cons_force = std::make_shared<SignalFlag>();
    prod.on_force_shutdown(channel, [prod_force]() { prod_force->set(); });
    cons.on_force_shutdown(channel, [cons_force]() { cons_force->set(); });

    // Request close — do NOT deregister either member
    svc().request_close_channel(channel);

    // Both should receive FORCE_SHUTDOWN after grace period
    ASSERT_TRUE(prod_force->wait(5000)) << "Producer did not receive FORCE_SHUTDOWN";
    ASSERT_TRUE(cons_force->wait(5000)) << "Consumer did not receive FORCE_SHUTDOWN";

    // Channel should be removed
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ChannelSnapshot snap = svc().query_channel_snapshot();
    bool still_present = false;
    for (const auto &ch : snap.channels)
    {
        if (ch.name == channel)
        {
            still_present = true;
            break;
        }
    }
    EXPECT_FALSE(still_present) << "Channel should be removed after FORCE_SHUTDOWN";
}

// ============================================================================
// Test 5: Closing status visible in snapshot
// ============================================================================

TEST_F(BrokerShutdownTest, ClosingStatus_InSnapshot)
{
    start_broker(std::chrono::seconds(10)); // long grace so we can observe Closing status
    const std::string channel = pid_chan("shutdown.status.closing");

    // Create producer
    Messenger prod;
    ASSERT_TRUE(prod.connect(ep(), pk()));
    auto prod_handle = prod.create_channel(channel, {.timeout_ms = 3000});
    ASSERT_TRUE(prod_handle.has_value());

    // Register closing callback
    auto closing_flag = std::make_shared<SignalFlag>();
    prod.on_channel_closing(channel, [closing_flag]() { closing_flag->set(); });

    // Request close
    svc().request_close_channel(channel);

    // Wait for notification delivery
    ASSERT_TRUE(closing_flag->wait(3000));

    // Verify Closing status in snapshot
    ChannelSnapshot snap = svc().query_channel_snapshot();
    const ChannelSnapshotEntry *entry = nullptr;
    for (const auto &ch : snap.channels)
    {
        if (ch.name == channel)
        {
            entry = &ch;
            break;
        }
    }
    ASSERT_NE(entry, nullptr) << "Channel not in snapshot";
    EXPECT_EQ(entry->status, "Closing");

    // Verify Closing status in list_channels_json_str
    auto j = json::parse(svc().list_channels_json_str());
    for (const auto &e : j)
    {
        if (e.value("name", "") == channel)
        {
            EXPECT_EQ(e.value("status", ""), "Closing");
        }
    }

    // Cleanup: deregister so channel is removed
    prod.unregister_channel(channel);
}

// ============================================================================
// Test 6: Zero grace period — immediate force shutdown (backward compat)
// ============================================================================

TEST_F(BrokerShutdownTest, ZeroGrace_ImmediateForceShutdown)
{
    start_broker(std::chrono::seconds(0)); // immediate
    const std::string channel = pid_chan("shutdown.zerograce");

    // Create producer
    Messenger prod;
    ASSERT_TRUE(prod.connect(ep(), pk()));
    auto prod_handle = prod.create_channel(channel, {.timeout_ms = 3000});
    ASSERT_TRUE(prod_handle.has_value());

    // Request close
    svc().request_close_channel(channel);

    // With grace=0, deadline is immediate — force shutdown fires on next poll cycle
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Channel should be removed
    ChannelSnapshot snap = svc().query_channel_snapshot();
    bool still_present = false;
    for (const auto &ch : snap.channels)
    {
        if (ch.name == channel)
        {
            still_present = true;
            break;
        }
    }
    EXPECT_FALSE(still_present) << "Channel should be immediately removed with grace=0";
}
