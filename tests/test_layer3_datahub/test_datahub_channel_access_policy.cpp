/**
 * @file test_datahub_channel_access_policy.cpp
 * @brief Connection access policy tests — Phase 3 (HEP-CORE-0009).
 *
 * Suite 1: Pure unit tests for enum conversion helpers (no ZMQ, no broker).
 * Suite 2: In-process broker enforcement tests covering Open / Required /
 *          Verified policies and per-channel glob overrides.
 *
 * All broker tests start a real BrokerService in a background thread and
 * exercise policy enforcement via BrokerRequestComm::register_channel().
 *
 * CTest safety: all channel names are suffixed with the process PID.
 */
#include "plh_datahub.hpp"
#include "plh_service.hpp"
#include "utils/broker_request_comm.hpp"
#include "utils/broker_service.hpp"
#include "utils/hub_state.hpp"
#include "utils/channel_access_policy.hpp"

#include "log_capture_fixture.h"

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

// ── Helper: try to register via BRC; returns true on success ────────────────

bool try_register(const std::string &endpoint,
                  const std::string &pubkey,
                  const std::string &channel,
                  const std::string &role_name = {},
                  const std::string &role_uid  = {})
{
    BrokerRequestComm brc;
    BrokerRequestComm::Config cfg;
    cfg.broker_endpoint = endpoint;
    cfg.broker_pubkey   = pubkey;
    cfg.role_uid        = role_uid;
    cfg.role_name       = role_name;
    if (!brc.connect(cfg))
        return false;

    std::atomic<bool> running{true};
    std::thread t([&] { brc.run_poll_loop([&] { return running.load(); }); });

    json opts;
    opts["channel_name"]      = channel;
    opts["pattern"]           = "PubSub";
    opts["has_shared_memory"] = false;
    opts["producer_pid"]      = ::getpid();
    opts["zmq_ctrl_endpoint"] = "tcp://127.0.0.1:0";
    opts["zmq_data_endpoint"] = "tcp://127.0.0.1:0";
    opts["zmq_pubkey"]        = "";
    opts["role_uid"]          = role_uid;
    opts["role_name"]         = role_name;

    auto result = brc.register_channel(opts, 3000);

    running.store(false);
    brc.stop();
    if (t.joinable())
        t.join();
    brc.disconnect();

    return result.has_value();
}

std::string pid_chan(const std::string &base)
{
    return base + ".pid" + std::to_string(getpid());
}

} // anonymous namespace

// ============================================================================
// Suite 1 — Enum conversion (no broker, no ZMQ)
// ============================================================================

class ConnectionPolicyEnumTest : public ::testing::Test
{
};
// (Pure enum/string conversion tests — no LOGGER paths exercised.)

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
    EXPECT_EQ(connection_policy_from_str("verfied"),  ConnectionPolicy::Open);
    EXPECT_EQ(connection_policy_from_str("REQUIRED"), ConnectionPolicy::Open);
    EXPECT_EQ(connection_policy_from_str(""),         ConnectionPolicy::Open);
    EXPECT_EQ(connection_policy_from_str("open "),    ConnectionPolicy::Open);
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
// Suite 2 — Broker policy enforcement via BrokerRequestComm
// ============================================================================

class ConnectionPolicyBrokerTest : public ::testing::Test,
                                    public pylabhub::tests::LogCaptureFixture
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
    void SetUp() override { LogCaptureFixture::Install(); }
    void TearDown() override
    {
        broker_.reset();
        AssertNoUnexpectedLogWarnError();
        LogCaptureFixture::Uninstall();
    }

    std::optional<LocalBrokerHandle> broker_;

    void StartBroker(BrokerService::Config cfg)
    {
        if (cfg.endpoint == "tcp://0.0.0.0:5570")
            cfg.endpoint = "tcp://127.0.0.1:0";
        broker_.emplace(start_local_broker(std::move(cfg)));
    }

    const std::string &ep() const { return broker_->endpoint; }
    const std::string &pk() const { return broker_->pubkey; }

private:
    static std::unique_ptr<LifecycleGuard> s_lifecycle_;
};

std::unique_ptr<LifecycleGuard> ConnectionPolicyBrokerTest::s_lifecycle_;

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
                             "lab.sensor1", "prod.sensor.uidaabbccdd"));
}

TEST_F(ConnectionPolicyBrokerTest, RequiredPolicyRejectsAnonymous)
{
    ExpectLogWarn("policy=required rejected producer");
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
                             "lab.sensor1", "prod.sensor.uidaabbccdd"));
}

TEST_F(ConnectionPolicyBrokerTest, VerifiedPolicyRejectsUnknownRole)
{
    ExpectLogWarn("Verified policy rejected producer");
    BrokerService::Config cfg;
    cfg.connection_policy = ConnectionPolicy::Verified;
    cfg.known_roles.push_back({"lab.sensor1", "prod.sensor.uidaabbccdd", "producer"});
    StartBroker(std::move(cfg));
    EXPECT_FALSE(try_register(ep(), pk(), pid_chan("lab.ver.unknown"),
                              "lab.intruder", "prod.intrude.uid11223344"));
}

TEST_F(ConnectionPolicyBrokerTest, VerifiedPolicyAcceptsKnownRole)
{
    BrokerService::Config cfg;
    cfg.connection_policy = ConnectionPolicy::Verified;
    cfg.known_roles.push_back({"lab.sensor1", "prod.sensor.uidaabbccdd", "producer"});
    StartBroker(std::move(cfg));
    EXPECT_TRUE(try_register(ep(), pk(), pid_chan("lab.ver.known"),
                             "lab.sensor1", "prod.sensor.uidaabbccdd"));
}

TEST_F(ConnectionPolicyBrokerTest, PerChannelGlobOverrideRestrictsChannel)
{
    ExpectLogWarn("Verified policy rejected producer");
    BrokerService::Config cfg;
    cfg.connection_policy = ConnectionPolicy::Tracked;
    cfg.channel_policies.push_back({"lab.secure.*", ConnectionPolicy::Verified});
    StartBroker(std::move(cfg));

    EXPECT_TRUE(try_register(ep(), pk(), pid_chan("lab.regular")));
    EXPECT_FALSE(try_register(ep(), pk(), "lab.secure." + std::to_string(getpid()),
                              "lab.sensor1", "prod.sensor.uidaabbccdd"));
}
