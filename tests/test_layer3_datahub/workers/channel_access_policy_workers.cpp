/**
 * @file channel_access_policy_workers.cpp
 * @brief Worker bodies for broker connection-policy enforcement tests
 *        (Pattern 3).  Migrated 2026-05-13 from the in-process
 *        `SetUpTestSuite`-owned `LifecycleGuard` antipattern.
 *
 * ─────────────────────────────────────────────────────────────────────────
 * IMPORTANT — placeholder-mechanism test, NOT production wiring.
 * ─────────────────────────────────────────────────────────────────────────
 *
 * This file tests `BrokerServiceImpl::check_connection_policy`, the
 * legacy role-identity gate that operates on self-asserted JSON
 * strings (`role_name` + `role_uid` from REG_REQ / CONSUMER_REG_REQ).
 * Per HEP-CORE-0035 §1 status banner, this mechanism is a
 * **placeholder pending retirement**: it never consults the CURVE
 * pubkey on the connecting socket and provides no actual
 * authentication.  HEP-0035 designs a two-layer ZAP-based replacement
 * (Layer 1: pubkey allowlist enforced at the ZMQ ZAP handler; Layer
 * 2: federation-trust gate consulting CURVE pubkey provenance).  As
 * of this commit, **no part of HEP-0035 is implemented** — the
 * placeholder is the only authorization mechanism in the broker.
 *
 * The placeholder is **structurally unreachable from production
 * hub.json today**: `HubBrokerConfig` (the hub.json `"broker":{...}`
 * parser at `src/include/utils/config/hub_broker_config.hpp:14-16`)
 * deliberately omits `connection_policy`, `known_roles`, and
 * `channel_policies` "pending HEP-CORE-0035."  The fields exist on
 * the C++ struct `BrokerService::Config` (defaulting to
 * `ConnectionPolicy::Open`) and `check_connection_policy` still runs
 * at every REG_REQ — but `effective_policy()` always evaluates to
 * Open in production because nothing else can set the fields.  This
 * test is therefore the only caller in the codebase that exercises
 * the Required / Verified / Tracked branches and the
 * channel-glob-override path.
 *
 * Because production cannot wire the fields, **real-HubHost
 * refactoring is structurally blocked** (the no-mocks principle in
 * `feedback_test_layering_and_no_mocks.md` cannot be applied here).
 * The workers below intentionally use direct `BrokerService::Config`
 * construction — not as the mock-host antipattern, but as the only
 * available reach to the placeholder's non-Open branches.  When
 * HEP-0035 lands (Phase 1+2 ZAP handler → Phase 3 HubBrokerConfig
 * re-adding the fields with `pubkey` required → Phase 6 deletion of
 * the entire placeholder + this test file), this file disappears
 * with the mechanism it pins.
 *
 * ─────────────────────────────────────────────────────────────────────────
 *
 * Module surface: Logger + CryptoUtils + ZMQContext.  Matches the
 * original `SetUpTestSuite` — no FileLock / JsonConfig because the
 * direct `BrokerService::Config` construction skips HubConfig
 * loading entirely.
 *
 * @see HEP-CORE-0035 §1 (placeholder status), §3 (gap analysis), §8 (Phase 6 deletion plan)
 * @see src/include/utils/channel_access_policy.hpp (legacy types)
 * @see src/utils/ipc/broker_service.cpp::check_connection_policy (gate impl)
 */

#include "channel_access_policy_workers.h"

#include "log_capture_fixture.h"
#include "plh_datahub.hpp"
#include "plh_service.hpp"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"
#include "utils/broker_request_comm.hpp"
#include "utils/broker_service.hpp"
#include "utils/channel_access_policy.hpp"
#include "utils/hub_state.hpp"
#include "utils/logger.hpp"

#include <atomic>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <unistd.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace pylabhub::utils;
using namespace pylabhub::hub;
using namespace pylabhub::broker;
using pylabhub::tests::LogCaptureFixture;
using pylabhub::tests::helper::run_gtest_worker;
using json = nlohmann::json;

