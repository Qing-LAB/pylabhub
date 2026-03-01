/**
 * @file test_datahub_channel_access_policy.cpp
 * @brief Channel access policy tests — Phase 3 (HEP-CORE-0009).
 *
 * Suite 1: Pure unit tests for enum conversion helpers (no ZMQ, no broker).
 * Suite 2: In-process broker enforcement tests covering Open / Required /
 *           Verified policies and per-channel glob overrides.
 *
 * All broker tests start a real BrokerService in a background thread and
 * exercise policy enforcement via Messenger::create_channel().
 *
 * CTest safety: all channel names are suffixed with the process PID so
 * parallel runs (`-j`) never share channels.
 */
#include "plh_datahub.hpp"
#include "plh_service.hpp"
#include "utils/broker_service.hpp"
#include "utils/channel_access_policy.hpp"
#include "utils/messenger.hpp"

#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <gtest/gtest.h>

using namespace pylabhub::utils;
using namespace pylabhub::hub;
using namespace pylabhub::broker;

namespace
{

// ── Local BrokerHandle (same pattern as datahub_broker_workers.cpp) ──────────

struct LocalBrokerHandle
{
    std::unique_ptr<BrokerService> service;
    std::thread                    thread;
    std::string                    endpoint;
    std::string                    pubkey;

    LocalBrokerHandle() = default;
    LocalBrokerHandle(LocalBrokerHandle&&) noexcept = default;
    LocalBrokerHandle& operator=(LocalBrokerHandle&&) noexcept = default;
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

    cfg.on_ready = [promise](const std::string& ep, const std::string& pk)
    { promise->set_value({ep, pk}); };

    auto svc     = std::make_unique<BrokerService>(std::move(cfg));
    auto raw_ptr = svc.get();
    std::thread t([raw_ptr] { raw_ptr->run(); });

    auto info = future.get(); // blocks until broker is bound

    LocalBrokerHandle h;
    h.service  = std::move(svc);
    h.thread   = std::move(t);
    h.endpoint = info.first;
    h.pubkey   = info.second;
    return h;
}

// ── Helper: try to register a channel; returns true on success ────────────────

bool try_register(const std::string& endpoint,
                  const std::string& pubkey,
                  const std::string& channel,
                  const std::string& actor_name = {},
                  const std::string& actor_uid  = {})
{
    Messenger m;
    if (!m.connect(endpoint, pubkey))
        return false;
    auto handle = m.create_channel(channel,
                                   ChannelPattern::PubSub,
                                   /*has_shared_memory=*/false,
                                   /*schema_hash=*/{},
                                   /*schema_version=*/0,
                                   /*timeout_ms=*/3000,
                                   actor_name,
                                   actor_uid);
    return handle.has_value();
}

// Suffix channel name with PID for CTest -j safety.
std::string pid_chan(const std::string& base)
{
    return base + "." + std::to_string(getpid());
}

} // anonymous namespace

// ============================================================================
// Suite 1 — Enum conversion (no broker, no ZMQ)
// ============================================================================

class ConnectionPolicyEnumTest : public ::testing::Test
{
};

TEST_F(ConnectionPolicyEnumTest, ToStrAllValues)
{
    EXPECT_STREQ(connection_policy_to_str(ConnectionPolicy::Open),     "open");
    EXPECT_STREQ(connection_policy_to_str(ConnectionPolicy::Tracked),  "tracked");
    EXPECT_STREQ(connection_policy_to_str(ConnectionPolicy::Required), "required");
    EXPECT_STREQ(connection_policy_to_str(ConnectionPolicy::Verified), "verified");
}

TEST_F(ConnectionPolicyEnumTest, FromStrKnownValues)
{
    EXPECT_EQ(connection_policy_from_str("open"),     ConnectionPolicy::Open);
    EXPECT_EQ(connection_policy_from_str("tracked"),  ConnectionPolicy::Tracked);
    EXPECT_EQ(connection_policy_from_str("required"), ConnectionPolicy::Required);
    EXPECT_EQ(connection_policy_from_str("verified"), ConnectionPolicy::Verified);
}

TEST_F(ConnectionPolicyEnumTest, FromStrUnknownFallsToOpen)
{
    // A typo must not silently upgrade security — it degrades to Open (safest default
    // for hub operator: they notice nothing connects, rather than unexpected access).
    EXPECT_EQ(connection_policy_from_str("verfied"),  ConnectionPolicy::Open);
    EXPECT_EQ(connection_policy_from_str("REQUIRED"), ConnectionPolicy::Open); // case-sensitive
    EXPECT_EQ(connection_policy_from_str(""),         ConnectionPolicy::Open);
    EXPECT_EQ(connection_policy_from_str("open "),    ConnectionPolicy::Open); // trailing space
}

TEST_F(ConnectionPolicyEnumTest, ToStrFromStrRoundTrip)
{
    for (auto pol : {ConnectionPolicy::Open, ConnectionPolicy::Tracked,
                     ConnectionPolicy::Required, ConnectionPolicy::Verified})
    {
        EXPECT_EQ(connection_policy_from_str(connection_policy_to_str(pol)), pol);
    }
}

