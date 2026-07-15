/**
 * @file zmq_endpoint_registry_workers.cpp
 * @brief Worker bodies for broker ZMQ endpoint registry tests
 *        (HEP-CORE-0021; Pattern 3).
 *
 * Migrated 2026-05-13 from in-process `SetUpTestSuite`-owned
 * `LifecycleGuard` to the worker-subprocess model.
 *
 * Refactored 2026-05-13 to use the real production assembly:
 * `HubConfig::load_from_directory` → `HubHost` → `host.broker()`,
 * matching broker_schema / broker_admin / broker_consumer.  Replaces
 * the legacy `LocalBrokerHandle` mock-host (raw `HubState` + bare
 * `BrokerService` + raw `std::thread`) per the test-design principle
 * in `feedback_test_layering_and_no_mocks.md`: L3 broker tests MUST
 * run against the real `HubHost` composite (real `BrokerService` +
 * real `HubState` + real `AdminService`) so that regressions in
 * HubHost's threading / lifecycle / state-ownership wiring are
 * actually caught.
 */

#include "zmq_endpoint_registry_workers.h"

#include "broker_test_harness.h"
#include "curve_test_setup.h"  // seed_curve_identities + role_keystore_name
#include "plh_datahub.hpp"
#include "plh_service.hpp"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"
#include "utils/broker_request_comm.hpp"
#include "utils/broker_service.hpp"
#include "utils/file_lock.hpp"
#include "utils/hub_host.hpp"
#include "utils/json_config.hpp"
#include "utils/logger.hpp"
#include "utils/zmq_context.hpp"

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <unistd.h>

#include <cppzmq/zmq.hpp>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace pylabhub::utils;
using namespace pylabhub::hub;
using namespace pylabhub::broker;
using pylabhub::tests::helper::run_gtest_worker;
using json = nlohmann::json;