namespace pylabhub::tests::worker
{
namespace channel_access_policy
{

namespace
{

// ── LocalBrokerHandle (direct construction — see file header) ───────────────

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

/// Attempt to register a channel via BrokerRequestComm; returns true on
/// `status == "success"`, false on any non-success response or transport
/// failure.  Drains its poll thread cleanly before returning.
bool try_register(const std::string &endpoint, const std::string &pubkey,
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
    opts["role_uid"]          = role_uid;
    opts["role_name"]         = role_name;

    auto result = brc.register_channel(opts, 3000);

    running.store(false);
    brc.stop();
    if (t.joinable())
        t.join();
    brc.disconnect();

    return result.has_value() &&
           result->value("status", std::string{}) == "success";
}

std::string pid_chan(const std::string &base)
{
    return base + ".pid" + std::to_string(::getpid());
}

/// Common worker prologue: install LogCaptureFixture, run the body
/// with a freshly-started LocalBrokerHandle, then AssertNoUnexpectedLogWarnError
/// + uninstall.  Body receives a const reference to the started
/// broker handle's `endpoint` and `pubkey` strings.
template <typename Body>
int run_with_broker(std::string_view worker_name,
                    BrokerService::Config cfg,
                    Body &&body,
                    std::vector<std::string> expect_log_warns = {})
{
    return run_gtest_worker(
        [cfg = std::move(cfg),
         body = std::forward<Body>(body),
         expect_log_warns = std::move(expect_log_warns)]() mutable {
            LogCaptureFixture log_cap;
            log_cap.Install();
            for (auto &w : expect_log_warns)
                log_cap.ExpectLogWarn(w);

            auto broker = start_local_broker(std::move(cfg));
            body(broker.endpoint, broker.pubkey);

            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
        },
        std::string(worker_name).c_str(),
        Logger::GetLifecycleModule(),
        pylabhub::crypto::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

BrokerService::Config base_cfg()
{
    BrokerService::Config cfg;
    cfg.endpoint = "tcp://127.0.0.1:0";
    return cfg;
}

} // namespace

int open_policy_accepts_anonymous()
{
    auto cfg = base_cfg();
    cfg.connection_policy = ConnectionPolicy::Open;
    return run_with_broker(
        "channel_access_policy::open_policy_accepts_anonymous",
        std::move(cfg),
        [](const std::string &ep, const std::string &pk) {
            EXPECT_TRUE(try_register(ep, pk, pid_chan("lab.open.anon")));
        });
}

int open_policy_accepts_with_identity()
{
    auto cfg = base_cfg();
    cfg.connection_policy = ConnectionPolicy::Open;
    return run_with_broker(
        "channel_access_policy::open_policy_accepts_with_identity",
        std::move(cfg),
        [](const std::string &ep, const std::string &pk) {
            EXPECT_TRUE(try_register(ep, pk, pid_chan("lab.open.id"),
                                      "lab.sensor1",
                                      "prod.sensor.uidaabbccdd"));
        });
}

int required_policy_rejects_anonymous()
{
    auto cfg = base_cfg();
    cfg.connection_policy = ConnectionPolicy::Required;
    return run_with_broker(
        "channel_access_policy::required_policy_rejects_anonymous",
        std::move(cfg),
        [](const std::string &ep, const std::string &pk) {
            EXPECT_FALSE(try_register(ep, pk, pid_chan("lab.req.anon")));
        },
        {"policy=required rejected producer"});
}

int required_policy_accepts_with_identity()
{
    auto cfg = base_cfg();
    cfg.connection_policy = ConnectionPolicy::Required;
    return run_with_broker(
        "channel_access_policy::required_policy_accepts_with_identity",
        std::move(cfg),
        [](const std::string &ep, const std::string &pk) {
            EXPECT_TRUE(try_register(ep, pk, pid_chan("lab.req.id"),
                                      "lab.sensor1",
                                      "prod.sensor.uidaabbccdd"));
        });
}

int verified_policy_rejects_unknown_role()
{
    auto cfg = base_cfg();
    cfg.connection_policy = ConnectionPolicy::Verified;
    cfg.known_roles.push_back(
        {"lab.sensor1", "prod.sensor.uidaabbccdd", "producer"});
    return run_with_broker(
        "channel_access_policy::verified_policy_rejects_unknown_role",
        std::move(cfg),
        [](const std::string &ep, const std::string &pk) {
            EXPECT_FALSE(try_register(ep, pk, pid_chan("lab.ver.unknown"),
                                       "lab.intruder",
                                       "prod.intrude.uid11223344"));
        },
        {"Verified policy rejected producer"});
}

int verified_policy_accepts_known_role()
{
    auto cfg = base_cfg();
    cfg.connection_policy = ConnectionPolicy::Verified;
    cfg.known_roles.push_back(
        {"lab.sensor1", "prod.sensor.uidaabbccdd", "producer"});
    return run_with_broker(
        "channel_access_policy::verified_policy_accepts_known_role",
        std::move(cfg),
        [](const std::string &ep, const std::string &pk) {
            EXPECT_TRUE(try_register(ep, pk, pid_chan("lab.ver.known"),
                                      "lab.sensor1",
                                      "prod.sensor.uidaabbccdd"));
        });
}

int per_channel_glob_override_restricts_channel()
{
    auto cfg = base_cfg();
    cfg.connection_policy = ConnectionPolicy::Tracked;
    cfg.channel_policies.push_back(
        {"lab.secure.*", ConnectionPolicy::Verified});
    return run_with_broker(
        "channel_access_policy::per_channel_glob_override_restricts_channel",
        std::move(cfg),
        [](const std::string &ep, const std::string &pk) {
            // Non-matching channel: base Tracked policy allows anonymous.
            EXPECT_TRUE(try_register(ep, pk, pid_chan("lab.regular")));
            // Matching channel: overridden to Verified; "lab.sensor1" is
            // NOT in cfg.known_roles (we didn't populate it for this
            // test), so rejection is expected.
            EXPECT_FALSE(try_register(
                ep, pk, "lab.secure." + std::to_string(::getpid()),
                "lab.sensor1", "prod.sensor.uidaabbccdd"));
        },
        {"Verified policy rejected producer"});
}

} // namespace channel_access_policy
} // namespace pylabhub::tests::worker

// ── Dispatcher registrar ────────────────────────────────────────────────────

namespace
{

struct ChannelAccessPolicyRegistrar
{
    ChannelAccessPolicyRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2) return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "channel_access_policy")
                    return -1;
                std::string sc(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::channel_access_policy;

                if (sc == "open_policy_accepts_anonymous")
                    return open_policy_accepts_anonymous();
                if (sc == "open_policy_accepts_with_identity")
                    return open_policy_accepts_with_identity();
                if (sc == "required_policy_rejects_anonymous")
                    return required_policy_rejects_anonymous();
                if (sc == "required_policy_accepts_with_identity")
                    return required_policy_accepts_with_identity();
                if (sc == "verified_policy_rejects_unknown_role")
                    return verified_policy_rejects_unknown_role();
                if (sc == "verified_policy_accepts_known_role")
                    return verified_policy_accepts_known_role();
                if (sc == "per_channel_glob_override_restricts_channel")
                    return per_channel_glob_override_restricts_channel();
                return -1;
            });
    }
} g_registrar;

} // namespace
