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
#include "utils/role_reg_payload.hpp"
#include "utils/security/shm_capability_channel.hpp" // #281 default_shm_capability_endpoint

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

// Thin wrappers around the canonical production payload builders
// (`pylabhub::hub::build_*_reg_payload` in `utils/role_reg_payload.hpp`).
// `producer_pid` / `consumer_pid` parameters retained because some
// scenarios pin specific pid values; we layer them on top of the
// builder's default `get_pid()`.
json make_reg_opts(const std::string &channel, const std::string &role_uid,
                   const std::string &zmq_pubkey, uint64_t producer_pid)
{
    // #281 (2026-06-23): `data_transport` REQUIRED — mirror production
    // SHM wire shape (HEP-CORE-0041 §5.1).  Broker only stores the
    // endpoint string; no L2 listener bound by this helper.
    auto opts = pylabhub::hub::build_producer_reg_payload(
        pylabhub::hub::ProducerRegInputs{
            .channel    = channel,
            .role_uid   = role_uid,
            .role_name  = "test_producer",
            .role_type   = "producer",
            .has_shm    = true,
            .is_zmq_transport  = false,
            .zmq_node_endpoint = {},
            .zmq_pubkey = zmq_pubkey,
            .shm_capability_endpoint =
                pylabhub::utils::security::default_shm_capability_endpoint(channel),
        });
    opts["producer_pid"] = producer_pid;
    return opts;
}

