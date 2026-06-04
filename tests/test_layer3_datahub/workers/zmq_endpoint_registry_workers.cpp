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

// ─── Stub-only BRC handle (wire-level fixture, NOT broker) ──────────────────
// PURPOSE: BRC timeout-conformance tests against `SilentRouterStub`,
//   a bare ZMQ ROUTER socket on plain TCP that records what BRC
//   sends but emits no replies.  Used ONLY by
//   `req_shape_sync_req_times_out_on_no_reply` to drive BRC's
//   `do_request` into its deterministic timeout branch.
// BYPASS: Plain-TCP connection (no CURVE) — `cfg.broker_pubkey = ""`
//   and no client keypair.  Per HEP-CORE-0035 §4.6.5 §"no-bypass"
//   discipline, every test that drives a BROKER uses real CURVE +
//   admission via `pylabhub::tests::BrcHandle`.  The stub here is
//   NOT a broker — it's a wire-level fixture that doesn't speak
//   CURVE because the test is about REQ-timeout behaviour, which
//   doesn't depend on CURVE.  Per `feedback_no_mocks_via_
//   observability`, a wire-level stub that records what BRC sends
//   is a fixture, not a mock of broker logic.
// CANONICAL STORAGE: BRC connection state lives in
//   `BrokerRequestComm::pImpl`; the matching the harness type is
//   `pylabhub::tests::BrcHandle` (CURVE).
// RE-EXAMINE WHEN: BRC's connection grammar changes such that
//   plain-TCP connect is no longer accepted at all (no test path
//   could exist).  Or if §4.6.5 is amended to forbid wire-level
//   stubs entirely.
struct StubBrcHandle
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

    ~StubBrcHandle()
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

/// Run a worker body with a freshly spun-up HubHostBrokerHandle under
/// real CURVE + admission (HEP-CORE-0035 §2 + §4.6.5).  Body receives
/// (broker, curve).
template <typename Body>
int run_with_host(std::string_view worker_name,
                  std::vector<std::string> role_uids,
                  Body &&body)
{
    return run_gtest_worker(
        [role_uids = std::move(role_uids),
         body = std::forward<Body>(body)]() mutable {
            auto curve = pylabhub::tests::make_curve_setup(role_uids);
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
        pylabhub::crypto::GetLifecycleModule(),
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
            bh.start(broker.endpoint, broker.pubkey, uid, curve.role(uid));
            auto reg = bh.brc.register_channel(make_reg_opts(channel, uid),
                                                 3000);
            ASSERT_TRUE(reg.has_value());

            bh.brc.send_heartbeat(channel, uid, "producer", {});

            pylabhub::tests::BrcHandle cons_bh;
            cons_bh.start(broker.endpoint, broker.pubkey, cons_uid,
                          curve.role(cons_uid));
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
            bh.start(broker.endpoint, broker.pubkey, uid, curve.role(uid));

            auto opts                 = make_reg_opts(channel, uid);
            opts["data_transport"]    = "zmq";
            opts["zmq_node_endpoint"] = zmq_ep;
            auto reg = bh.brc.register_channel(opts, 3000);
            ASSERT_TRUE(reg.has_value());

            bh.brc.send_heartbeat(channel, uid, "producer", {});

            pylabhub::tests::BrcHandle cons_bh;
            cons_bh.start(broker.endpoint, broker.pubkey, cons_uid,
                          curve.role(cons_uid));
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
            bh.start(broker.endpoint, broker.pubkey, uid, curve.role(uid));

            auto opts                 = make_reg_opts(channel, uid);
            opts["data_transport"]    = "zmq";
            opts["zmq_node_endpoint"] = zmq_ep;
            auto reg = bh.brc.register_channel(opts, 3000);
            ASSERT_TRUE(reg.has_value());

            bh.brc.send_heartbeat(channel, uid, "producer", {});

            pylabhub::tests::BrcHandle c1, c2;
            c1.start(broker.endpoint, broker.pubkey, c1_uid,
                     curve.role(c1_uid));
            c2.start(broker.endpoint, broker.pubkey, c2_uid,
                     curve.role(c2_uid));

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
                         curve.role(shm_uid));
            auto shm_reg = shm_bh.brc.register_channel(
                make_reg_opts(shm_ch, shm_uid), 3000);
            ASSERT_TRUE(shm_reg.has_value());

            pylabhub::tests::BrcHandle zmq_bh;
            zmq_bh.start(broker.endpoint, broker.pubkey, zmq_uid,
                         curve.role(zmq_uid));
            auto zmq_opts                 = make_reg_opts(zmq_ch, zmq_uid);
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
            bh.start(broker.endpoint, broker.pubkey, uid, curve.role(uid));

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

            pylabhub::tests::BrcHandle cons_bh;
            cons_bh.start(broker.endpoint, broker.pubkey, cons_uid,
                          curve.role(cons_uid));
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
                          curve.role(prod_uid));
            auto opts                 = make_reg_opts(channel, prod_uid);
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
                           curve.role(other_uid));
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
            bh.start(broker.endpoint, broker.pubkey, uid, curve.role(uid));

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