namespace pylabhub::tests::worker
{
namespace zmq_endpoint_registry
{

namespace
{

json hubhost_overrides()
{
    return json{
        {"network", {{"broker_endpoint", "tcp://127.0.0.1:0"}}},
        {"admin",   {{"enabled", false}}},
        {"script",  {{"path", ""}}},
    };
}

std::string pid_chan(const std::string &base)
{
    return base + ".pid" + std::to_string(::getpid());
}

/// Run a worker body with a freshly spun-up HubHostBrokerHandle under
/// real CURVE + admission (HEP-CORE-0035 §2 + §4.6.5).  Body receives
/// (broker, curve).
///
/// Strict-CURVE migration (#154 AUTH-6 C5, 2026-06-30): each subprocess
/// now constructs a `seed_curve_identities()` that seeds
/// `kHubIdentityName` and `role.<uid>` for every uid the worker will
/// register, so the canonical `BrcHandle` can resolve client keys by
/// `role_keystore_name(uid)` per HEP-CORE-0040 §172.  REG_REQ payloads
/// come from the canonical `make_reg_opts` / `make_cons_opts` helpers
/// in the shared harness (they carry the §5b canonical fields the
/// broker now requires).
template <typename Body>
int run_with_host(std::string_view worker_name,
                  std::vector<std::string> role_uids,
                  Body &&body)
{
    return run_gtest_worker(
        [role_uids = std::move(role_uids),
         body = std::forward<Body>(body)]() mutable {
            auto curve = pylabhub::tests::make_curve_setup(role_uids);
            pylabhub::tests::seed_curve_identities(curve);
            auto broker = pylabhub::tests::start_hubhost_broker(
                hubhost_overrides(), curve);
            ASSERT_TRUE(broker.host && broker.host->is_running());

            body(broker, curve);

            broker.stop_and_join();
        },
        std::string(worker_name).c_str(),
        Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule(),
        pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

} // namespace

int default_transport_is_shm()
{
    const std::string channel  = pid_chan("zmqvc.default.shm");
    const std::string uid      = "prod.ep." + channel;
    const std::string cons_uid = "cons.ep." + channel;
    return run_with_host(
        "zmq_endpoint_registry::default_transport_is_shm",
        {uid, cons_uid},
        [channel, uid, cons_uid](
            pylabhub::tests::HubHostBrokerHandle &broker,
            pylabhub::tests::CurveSetup &curve) {
            pylabhub::tests::BrcHandle bh;
            bh.start(broker.endpoint, broker.pubkey, uid, pylabhub::tests::role_keystore_name(uid));
            auto reg = bh.brc.register_channel(pylabhub::tests::make_reg_opts(channel, uid),
                                                 3000);
            ASSERT_TRUE(reg.has_value());

            bh.brc.send_heartbeat(channel, uid, "producer", {});

            pylabhub::tests::BrcHandle cons_bh;
            cons_bh.start(broker.endpoint, broker.pubkey, cons_uid,
                          pylabhub::tests::role_keystore_name(cons_uid));
            auto disc = cons_bh.brc.discover_channel(channel, {}, 3000);
            ASSERT_TRUE(disc.has_value());
            EXPECT_EQ(disc->value("data_transport", ""), "shm");

            cons_bh.stop();
            bh.stop();
        });
}

int zmq_transport_round_trip()
{
    const std::string channel  = pid_chan("zmqvc.zmq.roundtrip");
    const std::string uid      = "prod.ep." + channel;
    const std::string cons_uid = "cons.ep." + channel;
    return run_with_host(
        "zmq_endpoint_registry::zmq_transport_round_trip",
        {uid, cons_uid},
        [channel, uid, cons_uid](
            pylabhub::tests::HubHostBrokerHandle &broker,
            pylabhub::tests::CurveSetup &curve) {
            const std::string zmq_ep = "tcp://127.0.0.1:55555";

            pylabhub::tests::BrcHandle bh;
            bh.start(broker.endpoint, broker.pubkey, uid, pylabhub::tests::role_keystore_name(uid));

            auto opts                 = pylabhub::tests::make_reg_opts(channel, uid);
            opts["data_transport"]    = "zmq";
            opts["zmq_node_endpoint"] = zmq_ep;
            auto reg = bh.brc.register_channel(opts, 3000);
            ASSERT_TRUE(reg.has_value());

            bh.brc.send_heartbeat(channel, uid, "producer", {});

            pylabhub::tests::BrcHandle cons_bh;
            cons_bh.start(broker.endpoint, broker.pubkey, cons_uid,
                          pylabhub::tests::role_keystore_name(cons_uid));
            auto disc = cons_bh.brc.discover_channel(channel, {}, 3000);
            ASSERT_TRUE(disc.has_value());
            EXPECT_EQ(disc->value("data_transport", ""), "zmq");
            EXPECT_EQ(disc->value("zmq_node_endpoint", ""), zmq_ep);

            cons_bh.stop();
            bh.stop();
        });
}

int multiple_consumers_discover_same_endpoint()
{
    const std::string channel = pid_chan("zmqvc.multi.disc");
    const std::string uid     = "prod.ep." + channel;
    const std::string c1_uid  = "CONS1-" + channel;
    const std::string c2_uid  = "CONS2-" + channel;
    return run_with_host(
        "zmq_endpoint_registry::multiple_consumers_discover_same_endpoint",
        {uid, c1_uid, c2_uid},
        [channel, uid, c1_uid, c2_uid](
            pylabhub::tests::HubHostBrokerHandle &broker,
            pylabhub::tests::CurveSetup &curve) {
            const std::string zmq_ep = "tcp://127.0.0.1:55556";

            pylabhub::tests::BrcHandle bh;
            bh.start(broker.endpoint, broker.pubkey, uid, pylabhub::tests::role_keystore_name(uid));

            auto opts                 = pylabhub::tests::make_reg_opts(channel, uid);
            opts["data_transport"]    = "zmq";
            opts["zmq_node_endpoint"] = zmq_ep;
            auto reg = bh.brc.register_channel(opts, 3000);
            ASSERT_TRUE(reg.has_value());

            bh.brc.send_heartbeat(channel, uid, "producer", {});

            pylabhub::tests::BrcHandle c1, c2;
            c1.start(broker.endpoint, broker.pubkey, c1_uid,
                     pylabhub::tests::role_keystore_name(c1_uid));
            c2.start(broker.endpoint, broker.pubkey, c2_uid,
                     pylabhub::tests::role_keystore_name(c2_uid));

            auto d1 = c1.brc.discover_channel(channel, {}, 3000);
            auto d2 = c2.brc.discover_channel(channel, {}, 3000);
            ASSERT_TRUE(d1.has_value());
            ASSERT_TRUE(d2.has_value());
            EXPECT_EQ(d1->value("zmq_node_endpoint", ""), zmq_ep);
            EXPECT_EQ(d2->value("zmq_node_endpoint", ""), zmq_ep);

            c2.stop();
            c1.stop();
            bh.stop();
        });
}

int shm_and_zmq_coexist()
{
    const std::string shm_ch  = pid_chan("zmqvc.coexist.shm");
    const std::string zmq_ch  = pid_chan("zmqvc.coexist.zmq");
    const std::string shm_uid = "prod.shm." + shm_ch;
    const std::string zmq_uid = "prod." + zmq_ch;
    return run_with_host(
        "zmq_endpoint_registry::shm_and_zmq_coexist",
        {shm_uid, zmq_uid},
        [shm_ch, zmq_ch, shm_uid, zmq_uid](
            pylabhub::tests::HubHostBrokerHandle &broker,
            pylabhub::tests::CurveSetup &curve) {
            pylabhub::tests::BrcHandle shm_bh;
            shm_bh.start(broker.endpoint, broker.pubkey, shm_uid,
                         pylabhub::tests::role_keystore_name(shm_uid));
            auto shm_reg = shm_bh.brc.register_channel(
                pylabhub::tests::make_reg_opts(shm_ch, shm_uid), 3000);
            ASSERT_TRUE(shm_reg.has_value());

            pylabhub::tests::BrcHandle zmq_bh;
            zmq_bh.start(broker.endpoint, broker.pubkey, zmq_uid,
                         pylabhub::tests::role_keystore_name(zmq_uid));
            auto zmq_opts                 = pylabhub::tests::make_reg_opts(zmq_ch, zmq_uid);
            zmq_opts["data_transport"]    = "zmq";
            zmq_opts["zmq_node_endpoint"] = "tcp://127.0.0.1:55557";
            auto zmq_reg = zmq_bh.brc.register_channel(zmq_opts, 3000);
            ASSERT_TRUE(zmq_reg.has_value());

            ChannelSnapshot snap = broker.service().query_channel_snapshot();
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
        });
}

int endpoint_update_reflected_in_discovery()
{
    const std::string channel  = pid_chan("zmqvc.ep.update");
    const std::string uid      = "prod.ep." + channel;
    const std::string cons_uid = "cons.ep." + channel;
    return run_with_host(
        "zmq_endpoint_registry::endpoint_update_reflected_in_discovery",
        {uid, cons_uid},
        [channel, uid, cons_uid](
            pylabhub::tests::HubHostBrokerHandle &broker,
            pylabhub::tests::CurveSetup &curve) {
            const std::string updated_ep = "tcp://127.0.0.1:44444";

            pylabhub::tests::BrcHandle bh;
            bh.start(broker.endpoint, broker.pubkey, uid, pylabhub::tests::role_keystore_name(uid));

            auto opts                 = pylabhub::tests::make_reg_opts(channel, uid);
            opts["data_transport"]    = "zmq";
            opts["zmq_node_endpoint"] = "tcp://127.0.0.1:0";
            auto reg = bh.brc.register_channel(opts, 3000);
            ASSERT_TRUE(reg.has_value());

            bh.brc.send_heartbeat(channel, uid, "producer", {});

            // ENDPOINT_UPDATE_REQ is Sync Request/Response per
            // HEP-CORE-0007 §12.2.1 + HEP-CORE-0021 §16.3.  The
            // broker mutates ProducerEntry.zmq_node_endpoint before
            // emitting ENDPOINT_UPDATE_ACK — the ACK is a durability
            // barrier.  After this call returns success, any
            // subsequent DISC_REQ from any client is guaranteed to
            // observe the updated endpoint.
            //
            // Pre-2026-05-21 this was a fire-and-forget cmd_queue
            // push.  The flaky failure traced to a race where the
            // consumer's discover_channel below could land on the
            // broker before the broker had applied the producer's
            // update; that race is eliminated by the sync wait.
            auto upd = bh.brc.send_endpoint_update(channel, "zmq_node",
                                                   updated_ep, 2000);
            ASSERT_TRUE(upd.has_value())
                << "ENDPOINT_UPDATE_REQ timed out / no reply from broker";
            ASSERT_EQ(upd->value("status", ""), "success")
                << "ENDPOINT_UPDATE_REQ returned non-success: "
                << upd->dump();

            pylabhub::tests::BrcHandle cons_bh;
            cons_bh.start(broker.endpoint, broker.pubkey, cons_uid,
                          pylabhub::tests::role_keystore_name(cons_uid));
            auto disc = cons_bh.brc.discover_channel(channel, {}, 5000);
            ASSERT_TRUE(disc.has_value()) << "DISC_REQ timed out";
            EXPECT_EQ(disc->value("zmq_node_endpoint", ""), updated_ep);

            cons_bh.stop();
            bh.stop();
        });
}

// ── ENDPOINT_UPDATE error-path coverage ─────────────────────────────────
// These pin the typed-error branches of the HEP-CORE-0021 §16.3 contract:
// the BRC's sync send_endpoint_update returns a value-with-error when the
// broker rejects, distinct from nullopt (timeout / transport failure).
// Both tests assert the path AND the typed `error_code` field, mutation-
// sweep verified (flipping the broker handler's rejection to "success"
// would make either test fail).
//
// Companion to endpoint_update_reflected_in_discovery (the happy-path).

int endpoint_update_non_producer_returns_error()
{
    const std::string channel   = pid_chan("zmqvc.ep.nonprod");
    const std::string prod_uid  = "prod.ep." + channel;
    const std::string other_uid = "cons.notprod." + channel;
    return run_with_host(
        "zmq_endpoint_registry::endpoint_update_non_producer_returns_error",
        {prod_uid, other_uid},
        [channel, prod_uid, other_uid](
            pylabhub::tests::HubHostBrokerHandle &broker,
            pylabhub::tests::CurveSetup &curve) {
            const std::string updated_ep = "tcp://127.0.0.1:44455";

            // Producer registers the channel.
            pylabhub::tests::BrcHandle prod_bh;
            prod_bh.start(broker.endpoint, broker.pubkey, prod_uid,
                          pylabhub::tests::role_keystore_name(prod_uid));
            auto opts                 = pylabhub::tests::make_reg_opts(channel, prod_uid);
            opts["data_transport"]    = "zmq";
            opts["zmq_node_endpoint"] = "tcp://127.0.0.1:0";
            auto reg = prod_bh.brc.register_channel(opts, 3000);
            ASSERT_TRUE(reg.has_value());

            // A DIFFERENT identity tries to update the producer's endpoint.
            // Per HEP-CORE-0021 §16.3 + broker_service.cpp:handle_endpoint_
            // update_req identity-resolution: sender_zmq_identity must match
            // a registered producer's zmq_identity, else NOT_CHANNEL_OWNER.
            pylabhub::tests::BrcHandle other_bh;
            other_bh.start(broker.endpoint, broker.pubkey, other_uid,
                           pylabhub::tests::role_keystore_name(other_uid));
            auto upd = other_bh.brc.send_endpoint_update(
                channel, "zmq_node", updated_ep, 2000);

            // Contract: the broker DID reply (sync path completed), but
            // the reply status is "error" with a typed error_code.
            ASSERT_TRUE(upd.has_value())
                << "ENDPOINT_UPDATE_REQ timed out — broker did not reply";
            EXPECT_EQ(upd->value("status", ""), "error")
                << "Non-producer sender should be rejected; got: "
                << upd->dump();
            EXPECT_EQ(upd->value("error_code", ""), "NOT_CHANNEL_OWNER")
                << "Expected NOT_CHANNEL_OWNER; got: "
                << upd->dump();

            // The producer's record on the broker MUST be unchanged —
            // a sibling discovery should still see the original port-0
            // placeholder, not the rejected `updated_ep`.
            auto disc = prod_bh.brc.discover_channel(channel, {}, 2000);
            // Discovery may return CHANNEL_NOT_READY because the producer
            // is still on port-0 — that's OK; we only need to confirm the
            // broker did NOT silently apply the rejected update.
            if (disc.has_value())
            {
                EXPECT_NE(disc->value("zmq_node_endpoint", ""), updated_ep)
                    << "Broker silently applied a rejected update";
            }

            other_bh.stop();
            prod_bh.stop();
        });
}

int endpoint_update_port_zero_returns_error()
{
    const std::string channel = pid_chan("zmqvc.ep.zeroport");
    const std::string uid     = "prod.ep." + channel;
    return run_with_host(
        "zmq_endpoint_registry::endpoint_update_port_zero_returns_error",
        {uid},
        [channel, uid](pylabhub::tests::HubHostBrokerHandle &broker,
                       pylabhub::tests::CurveSetup &curve) {
            pylabhub::tests::BrcHandle bh;
            bh.start(broker.endpoint, broker.pubkey, uid, pylabhub::tests::role_keystore_name(uid));

            auto opts                 = pylabhub::tests::make_reg_opts(channel, uid);
            opts["data_transport"]    = "zmq";
            opts["zmq_node_endpoint"] = "tcp://127.0.0.1:0";
            auto reg = bh.brc.register_channel(opts, 3000);
            ASSERT_TRUE(reg.has_value());

            // Attempt to update with an endpoint that has port 0.
            // Broker validate_tcp_endpoint rejects port 0 with
            // INVALID_ENDPOINT per broker_service.cpp:handle_endpoint_
            // update_req.
            auto upd = bh.brc.send_endpoint_update(
                channel, "zmq_node", "tcp://127.0.0.1:0", 2000);

            ASSERT_TRUE(upd.has_value())
                << "ENDPOINT_UPDATE_REQ timed out — broker did not reply";
            EXPECT_EQ(upd->value("status", ""), "error")
                << "Port-0 endpoint must be rejected; got: " << upd->dump();
            EXPECT_EQ(upd->value("error_code", ""), "INVALID_ENDPOINT")
                << "Expected INVALID_ENDPOINT; got: " << upd->dump();

            bh.stop();
        });
}

// ── HEP-CORE-0007 §12.2.1 REQ-shape-conformance observability ───────────
//
// Runtime check that no fire-and-forget REQ produces a reply at the BRC.
// The BRC's receive loop counts `unmatched_replies_count` whenever a
// reply-shape message (`*_ACK` or `ERROR`) arrives with no pending
// request waiter — that's the runtime fingerprint of a shape-contract
// violation:
//   - broker accidentally added `send_reply(...)` for a fire-and-forget
//     REQ (server side), OR
//   - BRC method declared `void` (cmd_queue.push) for a REQ whose
//     broker handler does send_reply (client side — the
//     ENDPOINT_UPDATE half-mix shipped 2026-05-21).
// Either way, the silently-dropped reply is observable now.

// ── RETIRED 2026-06-30 (#154 AUTH-6 C5 — handoff #307) ──────────────────
// Was: SilentRouterStub + StubBrcHandle + req_shape_sync_req_times_out_
// on_no_reply.  Drove BRC's `do_request` into its timeout branch via a
// plain-TCP ROUTER stub.  HEP-CORE-0035 §2 strict-CURVE now refuses
// `BrokerRequestComm::connect()` without broker_pubkey + KeyStore
// role_identity, so the plain-TCP bypass is dead.  Coverage handoff:
// #307 reinstates this either via a CURVE-capable silent router (option
// a) or an L2 test against BRC do_request directly with a mock socket
// (option b).

int req_shape_no_unmatched_replies_for_fire_and_forget()
{
    const std::string channel = pid_chan("zmqvc.shape");
    const std::string uid     = "prod.ep." + channel;
    return run_with_host(
        "zmq_endpoint_registry::req_shape_no_unmatched_replies_for_fire_and_forget",
        {uid},
        [channel, uid](pylabhub::tests::HubHostBrokerHandle &broker,
                       pylabhub::tests::CurveSetup &curve) {
            pylabhub::tests::BrcHandle bh;
            bh.start(broker.endpoint, broker.pubkey, uid, pylabhub::tests::role_keystore_name(uid));

            // Register channel — needed so heartbeat / broadcast / band
            // calls have a legitimate target.
            auto opts = pylabhub::tests::make_reg_opts(channel, uid);
            opts["data_transport"]    = "zmq";
            opts["zmq_node_endpoint"] = "tcp://127.0.0.1:44777";
            auto reg = bh.brc.register_channel(opts, 3000);
            ASSERT_TRUE(reg.has_value());

            // Snapshot baseline.  Should be 0 after a clean
            // register_channel — sync REQ matches its ACK on the wire.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            const size_t baseline = bh.brc.unmatched_replies();
            ASSERT_EQ(baseline, 0u)
                << "BRC saw " << baseline << " unmatched replies during "
                << "register_channel — sync REG_REQ/REG_ACK should be "
                << "fully waited-for.";

            // Exercise every fire-and-forget REQ declared in
            // HEP-CORE-0007 §12.2.1.  Each is `void`-returning on the
            // BRC client side; if the broker side accidentally emits
            // a reply, the BRC's unmatched_replies counter increments.
            //
            // HEARTBEAT_NOTIFY — per-presence liveness.
            bh.brc.send_heartbeat(channel, uid, "producer",
                                   nlohmann::json::object());

            // CHECKSUM_ERROR_REPORT — Cat-2 broker telemetry.
            nlohmann::json csum_report;
            csum_report["channel_name"]   = channel;
            csum_report["role_uid"]       = uid;
            csum_report["slot_index"]     = 0;
            csum_report["expected_hash"]  = "deadbeef";
            csum_report["observed_hash"]  = "cafebabe";
            bh.brc.send_checksum_error(csum_report);

            // CHANNEL_BROADCAST_SEND_NOTIFY — fan-out to channel members.
            bh.brc.send_broadcast(channel, uid, "test-broadcast",
                                   "test-data");

            // BAND_BROADCAST_SEND_NOTIFY — fan-out to band members.
            // Join a band first so the broadcast has a valid target.
            const std::string band = "!shape.test";
            auto bj = bh.brc.band_join(band);
            ASSERT_TRUE(bj.has_value());
            nlohmann::json body;
            body["msg"] = "test-band-broadcast";
            bh.brc.send_broadcast(band, uid, "BAND_BROADCAST_SEND_NOTIFY",
                                   body.dump());

            // Give the broker time to (incorrectly) emit any replies.
            // Without this, the receive loop might not yet have observed
            // a reply that arrives milliseconds after our calls return.
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            // The contract: NONE of the four fire-and-forget REQs above
            // should have produced a wire reply.  If the broker added an
            // ACK for any of them (HEP-0007 §12.2.1 violation), this
            // counter will be non-zero.
            const size_t final_count = bh.brc.unmatched_replies();
            EXPECT_EQ(final_count, baseline)
                << "After exercising " << 4 << " fire-and-forget REQs "
                << "(HEARTBEAT, CHECKSUM_ERROR_REPORT, CHANNEL_BROADCAST, "
                << "BAND_BROADCAST), the BRC observed "
                << (final_count - baseline) << " unmatched reply-shape "
                << "message(s).  This is the runtime fingerprint of a "
                << "HEP-CORE-0007 §12.2.1 violation: the broker is "
                << "emitting an `_ACK` or `ERROR` for a REQ that the "
                << "shape catalog declares fire-and-forget.  Either fix "
                << "the broker to stop sending the reply, or move the "
                << "REQ to the sync list with a sync BRC client method.";

            (void)bh.brc.band_leave(band);
            bh.stop();
        });
}

} // namespace zmq_endpoint_registry
} // namespace pylabhub::tests::worker

// ── Dispatcher registrar ────────────────────────────────────────────────────

namespace
{

struct ZmqEndpointRegistryRegistrar
{
    ZmqEndpointRegistryRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2) return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "zmq_endpoint_registry")
                    return -1;
                std::string sc(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::zmq_endpoint_registry;

                if (sc == "default_transport_is_shm")
                    return default_transport_is_shm();
                if (sc == "zmq_transport_round_trip")
                    return zmq_transport_round_trip();
                if (sc == "multiple_consumers_discover_same_endpoint")
                    return multiple_consumers_discover_same_endpoint();
                if (sc == "shm_and_zmq_coexist")
                    return shm_and_zmq_coexist();
                if (sc == "endpoint_update_reflected_in_discovery")
                    return endpoint_update_reflected_in_discovery();
                if (sc == "endpoint_update_non_producer_returns_error")
                    return endpoint_update_non_producer_returns_error();
                if (sc == "endpoint_update_port_zero_returns_error")
                    return endpoint_update_port_zero_returns_error();
                if (sc == "req_shape_no_unmatched_replies_for_fire_and_forget")
                    return req_shape_no_unmatched_replies_for_fire_and_forget();
                return -1;
            });
    }
} g_registrar;

} // namespace