json make_cons_opts(const std::string &channel,
                    const std::string &consumer_uid,
                    const std::string &zmq_pubkey,
                    uint64_t consumer_pid)
{
    auto opts = pylabhub::hub::build_consumer_reg_payload(
        pylabhub::hub::ConsumerRegInputs{
            .channel    = channel,
            .role_uid   = consumer_uid,
            .role_name  = "test_consumer",
            .zmq_pubkey = zmq_pubkey,
        });
    opts["consumer_pid"] = consumer_pid;
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
            // HEP-CORE-0040 §172: fixture owns SMS + KeyStore + identity
            // seeding; start_hubhost_broker only reads.
            pylabhub::tests::CurveKeyStoreFixture ks_fixture(
                "test.l3", "test.broker_consumer.harness", curve);
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
            bh.start(broker.endpoint, broker.pubkey, uid, pylabhub::tests::role_keystore_name(uid));

            auto resp = bh.brc.register_consumer(
                make_cons_opts(pid_chan("consumer.no_such_channel"),
                                uid, curve.role(uid).public_z85, 12345),
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
                       pylabhub::tests::role_keystore_name(prod_uid));
            auto reg = prod.brc.register_channel(
                make_reg_opts(channel, prod_uid, curve.role(prod_uid).public_z85, 55001), 3000);
            ASSERT_TRUE(reg.has_value()) << "register_channel failed";
            ASSERT_EQ(reg->value("status", std::string{}), "success");

            prod.brc.send_heartbeat(channel, prod_uid, "producer",
                                     json::object());

            pylabhub::tests::BrcHandle cons;
            cons.start(broker.endpoint, broker.pubkey, cons_uid,
                       pylabhub::tests::role_keystore_name(cons_uid));
            auto creg = cons.brc.register_consumer(
                make_cons_opts(channel, cons_uid, curve.role(cons_uid).public_z85, 55100), 3000);
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
                       pylabhub::tests::role_keystore_name(prod_uid));
            ASSERT_TRUE(prod.brc.register_channel(
                            make_reg_opts(channel, prod_uid, curve.role(prod_uid).public_z85, my_pid), 3000)
                            .has_value());
            prod.brc.send_heartbeat(channel, prod_uid, "producer",
                                     json::object());

            pylabhub::tests::BrcHandle cons;
            cons.start(broker.endpoint, broker.pubkey, cons_uid,
                       pylabhub::tests::role_keystore_name(cons_uid));
            ASSERT_TRUE(cons.brc.register_consumer(
                            make_cons_opts(channel, cons_uid, curve.role(cons_uid).public_z85, my_pid), 3000)
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
                       pylabhub::tests::role_keystore_name(prod_uid));
            ASSERT_TRUE(prod.brc.register_channel(
                            make_reg_opts(channel, prod_uid, curve.role(prod_uid).public_z85, 56000), 3000)
                            .has_value());
            prod.brc.send_heartbeat(channel, prod_uid, "producer",
                                     json::object());

            pylabhub::tests::BrcHandle cons_correct;
            cons_correct.start(broker.endpoint, broker.pubkey,
                               cons_uid_correct,
                               pylabhub::tests::role_keystore_name(cons_uid_correct));
            ASSERT_TRUE(cons_correct.brc
                            .register_consumer(make_cons_opts(channel,
                                                                cons_uid_correct,
                                                                curve.role(cons_uid_correct).public_z85,
                                                                56001),
                                                3000)
                            .has_value());

            pylabhub::tests::BrcHandle cons_wrong;
            cons_wrong.start(broker.endpoint, broker.pubkey, cons_uid_wrong,
                             pylabhub::tests::role_keystore_name(cons_uid_wrong));
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
                       pylabhub::tests::role_keystore_name(prod_uid));
            ASSERT_TRUE(prod.brc.register_channel(
                            make_reg_opts(channel, prod_uid, curve.role(prod_uid).public_z85, my_pid), 3000)
                            .has_value());
            prod.brc.send_heartbeat(channel, prod_uid, "producer",
                                     json::object());

            pylabhub::tests::BrcHandle observer;
            observer.start(broker.endpoint, broker.pubkey, obs_uid,
                           pylabhub::tests::role_keystore_name(obs_uid));

            auto disc0 = observer.brc.discover_channel(channel, json::object(),
                                                        3000);
            ASSERT_TRUE(disc0.has_value());
            EXPECT_EQ(disc0->value("consumer_count", uint32_t{99}), 0u);

            pylabhub::tests::BrcHandle cons;
            cons.start(broker.endpoint, broker.pubkey, cons_uid,
                       pylabhub::tests::role_keystore_name(cons_uid));
            ASSERT_TRUE(cons.brc.register_consumer(
                            make_cons_opts(channel, cons_uid, curve.role(cons_uid).public_z85, my_pid), 3000)
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

int consumer_reg_unknown_role()
{
    // HEP-CORE-0036 §6.3 Layer-2 verification step 1: when CONSUMER_REG_REQ
    // body claims a role_uid that is not in `cfg.known_roles[]`, broker
    // rejects with UNKNOWN_ROLE.  Layer-1 ZAP is satisfied because the
    // BRC authenticates with a legitimately-known role's key; the
    // verification gate then catches the body-level uid lie.
    //
    // Pre-registers a producer so the channel exists (CHANNEL_NOT_FOUND
    // would otherwise fire first).  Uses a syntactically-valid but
    // unregistered cons-uid in the body to surface UNKNOWN_ROLE
    // specifically.
    const std::string channel  = pid_chan("consumer.reg_unknown_role");
    const std::string prod_uid = "prod." + channel;
    const std::string real_uid = "cons.real." + channel;
    return run_with_host(
        "broker_consumer::consumer_reg_unknown_role",
        {prod_uid, real_uid},
        [channel, prod_uid, real_uid](
            pylabhub::tests::HubHostBrokerHandle &broker,
            pylabhub::tests::CurveSetup &curve) {
            pylabhub::tests::BrcHandle prod;
            prod.start(broker.endpoint, broker.pubkey, prod_uid,
                       pylabhub::tests::role_keystore_name(prod_uid));
            ASSERT_TRUE(prod.brc.register_channel(
                            make_reg_opts(channel, prod_uid,
                                           curve.role(prod_uid).public_z85,
                                           60000),
                            3000).has_value());
            prod.brc.send_heartbeat(channel, prod_uid, "producer",
                                     json::object());

            pylabhub::tests::BrcHandle bh;
            bh.start(broker.endpoint, broker.pubkey, real_uid,
                     pylabhub::tests::role_keystore_name(real_uid));

            // Body declares a syntactically-valid cons-uid that is NOT
            // in `cfg.known_roles[]`.  The connecting socket's CURVE
            // key IS in known_roles (real_uid's), so Layer-1 ZAP
            // passes; only Layer-2 catches the body lie.
            const std::string fake_uid =
                "cons.fabricated.unregistered_" + channel;
            auto resp = bh.brc.register_consumer(
                make_cons_opts(channel, fake_uid,
                                curve.role(real_uid).public_z85, 60001),
                3000);
            ASSERT_TRUE(resp.has_value());
            EXPECT_EQ(resp->value("status", std::string{}), "error");
            EXPECT_EQ(resp->value("error_code", std::string{}), "UNKNOWN_ROLE");
        },
        {"CONSUMER_REG_REQ rejected"});
}

int consumer_reg_pubkey_mismatch()
{
    // HEP-CORE-0036 §6.3 Layer-2 verification step 2: when
    // CONSUMER_REG_REQ body's role_uid IS in known_roles but
    // `body.zmq_pubkey` does not match `known_roles[role_uid].pubkey_z85`,
    // broker rejects with PUBKEY_MISMATCH.  This is the spoofing-defence
    // path — a compromised role authenticated via ZAP cannot register
    // as a different role uid using that other role's pubkey claim.
    //
    // Pre-registers a producer so the channel exists (CHANNEL_NOT_FOUND
    // would otherwise fire first).
    const std::string channel    = pid_chan("consumer.reg_pubkey_mismatch");
    const std::string prod_uid   = "prod." + channel;
    const std::string real_uid_a = "cons.real_a." + channel;
    const std::string real_uid_b = "cons.real_b." + channel;
    return run_with_host(
        "broker_consumer::consumer_reg_pubkey_mismatch",
        {prod_uid, real_uid_a, real_uid_b},
        [channel, prod_uid, real_uid_a, real_uid_b](
            pylabhub::tests::HubHostBrokerHandle &broker,
            pylabhub::tests::CurveSetup &curve) {
            pylabhub::tests::BrcHandle prod;
            prod.start(broker.endpoint, broker.pubkey, prod_uid,
                       pylabhub::tests::role_keystore_name(prod_uid));
            ASSERT_TRUE(prod.brc.register_channel(
                            make_reg_opts(channel, prod_uid,
                                           curve.role(prod_uid).public_z85,
                                           60000),
                            3000).has_value());
            prod.brc.send_heartbeat(channel, prod_uid, "producer",
                                     json::object());

            pylabhub::tests::BrcHandle bh;
            bh.start(broker.endpoint, broker.pubkey, real_uid_a,
                     pylabhub::tests::role_keystore_name(real_uid_a));

            // Body claims role_uid=real_uid_a (in known_roles) but
            // attaches real_uid_b's pubkey — the verification step
            // catches the (uid, pubkey) split.  Layer-1 ZAP still
            // passes because the CONNECTING socket uses real_uid_a's
            // real key.
            auto resp = bh.brc.register_consumer(
                make_cons_opts(channel, real_uid_a,
                                curve.role(real_uid_b).public_z85, 60002),
                3000);
            ASSERT_TRUE(resp.has_value());
            EXPECT_EQ(resp->value("status", std::string{}), "error");
            EXPECT_EQ(resp->value("error_code", std::string{}),
                      "PUBKEY_MISMATCH");
        },
        {"CONSUMER_REG_REQ rejected"});
}

int consumer_reg_ack_emits_producers_zmq()
{
    // HEP-CORE-0036 §6.4 — CONSUMER_REG_ACK MUST carry a `producers[]`
    // array (transport=zmq only) so the consumer's role-host can
    // populate `RxQueueOptions::producer_peers` for the data-plane PULL
    // socket's CURVE handshake.  Each entry: {role_uid, pubkey,
    // endpoint}.  Single-producer pins the length-1 case.
    const std::string channel  = pid_chan("consumer.reg_ack_producers_zmq");
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;
    const std::string prod_endpoint = "tcp://127.0.0.1:55557";
    return run_with_host(
        "broker_consumer::consumer_reg_ack_emits_producers_zmq",
        {prod_uid, cons_uid},
        [channel, prod_uid, cons_uid, prod_endpoint](
            pylabhub::tests::HubHostBrokerHandle &broker,
            pylabhub::tests::CurveSetup &curve) {
            pylabhub::tests::BrcHandle prod;
            prod.start(broker.endpoint, broker.pubkey, prod_uid,
                       pylabhub::tests::role_keystore_name(prod_uid));

            auto reg_opts = make_reg_opts(
                channel, prod_uid, curve.role(prod_uid).public_z85, 55001);
            // Convert to ZMQ transport so the broker emits producers[]
            // per §6.4.  Post-B-4 (#289) SHM channels ALSO emit
            // producers[] with the same shape — the array's `endpoint`
            // is the SHM L2 capability URI rather than a ZMQ tcp:// —
            // and the broker echoes `data_transport` as the
            // consumer-side dispatcher.  This test pins the ZMQ case;
            // a sibling SHM pin would be added by B-4 follow-up tests.
            reg_opts["data_transport"]    = "zmq";
            reg_opts["zmq_node_endpoint"] = prod_endpoint;
            auto reg = prod.brc.register_channel(reg_opts, 3000);
            ASSERT_TRUE(reg.has_value());
            ASSERT_EQ(reg->value("status", std::string{}), "success");

            prod.brc.send_heartbeat(channel, prod_uid, "producer",
                                     json::object());

            pylabhub::tests::BrcHandle cons;
            cons.start(broker.endpoint, broker.pubkey, cons_uid,
                       pylabhub::tests::role_keystore_name(cons_uid));
            auto creg = cons.brc.register_consumer(
                make_cons_opts(channel, cons_uid,
                                curve.role(cons_uid).public_z85, 55100),
                3000);
            ASSERT_TRUE(creg.has_value());
            EXPECT_EQ(creg->value("status", std::string{}), "success");

            // D5 pin: producers[] present, length 1, single entry's
            // (role_uid, pubkey_z85, endpoint) match the producer's REG.
            // Post-B-4 (#289) the canonical pubkey key is `pubkey_z85`
            // — the pre-B-4 `pubkey` dual-name has been dropped.
            ASSERT_TRUE(creg->contains("producers"));
            const auto &producers = creg->at("producers");
            ASSERT_TRUE(producers.is_array());
            ASSERT_EQ(producers.size(), 1u);
            EXPECT_EQ(producers[0].value("role_uid", std::string{}),
                      prod_uid);
            EXPECT_EQ(producers[0].value("pubkey_z85", std::string{}),
                      curve.role(prod_uid).public_z85);
            EXPECT_EQ(producers[0].value("endpoint", std::string{}),
                      prod_endpoint);
            // B-4 pin: ACK echoes `data_transport` so consumers can
            // dispatch by value, not by "which field is present".
            EXPECT_EQ(creg->value("data_transport", std::string{}), "zmq");
            // B-4 mutation pin: ensure the pre-B-4 flat fields are NOT
            // present (the canonical shape is `producers[]` only).
            EXPECT_FALSE(creg->contains("shm_capability_endpoint"));
            EXPECT_FALSE(creg->contains("producer_pubkey_z85"));
            // B-4 mutation pin: the dual-name `pubkey` key on
            // producers[] entries has been retired.
            EXPECT_FALSE(producers[0].contains("pubkey"));
        });
}

int get_channel_auth_returns_allowlist()
{
    // HEP-CORE-0036 §6.5 GET_CHANNEL_AUTH_REQ pull — producer fetches
    // the current channel-scope allowlist via sync BRC call.  After a
    // consumer registers, the producer should observe the consumer's
    // pubkey in the pulled allowlist.  This is the lower half of D4's
    // notify-then-pull contract (the broker emits NOTIFY; producer's
    // worker thread reacts by calling this very API).
    const std::string channel  = pid_chan("auth.get_returns_allowlist");
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;
    return run_with_host(
        "broker_consumer::get_channel_auth_returns_allowlist",
        {prod_uid, cons_uid},
        [channel, prod_uid, cons_uid](
            pylabhub::tests::HubHostBrokerHandle &broker,
            pylabhub::tests::CurveSetup &curve) {
            pylabhub::tests::BrcHandle prod;
            prod.start(broker.endpoint, broker.pubkey, prod_uid,
                       pylabhub::tests::role_keystore_name(prod_uid));
            auto reg_opts = make_reg_opts(
                channel, prod_uid, curve.role(prod_uid).public_z85, 65001);
            reg_opts["data_transport"]    = "zmq";
            reg_opts["zmq_node_endpoint"] = "tcp://127.0.0.1:55558";
            ASSERT_TRUE(prod.brc.register_channel(reg_opts, 3000).has_value());
            prod.brc.send_heartbeat(channel, prod_uid, "producer",
                                     json::object());

            // Pre-registration: allowlist is empty.
            {
                auto pre = prod.brc.get_channel_auth(channel, prod_uid, 3000);
                ASSERT_TRUE(pre.has_value());
                EXPECT_EQ(pre->value("status", std::string{}), "success");
                ASSERT_TRUE(pre->contains("allowlist"));
                EXPECT_TRUE(pre->at("allowlist").is_array());
                EXPECT_EQ(pre->at("allowlist").size(), 0u);
            }

            // Consumer joins — broker mutates allowlist + fires NOTIFY.
            pylabhub::tests::BrcHandle cons;
            cons.start(broker.endpoint, broker.pubkey, cons_uid,
                       pylabhub::tests::role_keystore_name(cons_uid));
            // dereg matches by (consumer_pid, role_uid) per
            // broker_service.cpp:2640 + broker_request_comm.cpp:960
            // (`deregister_consumer` sends getpid()).  Register with
            // the same pid so the dereg-roundtrip assertion below
            // succeeds.
            ASSERT_TRUE(cons.brc.register_consumer(
                            make_cons_opts(channel, cons_uid,
                                            curve.role(cons_uid).public_z85,
                                            static_cast<uint64_t>(::getpid())),
                            3000).has_value());

            // Post-registration: allowlist includes consumer entry.
            // Per HEP-CORE-0036 §6.5 (locked 2026-06-12): entries are
            // bare Z85 pubkey strings — the role_uid is operator-side
            // metadata and is not carried on the wire (symmetric with
            // §6.2 REG_ACK.initial_allowlist + §7.2 cache shape).
            auto post = prod.brc.get_channel_auth(channel, prod_uid, 3000);
            ASSERT_TRUE(post.has_value());
            EXPECT_EQ(post->value("status", std::string{}), "success");
            ASSERT_TRUE(post->contains("allowlist"));
            const auto &al = post->at("allowlist");
            ASSERT_TRUE(al.is_array());
            ASSERT_EQ(al.size(), 1u);
            ASSERT_TRUE(al[0].is_string());
            EXPECT_EQ(al[0].get<std::string>(),
                      curve.role(cons_uid).public_z85);

            // Consumer dereg: broker mutates allowlist (revoke) + fires
            // CHANNEL_AUTH_CHANGED_NOTIFY (reason="consumer_left").  Pin
            // that the allowlist now reflects the empty state — the
            // contract dictates that revocation propagates within the
            // next producer pull (§I5: revocation prevents new
            // connections; existing sessions trusted).
            auto dereg = cons.brc.deregister_consumer(channel, 3000);
            ASSERT_TRUE(dereg.has_value());
            EXPECT_EQ(dereg->value("status", std::string{}), "success");

            auto after_dereg =
                prod.brc.get_channel_auth(channel, prod_uid, 3000);
            ASSERT_TRUE(after_dereg.has_value());
            EXPECT_EQ(after_dereg->value("status", std::string{}), "success");
            ASSERT_TRUE(after_dereg->contains("allowlist"));
            const auto &al2 = after_dereg->at("allowlist");
            ASSERT_TRUE(al2.is_array());
            EXPECT_EQ(al2.size(), 0u)
                << "allowlist must be empty after consumer dereg";
        });
}

// ── CONSUMER_ATTACH_REQ (HEP-CORE-0041 §9 D4) ────────────────────────────
//
// Pre-attach broker confirmation.  Producer queries broker before
// handing a SHM capability fd to a consumer; broker checks
// `authorized_consumer_pubkeys` and replies success | denied | ERROR.
//
// Test coverage (5 workers):
//   - consumer_attach_authorized       : success path
//   - consumer_attach_denied           : clean broker "no" (not ERROR)
//   - consumer_attach_channel_not_found: ERROR CHANNEL_NOT_FOUND
//   - consumer_attach_non_producer     : ERROR PRODUCER_NOT_AUTHORIZED
//   - consumer_attach_invalid_request  : ERROR INVALID_REQUEST

int consumer_attach_authorized()
{
    // Happy path: producer registered, consumer registered (in
    // allowlist), broker confirms attach.
    const std::string channel  = pid_chan("attach.authorized");
    const std::string prod_uid = "prod." + channel;
    const std::string cons_uid = "cons." + channel;
    return run_with_host(
        "broker_consumer::consumer_attach_authorized",
        {prod_uid, cons_uid},
        [channel, prod_uid, cons_uid](
            pylabhub::tests::HubHostBrokerHandle &broker,
            pylabhub::tests::CurveSetup          &curve) {
            pylabhub::tests::BrcHandle prod;
            prod.start(broker.endpoint, broker.pubkey, prod_uid,
                       pylabhub::tests::role_keystore_name(prod_uid));
            auto reg_opts = make_reg_opts(
                channel, prod_uid, curve.role(prod_uid).public_z85, 65010);
            reg_opts["data_transport"]    = "zmq";
            reg_opts["zmq_node_endpoint"] = "tcp://127.0.0.1:55570";
            ASSERT_TRUE(prod.brc.register_channel(reg_opts, 3000).has_value());
            prod.brc.send_heartbeat(channel, prod_uid, "producer",
                                    json::object());

            // Register consumer — broker mutates allowlist.
            pylabhub::tests::BrcHandle cons;
            cons.start(broker.endpoint, broker.pubkey, cons_uid,
                       pylabhub::tests::role_keystore_name(cons_uid));
            ASSERT_TRUE(cons.brc.register_consumer(
                            make_cons_opts(channel, cons_uid,
                                           curve.role(cons_uid).public_z85,
                                           static_cast<uint64_t>(::getpid())),
                            3000).has_value());

            // Producer asks: is this consumer authorized?
            auto resp = prod.brc.consumer_attach(
                channel,
                curve.role(cons_uid).public_z85,  // consumer_pubkey
                cons_uid,                          // consumer_role_uid
                prod_uid,                          // producer_role_uid
                3000);
            ASSERT_TRUE(resp.has_value());
            EXPECT_EQ(resp->value("status", std::string{}), "success")
                << "Broker must confirm 'success' for a registered consumer";
            EXPECT_EQ(resp->value("channel_name", std::string{}), channel);
            EXPECT_EQ(resp->value("consumer_pubkey", std::string{}),
                      curve.role(cons_uid).public_z85);
            EXPECT_FALSE(resp->contains("denial_reason"))
                << "denial_reason field must be absent on success";
        },
        {"event=ConsumerAttachAuthorized"});
}

int consumer_attach_denied()
{
    // Producer registered, but the queried consumer_pubkey is NOT in
    // the channel's allowlist (consumer never registered).  Broker
    // returns CONSUMER_ATTACH_ACK with status="denied" — NOT an ERROR
    // frame.  Substep 1e (producer cache divergence WARN) depends on
    // this distinction.
    const std::string channel       = pid_chan("attach.denied");
    const std::string prod_uid      = "prod." + channel;
    const std::string fake_cons_uid = "cons.unregistered." + channel;
    return run_with_host(
        "broker_consumer::consumer_attach_denied",
        {prod_uid, fake_cons_uid},
        [channel, prod_uid, fake_cons_uid](
            pylabhub::tests::HubHostBrokerHandle &broker,
            pylabhub::tests::CurveSetup          &curve) {
            pylabhub::tests::BrcHandle prod;
            prod.start(broker.endpoint, broker.pubkey, prod_uid,
                       pylabhub::tests::role_keystore_name(prod_uid));
            auto reg_opts = make_reg_opts(
                channel, prod_uid, curve.role(prod_uid).public_z85, 65011);
            reg_opts["data_transport"]    = "zmq";
            reg_opts["zmq_node_endpoint"] = "tcp://127.0.0.1:55571";
            ASSERT_TRUE(prod.brc.register_channel(reg_opts, 3000).has_value());
            prod.brc.send_heartbeat(channel, prod_uid, "producer",
                                    json::object());

            // Consumer's keypair exists in CurveSetup (so we have a
            // well-formed Z85 pubkey to query) but the consumer has
            // NOT called register_consumer — pubkey is NOT in the
            // channel's authorized_consumer_pubkeys.
            const std::string fake_pubkey =
                curve.role(fake_cons_uid).public_z85;

            auto resp = prod.brc.consumer_attach(
                channel, fake_pubkey, fake_cons_uid, prod_uid, 3000);
            ASSERT_TRUE(resp.has_value());
            EXPECT_EQ(resp->value("status", std::string{}), "denied")
                << "Unregistered consumer must yield status='denied' "
                   "(not 'error') so substep 1e can distinguish broker "
                   "decision from wire failure";
            EXPECT_EQ(resp->value("channel_name", std::string{}), channel);
            EXPECT_EQ(resp->value("consumer_pubkey", std::string{}),
                      fake_pubkey);
            EXPECT_TRUE(resp->contains("denial_reason"))
                << "denial_reason field must be present on denied";
        },
        {"event=ConsumerAttachDenied"});
}

int consumer_attach_channel_not_found()
{
    // Channel was never registered with the broker — ERROR
    // CHANNEL_NOT_FOUND, the standard error envelope.
    const std::string prod_uid = "prod.attach.no_channel";
    return run_with_host(
        "broker_consumer::consumer_attach_channel_not_found",
        {prod_uid},
        [prod_uid](pylabhub::tests::HubHostBrokerHandle &broker,
                   pylabhub::tests::CurveSetup          &curve) {
            pylabhub::tests::BrcHandle prod;
            prod.start(broker.endpoint, broker.pubkey, prod_uid,
                       pylabhub::tests::role_keystore_name(prod_uid));

            auto resp = prod.brc.consumer_attach(
                "nonexistent.channel",
                curve.role(prod_uid).public_z85,
                prod_uid,
                prod_uid,
                3000);
            ASSERT_TRUE(resp.has_value());
            EXPECT_EQ(resp->value("status", std::string{}), "error");
            EXPECT_EQ(resp->value("error_code", std::string{}),
                      "CHANNEL_NOT_FOUND");
        });
}

int consumer_attach_non_producer()
{
    // Channel exists, but the caller is NOT a registered producer of
    // it — ERROR PRODUCER_NOT_AUTHORIZED.  Defence in depth: even
    // though Layer-1 ZAP gated the connection, we never reveal another
    // channel's auth state to a non-producer caller.  Mirrors
    // GET_CHANNEL_AUTH_REQ's defence-in-depth.
    const std::string channel       = pid_chan("attach.non_prod");
    const std::string real_prod_uid = "prod." + channel;
    const std::string other_uid     = "cons.other." + channel;
    return run_with_host(
        "broker_consumer::consumer_attach_non_producer",
        {real_prod_uid, other_uid},
        [channel, real_prod_uid, other_uid](
            pylabhub::tests::HubHostBrokerHandle &broker,
            pylabhub::tests::CurveSetup          &curve) {
            pylabhub::tests::BrcHandle real_prod;
            real_prod.start(broker.endpoint, broker.pubkey, real_prod_uid,
                            pylabhub::tests::role_keystore_name(real_prod_uid));
            auto reg_opts = make_reg_opts(channel, real_prod_uid,
                                          curve.role(real_prod_uid).public_z85,
                                          65012);
            reg_opts["data_transport"]    = "zmq";
            reg_opts["zmq_node_endpoint"] = "tcp://127.0.0.1:55572";
            ASSERT_TRUE(real_prod.brc.register_channel(reg_opts, 3000).has_value());
            real_prod.brc.send_heartbeat(channel, real_prod_uid, "producer",
                                         json::object());

            // A different role (not a producer of channel) asks.
            pylabhub::tests::BrcHandle other;
            other.start(broker.endpoint, broker.pubkey, other_uid,
                        pylabhub::tests::role_keystore_name(other_uid));
            auto resp = other.brc.consumer_attach(
                channel,
                curve.role(other_uid).public_z85,
                other_uid,
                other_uid,  // caller declares its own uid as producer_role_uid
                3000);
            ASSERT_TRUE(resp.has_value());
            EXPECT_EQ(resp->value("status", std::string{}), "error");
            EXPECT_EQ(resp->value("error_code", std::string{}),
                      "PRODUCER_NOT_AUTHORIZED");
        },
        {"CONSUMER_ATTACH_REQ rejected"});
}

int consumer_attach_invalid_request()
{
    // Missing required field (consumer_pubkey is empty) — ERROR
    // INVALID_REQUEST.  Boundary validation.
    const std::string prod_uid = "prod.attach.invalid";
    return run_with_host(
        "broker_consumer::consumer_attach_invalid_request",
        {prod_uid},
        [prod_uid](pylabhub::tests::HubHostBrokerHandle &broker,
                   pylabhub::tests::CurveSetup          & /*curve*/) {
            pylabhub::tests::BrcHandle prod;
            prod.start(broker.endpoint, broker.pubkey, prod_uid,
                       pylabhub::tests::role_keystore_name(prod_uid));

            // consumer_pubkey deliberately empty — broker rejects at
            // shape validation, never reaches channel lookup.
            auto resp = prod.brc.consumer_attach(
                "any.channel",
                /*consumer_pubkey=*/"",
                "any.consumer.uid",
                prod_uid,
                3000);
            ASSERT_TRUE(resp.has_value());
            EXPECT_EQ(resp->value("status", std::string{}), "error");
            EXPECT_EQ(resp->value("error_code", std::string{}),
                      "INVALID_REQUEST");
        });
}

int get_channel_auth_rejects_non_producer()
{
    // HEP-CORE-0036 §6.6 PRODUCER_NOT_AUTHORIZED — a role that is NOT
    // a registered producer of the channel cannot pull the allowlist.
    // Defence-in-depth on top of Layer-1 ZAP.  Auth contract: only the
    // role that owns the channel's tx side may observe the allowlist
    // (no broadcasting of who's authorized to non-producers).
    const std::string channel       = pid_chan("auth.get_rejects_non_prod");
    const std::string real_prod_uid = "prod." + channel;
    const std::string other_uid     = "cons.other." + channel;
    return run_with_host(
        "broker_consumer::get_channel_auth_rejects_non_producer",
        {real_prod_uid, other_uid},
        [channel, real_prod_uid, other_uid](
            pylabhub::tests::HubHostBrokerHandle &broker,
            pylabhub::tests::CurveSetup &curve) {
            pylabhub::tests::BrcHandle prod;
            prod.start(broker.endpoint, broker.pubkey, real_prod_uid,
                       pylabhub::tests::role_keystore_name(real_prod_uid));
            auto reg_opts = make_reg_opts(channel, real_prod_uid,
                                           curve.role(real_prod_uid).public_z85,
                                           65200);
            reg_opts["data_transport"]    = "zmq";
            reg_opts["zmq_node_endpoint"] = "tcp://127.0.0.1:55559";
            ASSERT_TRUE(prod.brc.register_channel(reg_opts, 3000).has_value());
            prod.brc.send_heartbeat(channel, real_prod_uid, "producer",
                                     json::object());

            pylabhub::tests::BrcHandle other;
            other.start(broker.endpoint, broker.pubkey, other_uid,
                        pylabhub::tests::role_keystore_name(other_uid));
            auto resp = other.brc.get_channel_auth(channel, other_uid, 3000);
            ASSERT_TRUE(resp.has_value());
            EXPECT_EQ(resp->value("status", std::string{}), "error");
            EXPECT_EQ(resp->value("error_code", std::string{}),
                      "PRODUCER_NOT_AUTHORIZED");
        },
        {"GET_CHANNEL_AUTH_REQ rejected"});
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
                if (sc == "consumer_reg_unknown_role")
                    return consumer_reg_unknown_role();
                if (sc == "consumer_reg_pubkey_mismatch")
                    return consumer_reg_pubkey_mismatch();
                if (sc == "consumer_reg_ack_emits_producers_zmq")
                    return consumer_reg_ack_emits_producers_zmq();
                if (sc == "get_channel_auth_returns_allowlist")
                    return get_channel_auth_returns_allowlist();
                if (sc == "get_channel_auth_rejects_non_producer")
                    return get_channel_auth_rejects_non_producer();
                if (sc == "consumer_attach_authorized")
                    return consumer_attach_authorized();
                if (sc == "consumer_attach_denied")
                    return consumer_attach_denied();
                if (sc == "consumer_attach_channel_not_found")
                    return consumer_attach_channel_not_found();
                if (sc == "consumer_attach_non_producer")
                    return consumer_attach_non_producer();
                if (sc == "consumer_attach_invalid_request")
                    return consumer_attach_invalid_request();
                return -1;
            });
    }
} g_registrar;

} // namespace