// ── HEP-CORE-0007 §12.2.1 TIMEOUT-path conformance (Option A minimal) ──
//
// "Silent stub" — a bare ZMQ ROUTER socket that accepts BRC's plain-TCP
// DEALER connection, receives REQs, and emits NO reply.  Drives BRC's
// `do_request` into its timeout branch deterministically (no need to
// kill the broker mid-call or use a flaky tiny-timeout heuristic
// against the real broker).
//
// Per `feedback_no_mocks_via_observability.md`: this is a wire-level
// test fixture (records what BRC sends; replies per a policy), NOT a
// mock of broker logic.  No production behaviour is duplicated.
//
// HEP contract under test:
//   - Sync REQ methods (per HEP-CORE-0007 §12.2.1 catalog) MUST return
//     `nullopt` when no reply arrives within `timeout_ms`.
//   - The wait MUST be approximately `timeout_ms` (not zero — would
//     mean the method skipped the wait; not >>budget — would mean the
//     method blocked past its declared deadline).

class SilentRouterStub
{
  public:
    SilentRouterStub()
        : sock_(pylabhub::hub::get_zmq_context(), zmq::socket_type::router)
    {
        sock_.set(zmq::sockopt::linger, 0);
        sock_.bind("tcp://127.0.0.1:*");
        endpoint_ = sock_.get(zmq::sockopt::last_endpoint);
        thread_ = std::thread([this] { run_(); });
    }

    ~SilentRouterStub()
    {
        stop_.store(true, std::memory_order_release);
        if (thread_.joinable())
            thread_.join();
    }

    const std::string &endpoint() const noexcept { return endpoint_; }
    size_t  received_count() const noexcept
    {
        return received_count_.load(std::memory_order_relaxed);
    }

