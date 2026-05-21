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

#include "plh_datahub.hpp"
#include "plh_service.hpp"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"
#include "utils/broker_request_comm.hpp"
#include "utils/broker_service.hpp"
#include "utils/config/hub_config.hpp"
#include "utils/file_lock.hpp"
#include "utils/hub_directory.hpp"
#include "utils/hub_host.hpp"
#include "utils/hub_state.hpp"
#include "utils/json_config.hpp"
#include "utils/logger.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
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
using pylabhub::config::HubConfig;
using pylabhub::hub_host::HubHost;
using pylabhub::tests::helper::run_gtest_worker;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace pylabhub::tests::worker
{
namespace zmq_endpoint_registry
{

namespace
{

fs::path make_test_hub_dir()
{
    static std::atomic<int> ctr{0};
    fs::path dir = fs::temp_directory_path() /
                   ("plh_l3_zmqep_" + std::to_string(::getpid()) + "_" +
                    std::to_string(ctr.fetch_add(1)));
    fs::remove_all(dir);
    fs::create_directories(dir);
    pylabhub::utils::HubDirectory::init_directory(dir, "ZmqEpTestHub");

    const fs::path hub_json = dir / "hub.json";
    json j;
    {
        std::ifstream f(hub_json);
        if (f.is_open())
            j = json::parse(f);
    }
    j["network"]["broker_endpoint"] = "tcp://127.0.0.1:0";
    j["admin"]["enabled"]           = false;
    j["script"]["path"]             = "";
    {
        std::ofstream f(hub_json);
        f << j.dump(2);
    }
    return dir;
}

struct BrcHandle
{
    BrokerRequestComm brc;
    std::atomic<bool> running{true};
    std::thread       thread;

    void start(const std::string &ep, const std::string &pk,
               const std::string &uid)
    {
        BrokerRequestComm::Config cfg;
        cfg.broker_endpoint = ep;
        cfg.broker_pubkey   = pk;
        cfg.role_uid        = uid;
        ASSERT_TRUE(brc.connect(cfg));
        thread = std::thread([this] {
            brc.run_poll_loop([this] { return running.load(); });
        });
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

std::string pid_chan(const std::string &base)
{
    return base + ".pid" + std::to_string(::getpid());
}

json make_reg_opts(const std::string &channel, const std::string &role_uid)
{
    json opts;
    opts["channel_name"]      = channel;
    opts["pattern"]           = "PubSub";
    opts["has_shared_memory"] = false;
    opts["producer_pid"]      = ::getpid();
    opts["role_uid"]          = role_uid;
    opts["role_name"]         = "test_producer";
    return opts;
}

/// Run a worker body with a freshly spun-up real HubHost.
///
/// Real-production-wiring: HubConfig::load_from_directory → HubHost
/// (which composes the real BrokerService + HubState + AdminService
/// + ThreadManager-backed broker run-loop).  The body sees a fully
/// assembled production hub via `host.broker_endpoint() / pubkey() /
/// broker()` — same surface broker_schema / broker_admin /
/// broker_consumer use.  No LogCaptureFixture: the original suite did
/// not install one and the wave's coverage discipline is "preserve
/// depth, don't strengthen mid-migration"; a follow-up sweep can add
/// must-fire log expectations where production paths are deterministic.
template <typename Body>
int run_with_host(std::string_view worker_name, Body &&body)
{
    return run_gtest_worker(
        [body = std::forward<Body>(body)]() mutable {
            const fs::path dir = make_test_hub_dir();
            auto host = std::make_unique<HubHost>(
                HubConfig::load_from_directory(dir.string()));
            host->startup();
            ASSERT_TRUE(host->is_running());

            body(*host);

            host.reset();
            std::error_code ec;
            fs::remove_all(dir, ec);
        },
        std::string(worker_name).c_str(),
        Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule(),
        pylabhub::crypto::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

} // namespace

int default_transport_is_shm()
{
    return run_with_host(
        "zmq_endpoint_registry::default_transport_is_shm",
        [](HubHost &host) {
            const std::string channel = pid_chan("zmqvc.default.shm");
            const std::string uid     = "prod.ep." + channel;

            BrcHandle bh;
            bh.start(host.broker_endpoint(), host.broker_pubkey(), uid);
            auto reg = bh.brc.register_channel(make_reg_opts(channel, uid),
                                                 3000);
            ASSERT_TRUE(reg.has_value());

            bh.brc.send_heartbeat(channel, uid, "producer", {});

            BrcHandle cons_bh;
            cons_bh.start(host.broker_endpoint(), host.broker_pubkey(),
                          "cons.ep." + channel);
            auto disc = cons_bh.brc.discover_channel(channel, {}, 3000);
            ASSERT_TRUE(disc.has_value());
            EXPECT_EQ(disc->value("data_transport", ""), "shm");

            cons_bh.stop();
            bh.stop();
        });
}

int zmq_transport_round_trip()
{
    return run_with_host(
        "zmq_endpoint_registry::zmq_transport_round_trip",
        [](HubHost &host) {
            const std::string channel = pid_chan("zmqvc.zmq.roundtrip");
            const std::string uid     = "prod.ep." + channel;
            const std::string zmq_ep  = "tcp://127.0.0.1:55555";

            BrcHandle bh;
            bh.start(host.broker_endpoint(), host.broker_pubkey(), uid);

            auto opts                 = make_reg_opts(channel, uid);
            opts["data_transport"]    = "zmq";
            opts["zmq_node_endpoint"] = zmq_ep;
            auto reg = bh.brc.register_channel(opts, 3000);
            ASSERT_TRUE(reg.has_value());

            bh.brc.send_heartbeat(channel, uid, "producer", {});

            BrcHandle cons_bh;
            cons_bh.start(host.broker_endpoint(), host.broker_pubkey(),
                          "cons.ep." + channel);
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
    return run_with_host(
        "zmq_endpoint_registry::multiple_consumers_discover_same_endpoint",
        [](HubHost &host) {
            const std::string channel = pid_chan("zmqvc.multi.disc");
            const std::string uid     = "prod.ep." + channel;
            const std::string zmq_ep  = "tcp://127.0.0.1:55556";

            BrcHandle bh;
            bh.start(host.broker_endpoint(), host.broker_pubkey(), uid);

            auto opts                 = make_reg_opts(channel, uid);
            opts["data_transport"]    = "zmq";
            opts["zmq_node_endpoint"] = zmq_ep;
            auto reg = bh.brc.register_channel(opts, 3000);
            ASSERT_TRUE(reg.has_value());

            bh.brc.send_heartbeat(channel, uid, "producer", {});

            BrcHandle c1, c2;
            c1.start(host.broker_endpoint(), host.broker_pubkey(),
                     "CONS1-" + channel);
            c2.start(host.broker_endpoint(), host.broker_pubkey(),
                     "CONS2-" + channel);

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
    return run_with_host(
        "zmq_endpoint_registry::shm_and_zmq_coexist",
        [](HubHost &host) {
            const std::string shm_ch = pid_chan("zmqvc.coexist.shm");
            const std::string zmq_ch = pid_chan("zmqvc.coexist.zmq");

            BrcHandle shm_bh;
            shm_bh.start(host.broker_endpoint(), host.broker_pubkey(),
                         "prod.shm." + shm_ch);
            auto shm_reg = shm_bh.brc.register_channel(
                make_reg_opts(shm_ch, "prod.shm." + shm_ch), 3000);
            ASSERT_TRUE(shm_reg.has_value());

            BrcHandle zmq_bh;
            zmq_bh.start(host.broker_endpoint(), host.broker_pubkey(),
                         "prod." + zmq_ch);
            auto zmq_opts                 = make_reg_opts(zmq_ch, "prod." + zmq_ch);
            zmq_opts["data_transport"]    = "zmq";
            zmq_opts["zmq_node_endpoint"] = "tcp://127.0.0.1:55557";
            auto zmq_reg = zmq_bh.brc.register_channel(zmq_opts, 3000);
            ASSERT_TRUE(zmq_reg.has_value());

            ChannelSnapshot snap = host.broker().query_channel_snapshot();
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
    return run_with_host(
        "zmq_endpoint_registry::endpoint_update_reflected_in_discovery",
        [](HubHost &host) {
            const std::string channel    = pid_chan("zmqvc.ep.update");
            const std::string uid        = "prod.ep." + channel;
            const std::string updated_ep = "tcp://127.0.0.1:44444";

            BrcHandle bh;
            bh.start(host.broker_endpoint(), host.broker_pubkey(), uid);

            auto opts                 = make_reg_opts(channel, uid);
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

            BrcHandle cons_bh;
            cons_bh.start(host.broker_endpoint(), host.broker_pubkey(),
                          "cons.ep." + channel);
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
    return run_with_host(
        "zmq_endpoint_registry::endpoint_update_non_producer_returns_error",
        [](HubHost &host) {
            const std::string channel  = pid_chan("zmqvc.ep.nonprod");
            const std::string prod_uid = "prod.ep." + channel;
            const std::string other_uid = "cons.notprod." + channel;
            const std::string updated_ep = "tcp://127.0.0.1:44455";

            // Producer registers the channel.
            BrcHandle prod_bh;
            prod_bh.start(host.broker_endpoint(), host.broker_pubkey(),
                          prod_uid);
            auto opts                 = make_reg_opts(channel, prod_uid);
            opts["data_transport"]    = "zmq";
            opts["zmq_node_endpoint"] = "tcp://127.0.0.1:0";
            auto reg = prod_bh.brc.register_channel(opts, 3000);
            ASSERT_TRUE(reg.has_value());

            // A DIFFERENT identity tries to update the producer's endpoint.
            // Per HEP-CORE-0021 §16.3 + broker_service.cpp:handle_endpoint_
            // update_req identity-resolution: sender_zmq_identity must match
            // a registered producer's zmq_identity, else NOT_CHANNEL_OWNER.
            BrcHandle other_bh;
            other_bh.start(host.broker_endpoint(), host.broker_pubkey(),
                           other_uid);
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
    return run_with_host(
        "zmq_endpoint_registry::endpoint_update_port_zero_returns_error",
        [](HubHost &host) {
            const std::string channel = pid_chan("zmqvc.ep.zeroport");
            const std::string uid     = "prod.ep." + channel;

            BrcHandle bh;
            bh.start(host.broker_endpoint(), host.broker_pubkey(), uid);

            auto opts                 = make_reg_opts(channel, uid);
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

int req_shape_no_unmatched_replies_for_fire_and_forget()
{
    return run_with_host(
        "zmq_endpoint_registry::req_shape_no_unmatched_replies_for_fire_and_forget",
        [](HubHost &host) {
            const std::string channel = pid_chan("zmqvc.shape");
            const std::string uid     = "prod.ep." + channel;

            BrcHandle bh;
            bh.start(host.broker_endpoint(), host.broker_pubkey(), uid);

            // Register channel — needed so heartbeat / broadcast / band
            // calls have a legitimate target.
            auto opts = make_reg_opts(channel, uid);
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
            // HEARTBEAT_REQ — per-presence liveness.
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

            // CHANNEL_BROADCAST_REQ — fan-out to channel members.
            bh.brc.send_broadcast(channel, uid, "test-broadcast",
                                   "test-data");

            // BAND_BROADCAST_REQ — fan-out to band members.
            // Join a band first so the broadcast has a valid target.
            const std::string band = "!shape.test";
            auto bj = bh.brc.band_join(band);
            ASSERT_TRUE(bj.has_value());
            nlohmann::json body;
            body["msg"] = "test-band-broadcast";
            bh.brc.send_broadcast(band, uid, "BAND_BROADCAST_REQ",
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

            bh.brc.band_leave(band);
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
