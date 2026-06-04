/**
 * @file broker_consumer_workers.cpp
 * @brief Worker bodies for consumer registration protocol integration
 *        tests (Pattern 3).  Migrated 2026-05-13 from in-process
 *        `SetUpTestSuite`-owned `LifecycleGuard`.
 */

#include "broker_consumer_workers.h"

#include "broker_test_harness.h"
#include "log_capture_fixture.h"
#include "plh_datahub.hpp"
#include "plh_service.hpp"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"
#include "utils/broker_request_comm.hpp"
#include "utils/file_lock.hpp"
#include "utils/hub_host.hpp"
#include "utils/json_config.hpp"
#include "utils/logger.hpp"

#include <atomic>
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
using pylabhub::tests::LogCaptureFixture;
using pylabhub::tests::helper::run_gtest_worker;
using json = nlohmann::json;

namespace pylabhub::tests::worker
{
namespace broker_consumer
{

namespace
{

json make_reg_opts(const std::string &channel, const std::string &role_uid,
                   uint64_t producer_pid)
{
    json opts;
    opts["channel_name"]      = channel;
    opts["pattern"]           = "PubSub";
    opts["has_shared_memory"] = false;
    opts["producer_pid"]      = producer_pid;
    opts["role_uid"]          = role_uid;
    opts["role_name"]         = "test_producer";
    return opts;
}

json make_cons_opts(const std::string &channel,
                    const std::string &consumer_uid, uint64_t consumer_pid)
{
    json opts;
    opts["channel_name"]   = channel;
    opts["role_uid"]   = consumer_uid;
    opts["role_name"]  = "test_consumer";
    opts["consumer_pid"]   = consumer_pid;
    return opts;
}

std::string pid_chan(const std::string &base)
{
    return base + ".pid" + std::to_string(::getpid());
}

json hubhost_overrides()
{
    return json{
        {"network", {{"broker_endpoint", "tcp://127.0.0.1:0"}}},
        {"admin",   {{"enabled", false}}},
        {"script",  {{"path", ""}}},
    };
}

/// Run a worker body with a freshly spun-up HubHostBrokerHandle +
/// LogCaptureFixture under real CURVE + admission (HEP-CORE-0035
/// §2 + §4.6.5).  The body receives both the broker handle and
/// the CurveSetup so it can mint BRC clients with their pre-
/// admitted keypairs via `curve.role(uid)`.
template <typename Body>
int run_with_host(std::string_view worker_name,
                  std::vector<std::string> role_uids,
                  Body &&body,
                  std::vector<std::string> expect_log_warns = {})
{
    return run_gtest_worker(
        [role_uids = std::move(role_uids),
         body = std::forward<Body>(body),
         expect_log_warns = std::move(expect_log_warns)]() mutable {
            LogCaptureFixture log_cap;
            log_cap.Install();
            for (auto &w : expect_log_warns)
                log_cap.ExpectLogWarn(w);

            auto curve = pylabhub::tests::make_curve_setup(role_uids);
            auto broker = pylabhub::tests::start_hubhost_broker(
                hubhost_overrides(), curve);
            ASSERT_TRUE(broker.host && broker.host->is_running());

            body(broker, curve);

            broker.stop_and_join();
            log_cap.AssertNoUnexpectedLogWarnError();
            log_cap.Uninstall();
        },
        std::string(worker_name).c_str(),
        Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule(),
        pylabhub::crypto::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

} // namespace

int consumer_reg_channel_not_found()
{
    const std::string uid = "cons.unknown.uid001";
    return run_with_host(
        "broker_consumer::consumer_reg_channel_not_found",
        {uid},
        [uid](pylabhub::tests::HubHostBrokerHandle &broker,
              pylabhub::tests::CurveSetup &curve) {
            pylabhub::tests::BrcHandle bh;
            bh.start(broker.endpoint, broker.pubkey, uid, curve.role(uid));

            auto resp = bh.brc.register_consumer(
                make_cons_opts(pid_chan("consumer.no_such_channel"),
                                uid, 12345),
                3000);
            ASSERT_TRUE(resp.has_value());
            EXPECT_EQ(resp->value("status", std::string{}), "error");
            EXPECT_EQ(resp->value("error_code", std::string{}),
                      "CHANNEL_NOT_FOUND");
        },
        {"CONSUMER_REG_REQ channel"});
}

int consumer_reg_happy_path()
{
    const std::string channel  = pid_chan("consumer.reg_happy");
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;
    return run_with_host(
        "broker_consumer::consumer_reg_happy_path",
        {prod_uid, cons_uid},
        [channel, prod_uid, cons_uid](
            pylabhub::tests::HubHostBrokerHandle &broker,
            pylabhub::tests::CurveSetup &curve) {
            pylabhub::tests::BrcHandle prod;
            prod.start(broker.endpoint, broker.pubkey, prod_uid,
                       curve.role(prod_uid));
            auto reg = prod.brc.register_channel(
                make_reg_opts(channel, prod_uid, 55001), 3000);
            ASSERT_TRUE(reg.has_value()) << "register_channel failed";
            ASSERT_EQ(reg->value("status", std::string{}), "success");

            prod.brc.send_heartbeat(channel, prod_uid, "producer",
                                     json::object());

            pylabhub::tests::BrcHandle cons;
            cons.start(broker.endpoint, broker.pubkey, cons_uid,
                       curve.role(cons_uid));
            auto creg = cons.brc.register_consumer(
                make_cons_opts(channel, cons_uid, 55100), 3000);
            ASSERT_TRUE(creg.has_value());
            EXPECT_EQ(creg->value("status", std::string{}), "success");

            auto disc = cons.brc.discover_channel(channel, json::object(),
                                                    3000);
            ASSERT_TRUE(disc.has_value());
            EXPECT_EQ(disc->value("status", std::string{}), "success");
            EXPECT_GE(disc->value("consumer_count", uint32_t{0}), 1u);
        });
}

int consumer_dereg_happy_path()
{
    const std::string channel  = pid_chan("consumer.dereg_happy");
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;
    return run_with_host(
        "broker_consumer::consumer_dereg_happy_path",
        {prod_uid, cons_uid},
        [channel, prod_uid, cons_uid](
            pylabhub::tests::HubHostBrokerHandle &broker,
            pylabhub::tests::CurveSetup &curve) {
            const uint64_t my_pid = static_cast<uint64_t>(::getpid());

            pylabhub::tests::BrcHandle prod;
            prod.start(broker.endpoint, broker.pubkey, prod_uid,
                       curve.role(prod_uid));
            ASSERT_TRUE(prod.brc.register_channel(
                            make_reg_opts(channel, prod_uid, my_pid), 3000)
                            .has_value());
            prod.brc.send_heartbeat(channel, prod_uid, "producer",
                                     json::object());

            pylabhub::tests::BrcHandle cons;
            cons.start(broker.endpoint, broker.pubkey, cons_uid,
                       curve.role(cons_uid));
            ASSERT_TRUE(cons.brc.register_consumer(
                            make_cons_opts(channel, cons_uid, my_pid), 3000)
                            .has_value());

            auto disc1 = cons.brc.discover_channel(channel, json::object(),
                                                     3000);
            ASSERT_TRUE(disc1.has_value());
            EXPECT_EQ(disc1->value("consumer_count", uint32_t{99}), 1u);

            {
                auto dereg_resp = cons.brc.deregister_consumer(channel, 3000);
                ASSERT_TRUE(dereg_resp.has_value());
                EXPECT_EQ(dereg_resp->value("status", std::string{}),
                          "success");
            }

            auto disc2 = cons.brc.discover_channel(channel, json::object(),
                                                     3000);
            ASSERT_TRUE(disc2.has_value());
            EXPECT_EQ(disc2->value("consumer_count", uint32_t{99}), 0u);
        });
}

int consumer_dereg_pid_mismatch()
{
    const std::string channel = pid_chan("consumer.dereg_pid_mismatch");
    const std::string prod_uid          = "prod." + channel;
    const std::string cons_uid_correct  = "cons." + channel + ".correct";
    const std::string cons_uid_wrong    = "cons." + channel + ".wrong";
    return run_with_host(
        "broker_consumer::consumer_dereg_pid_mismatch",
        {prod_uid, cons_uid_correct, cons_uid_wrong},
        [channel, prod_uid, cons_uid_correct, cons_uid_wrong](
            pylabhub::tests::HubHostBrokerHandle &broker,
            pylabhub::tests::CurveSetup &curve) {
            pylabhub::tests::BrcHandle prod;
            prod.start(broker.endpoint, broker.pubkey, prod_uid,
                       curve.role(prod_uid));
            ASSERT_TRUE(prod.brc.register_channel(
                            make_reg_opts(channel, prod_uid, 56000), 3000)
                            .has_value());
            prod.brc.send_heartbeat(channel, prod_uid, "producer",
                                     json::object());

            pylabhub::tests::BrcHandle cons_correct;
            cons_correct.start(broker.endpoint, broker.pubkey,
                               cons_uid_correct,
                               curve.role(cons_uid_correct));
            ASSERT_TRUE(cons_correct.brc
                            .register_consumer(make_cons_opts(channel,
                                                                cons_uid_correct,
                                                                56001),
                                                3000)
                            .has_value());

            pylabhub::tests::BrcHandle cons_wrong;
            cons_wrong.start(broker.endpoint, broker.pubkey, cons_uid_wrong,
                             curve.role(cons_uid_wrong));
            auto dereg_resp = cons_wrong.brc.deregister_consumer(channel, 3000);
            ASSERT_TRUE(dereg_resp.has_value())
                << "Broker should respond, not time out";
            EXPECT_EQ(dereg_resp->value("status", std::string{}), "error");
            EXPECT_EQ(dereg_resp->value("error_code", std::string{}),
                      "NOT_REGISTERED");

            auto disc = cons_correct.brc.discover_channel(
                channel, json::object(), 3000);
            ASSERT_TRUE(disc.has_value());
            EXPECT_EQ(disc->value("consumer_count", uint32_t{0}), 1u);
        },
        {"CONSUMER_DEREG_REQ failed"});
}

int disc_shows_consumer_count()
{
    const std::string channel  = pid_chan("consumer.disc_count");
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;
    const std::string obs_uid  = "observer." + channel;
    return run_with_host(
        "broker_consumer::disc_shows_consumer_count",
        {prod_uid, cons_uid, obs_uid},
        [channel, prod_uid, cons_uid, obs_uid](
            pylabhub::tests::HubHostBrokerHandle &broker,
            pylabhub::tests::CurveSetup &curve) {
            const uint64_t my_pid = static_cast<uint64_t>(::getpid());

            pylabhub::tests::BrcHandle prod;
            prod.start(broker.endpoint, broker.pubkey, prod_uid,
                       curve.role(prod_uid));
            ASSERT_TRUE(prod.brc.register_channel(
                            make_reg_opts(channel, prod_uid, my_pid), 3000)
                            .has_value());
            prod.brc.send_heartbeat(channel, prod_uid, "producer",
                                     json::object());

            pylabhub::tests::BrcHandle observer;
            observer.start(broker.endpoint, broker.pubkey, obs_uid,
                           curve.role(obs_uid));

            auto disc0 = observer.brc.discover_channel(channel, json::object(),
                                                        3000);
            ASSERT_TRUE(disc0.has_value());
            EXPECT_EQ(disc0->value("consumer_count", uint32_t{99}), 0u);

            pylabhub::tests::BrcHandle cons;
            cons.start(broker.endpoint, broker.pubkey, cons_uid,
                       curve.role(cons_uid));
            ASSERT_TRUE(cons.brc.register_consumer(
                            make_cons_opts(channel, cons_uid, my_pid), 3000)
                            .has_value());

            auto disc1 = observer.brc.discover_channel(channel, json::object(),
                                                        3000);
            ASSERT_TRUE(disc1.has_value());
            EXPECT_EQ(disc1->value("consumer_count", uint32_t{99}), 1u);

            {
                auto dereg_resp = cons.brc.deregister_consumer(channel, 3000);
                ASSERT_TRUE(dereg_resp.has_value());
                EXPECT_EQ(dereg_resp->value("status", std::string{}),
                          "success");
            }

            auto disc2 = observer.brc.discover_channel(channel, json::object(),
                                                        3000);
            ASSERT_TRUE(disc2.has_value());
            EXPECT_EQ(disc2->value("consumer_count", uint32_t{99}), 0u);
        });
}

} // namespace broker_consumer
} // namespace pylabhub::tests::worker

// ── Dispatcher registrar ────────────────────────────────────────────────────

namespace
{

struct BrokerConsumerRegistrar
{
    BrokerConsumerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2) return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "broker_consumer")
                    return -1;
                std::string sc(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::broker_consumer;

                if (sc == "consumer_reg_channel_not_found")
                    return consumer_reg_channel_not_found();
                if (sc == "consumer_reg_happy_path")
                    return consumer_reg_happy_path();
                if (sc == "consumer_dereg_happy_path")
                    return consumer_dereg_happy_path();
                if (sc == "consumer_dereg_pid_mismatch")
                    return consumer_dereg_pid_mismatch();
                if (sc == "disc_shows_consumer_count")
                    return disc_shows_consumer_count();
                return -1;
            });
    }
} g_registrar;

} // namespace