// ============================================================================
// Suite 2 — Broker policy enforcement
// ============================================================================

class ConnectionPolicyBrokerTest : public ::testing::Test
{
  public:
    static void SetUpTestSuite()
    {
        s_lifecycle_ = std::make_unique<pylabhub::utils::LifecycleGuard>(
            pylabhub::utils::MakeModDefList(
                pylabhub::utils::Logger::GetLifecycleModule(),
                pylabhub::crypto::GetLifecycleModule(),
                pylabhub::hub::GetLifecycleModule()));
    }

    static void TearDownTestSuite() { s_lifecycle_.reset(); }

  protected:
    std::optional<LocalBrokerHandle> broker_;

    void StartBroker(BrokerService::Config cfg)
    {
        // Use ephemeral port so parallel ctest -j runs don't collide.
        if (cfg.endpoint == "tcp://0.0.0.0:5570")
            cfg.endpoint = "tcp://127.0.0.1:0";
        broker_.emplace(start_local_broker(std::move(cfg)));
    }

    const std::string& ep() const { return broker_->endpoint; }
    const std::string& pk() const { return broker_->pubkey;   }

  private:
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    static std::unique_ptr<pylabhub::utils::LifecycleGuard> s_lifecycle_;
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::unique_ptr<pylabhub::utils::LifecycleGuard> ConnectionPolicyBrokerTest::s_lifecycle_;

TEST_F(ConnectionPolicyBrokerTest, OpenPolicyAcceptsAnonymous)
{
    BrokerService::Config cfg;
    cfg.connection_policy = ConnectionPolicy::Open;
    StartBroker(std::move(cfg));

    EXPECT_TRUE(try_register(ep(), pk(), pid_chan("lab.open.anon")));
}

TEST_F(ConnectionPolicyBrokerTest, OpenPolicyAcceptsWithIdentity)
{
    BrokerService::Config cfg;
    cfg.connection_policy = ConnectionPolicy::Open;
    StartBroker(std::move(cfg));

    EXPECT_TRUE(try_register(ep(), pk(), pid_chan("lab.open.id"),
                             "lab.sensor1", "ACTOR-SENSOR-AABBCCDD"));
}

TEST_F(ConnectionPolicyBrokerTest, RequiredPolicyRejectsAnonymous)
{
    BrokerService::Config cfg;
    cfg.connection_policy = ConnectionPolicy::Required;
    StartBroker(std::move(cfg));

    EXPECT_FALSE(try_register(ep(), pk(), pid_chan("lab.req.anon")));
}

TEST_F(ConnectionPolicyBrokerTest, RequiredPolicyAcceptsWithIdentity)
{
    BrokerService::Config cfg;
    cfg.connection_policy = ConnectionPolicy::Required;
    StartBroker(std::move(cfg));

    EXPECT_TRUE(try_register(ep(), pk(), pid_chan("lab.req.id"),
                             "lab.sensor1", "ACTOR-SENSOR-AABBCCDD"));
}

TEST_F(ConnectionPolicyBrokerTest, VerifiedPolicyRejectsUnknownActor)
{
    BrokerService::Config cfg;
    cfg.connection_policy = ConnectionPolicy::Verified;
    cfg.known_actors.push_back({"lab.sensor1", "ACTOR-SENSOR-AABBCCDD", "producer"});
    StartBroker(std::move(cfg));

    // Different UID — not in known_actors.
    EXPECT_FALSE(try_register(ep(), pk(), pid_chan("lab.ver.unknown"),
                              "lab.intruder", "ACTOR-INTRUDE-11223344"));
}

TEST_F(ConnectionPolicyBrokerTest, VerifiedPolicyAcceptsKnownActor)
{
    BrokerService::Config cfg;
    cfg.connection_policy = ConnectionPolicy::Verified;
    cfg.known_actors.push_back({"lab.sensor1", "ACTOR-SENSOR-AABBCCDD", "producer"});
    StartBroker(std::move(cfg));

    EXPECT_TRUE(try_register(ep(), pk(), pid_chan("lab.ver.known"),
                             "lab.sensor1", "ACTOR-SENSOR-AABBCCDD"));
}

TEST_F(ConnectionPolicyBrokerTest, PerChannelGlobOverrideRestrictsChannel)
{
    // Hub-wide = Tracked (permissive).
    // "lab.secure.*" channels override to Verified with an empty known_actors list
    // → any actor rejected for the secured glob, while the regular channel succeeds.
    BrokerService::Config cfg;
    cfg.connection_policy = ConnectionPolicy::Tracked;
    cfg.channel_policies.push_back({"lab.secure.*", ConnectionPolicy::Verified});
    // known_actors intentionally empty — Verified rejects everyone.
    StartBroker(std::move(cfg));

    // Regular channel (Tracked) accepts without identity.
    EXPECT_TRUE(try_register(ep(), pk(), pid_chan("lab.regular")));

    // Secured channel (Verified, empty allowlist) rejects even with identity.
    EXPECT_FALSE(try_register(ep(), pk(), "lab.secure." + std::to_string(getpid()),
                              "lab.sensor1", "ACTOR-SENSOR-AABBCCDD"));
}