  private:
    void run_()
    {
        // Drain incoming messages; discard them.  No reply on any.
        while (!stop_.load(std::memory_order_acquire))
        {
            zmq::pollitem_t items[] = {
                {static_cast<void *>(sock_), 0, ZMQ_POLLIN, 0}};
            (void)zmq::poll(items, 1, std::chrono::milliseconds(50));
            if (items[0].revents & ZMQ_POLLIN)
            {
                // Drain the full frame (identity + possible empty +
                // payload).  Don't reply.
                while (true)
                {
                    zmq::message_t msg;
                    auto r = sock_.recv(msg, zmq::recv_flags::dontwait);
                    if (!r.has_value()) break;
                    if (!msg.more()) break;
                }
                received_count_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    zmq::socket_t       sock_;
    std::string         endpoint_;
    std::thread         thread_;
    std::atomic<bool>   stop_{false};
    std::atomic<size_t> received_count_{0};
};

int req_shape_sync_req_times_out_on_no_reply()
{
    return run_gtest_worker(
        [] {
            // Bring up the silent stub on a random local port.
            SilentRouterStub stub;

            // BRC connects with empty pubkey → plain TCP (no CURVE).
            // Uses StubBrcHandle — the declared bypass for wire-level
            // fixtures (see fixture block at top of file).
            StubBrcHandle bh;
            bh.start(stub.endpoint(), /*pubkey=*/"",
                     /*uid=*/"req-shape-timeout-test");

            // Probe sync REQs from across the §12.2.1 catalog.  Each
            // MUST return nullopt after waiting ~kTimeoutMs.  Sample
            // covers a state-mutator (REG), a state-query (CHANNEL_LIST),
            // and the bug-of-the-session (ENDPOINT_UPDATE) — gives us
            // coverage across the do_request shapes.
            constexpr int kTimeoutMs       = 200;
            constexpr int kMinElapsedMs    = 150;  // didn't skip the wait
            constexpr int kMaxElapsedMs    = 500;  // didn't block past it

            using clk = std::chrono::steady_clock;
            using ms  = std::chrono::milliseconds;

            auto time_call = [](auto &&fn) {
                auto t0 = clk::now();
                auto r  = fn();
                auto el = std::chrono::duration_cast<ms>(clk::now() - t0).count();
                return std::make_pair(r, el);
            };

            // 1. REG_REQ — state mutation, do_request("REG_REQ", "REG_ACK").
            {
                auto opts = make_reg_opts(pid_chan("timeout.reg"),
                                          "prod.timeout-test");
                auto [reg, el] = time_call(
                    [&] { return bh.brc.register_channel(opts, kTimeoutMs); });
                EXPECT_FALSE(reg.has_value())
                    << "REG_REQ must return nullopt on no-reply, got: "
                    << (reg ? reg->dump() : "<nullopt>");
                EXPECT_GE(el, kMinElapsedMs)
                    << "REG_REQ returned too fast (" << el << "ms) — "
                    << "did it skip the wait?";
                EXPECT_LE(el, kMaxElapsedMs)
                    << "REG_REQ blocked past timeout (" << el << "ms > "
                    << kMaxElapsedMs << "ms budget)";
            }

            // 2. CHANNEL_LIST_REQ — pure query, do_request("CHANNEL_LIST_REQ",
            //    "CHANNEL_LIST_ACK").
            {
                auto [list, el] = time_call(
                    [&] { return bh.brc.list_channels(kTimeoutMs); });
                EXPECT_FALSE(list.has_value())
                    << "CHANNEL_LIST_REQ must return nullopt on no-reply";
                EXPECT_GE(el, kMinElapsedMs);
                EXPECT_LE(el, kMaxElapsedMs);
            }

            // 3. ENDPOINT_UPDATE_REQ — the bug-of-the-session.  Verifies
            //    the just-shipped sync conversion (8228f1ac) waits + times
            //    out correctly under the same fixture.
            {
                auto [upd, el] = time_call([&] {
                    return bh.brc.send_endpoint_update(
                        "timeout.ep", "zmq_node",
                        "tcp://127.0.0.1:44321", kTimeoutMs);
                });
                EXPECT_FALSE(upd.has_value())
                    << "ENDPOINT_UPDATE_REQ must return nullopt on no-reply";
                EXPECT_GE(el, kMinElapsedMs);
                EXPECT_LE(el, kMaxElapsedMs);
            }

            // Sanity: stub did receive at least one frame (BRC isn't
            // failing at the socket layer — it's reaching the wire,
            // just timing out as designed).
            EXPECT_GT(stub.received_count(), 0u)
                << "Stub received zero frames — BRC may not have connected";

            bh.stop();
        },
        "zmq_endpoint_registry::req_shape_sync_req_times_out_on_no_reply",
        Logger::GetLifecycleModule(),
        FileLock::GetLifecycleModule(),
        JsonConfig::GetLifecycleModule(),
        pylabhub::crypto::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

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
            bh.start(broker.endpoint, broker.pubkey, uid, curve.role(uid));

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
                if (sc == "req_shape_sync_req_times_out_on_no_reply")
                    return req_shape_sync_req_times_out_on_no_reply();
                return -1;
            });
    }
} g_registrar;

} // namespace
