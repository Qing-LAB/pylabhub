// tests/test_layer3_datahub/workers/datahub_role_state_workers.cpp
//
// HEP-CORE-0023 §2.5: broker role-liveness state machine workers.
// Exercises Ready/Pending transitions + RoleStateMetrics counters.

#include "test_entrypoint.h"
#include "test_process_utils.h"
#include "shared_test_helpers.h"
#include "test_sync_utils.h"

#include "utils/broker_request_comm.hpp"
#include "utils/broker_service.hpp"
#include "utils/hub_state.hpp"
#include "utils/role_api_base.hpp"
#include "utils/role_handler.hpp"
#include "utils/role_host_core.hpp"
#include "plh_datahub.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>

using namespace pylabhub::tests::helper;
using namespace pylabhub::hub;
using pylabhub::broker::BrokerService;
using pylabhub::broker::RoleStateMetrics;

namespace pylabhub::tests::worker::broker_role_state
{

static auto logger_module() { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto crypto_module() { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto zmq_module()    { return ::pylabhub::hub::GetZMQContextModule(); }

namespace
{

struct BrokerHandle
{
    std::unique_ptr<pylabhub::hub::HubState> hub_state;
    std::unique_ptr<BrokerService> service;
    std::thread                    thread;
    std::string                    endpoint;
    std::string                    pubkey;

    ~BrokerHandle()
    {
        if (thread.joinable())
        {
            if (service) service->stop();
            thread.join();
        }
    }
    BrokerHandle() = default;
    BrokerHandle(BrokerHandle&&) = default;
    BrokerHandle& operator=(BrokerHandle&&) = default;

    void stop_and_join()
    {
        if (service) service->stop();
        if (thread.joinable()) thread.join();
    }
};

BrokerHandle start_broker_with_cfg(BrokerService::Config cfg)
{
    using Ready = std::pair<std::string, std::string>;
    auto promise = std::make_shared<std::promise<Ready>>();
    auto fut     = promise->get_future();
    cfg.on_ready = [promise](const std::string& ep, const std::string& pk) {
        promise->set_value({ep, pk});
    };
    auto state = std::make_unique<pylabhub::hub::HubState>();
    auto svc = std::make_unique<BrokerService>(std::move(cfg), *state);
    auto* raw = svc.get();
    std::thread t([raw]() { raw->run(); });
    auto info = fut.get();
    BrokerHandle h;
    h.hub_state  = std::move(state);
    h.service  = std::move(svc);
    h.thread   = std::move(t);
    h.endpoint = info.first;
    h.pubkey   = info.second;
    return h;
}

struct BrcHandle
{
    BrokerRequestComm brc;
    std::atomic<bool> running{true};
    std::thread       thread;

    void start(const std::string& ep, const std::string& pk, const std::string& uid)
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
        if (thread.joinable()) thread.join();
        brc.disconnect();
    }
    ~BrcHandle() { if (thread.joinable()) stop(); }
};

nlohmann::json make_reg_opts(const std::string& channel, const std::string& role_uid)
{
    nlohmann::json opts;
    opts["channel_name"]      = channel;
    opts["pattern"]           = "PubSub";
    opts["has_shared_memory"] = false;
    opts["producer_pid"]      = ::getpid();
    opts["role_uid"]          = role_uid;
    opts["role_name"]         = "role_state_test";
    return opts;
}

} // anon

// ============================================================================
// metrics_reclaim_cycle — full Ready -> Pending -> dereg, verified via metrics
// ============================================================================

int metrics_reclaim_cycle()
{
    return run_gtest_worker(
        []() {
            BrokerService::Config cfg;
            cfg.endpoint                         = "tcp://127.0.0.1:0";
            cfg.use_curve                        = true;
            cfg.ready_timeout_override           = std::chrono::milliseconds(150);
            cfg.pending_timeout_override         = std::chrono::milliseconds(150);
            cfg.consumer_liveness_check_interval = std::chrono::seconds(0);
            auto broker = start_broker_with_cfg(std::move(cfg));

            const std::string ch  = make_test_channel_name("role_state.metrics_cycle");
            const std::string uid = "prod." + ch;

            BrcHandle bh;
            bh.start(broker.endpoint, broker.pubkey, uid);

            auto reg = bh.brc.register_channel(make_reg_opts(ch, uid), 3000);
            ASSERT_TRUE(reg.has_value());

            // One heartbeat -> PendingReady -> Ready, bumps pending_to_ready.
            bh.brc.send_heartbeat(ch, uid, "producer", {});

            // Stop heartbeating; wait for Ready -> Pending -> dereg via metrics.
            auto metrics_reclaimed = [&]() {
                auto m = broker.service->query_role_state_metrics();
                return m.pending_to_deregistered_total >= 1;
            };
            ASSERT_TRUE(poll_until(metrics_reclaimed, std::chrono::seconds(3)))
                << "pending_to_deregistered_total did not increment within 3s";

            auto m = broker.service->query_role_state_metrics();
            // HEP-CORE-0023 §2.5 — pending_to_ready counts genuine
            // Pending→Connected recoveries.  First-heartbeat (kRegistering
            // → kLive sub-state flip) is NOT a Pending→Connected transition
            // and does not bump this counter.
            EXPECT_EQ(m.pending_to_ready_total, 0u)
                << "no recovery happened in this scenario";
            EXPECT_GE(m.ready_to_pending_total, 1u)
                << "ready_to_pending_total should be >=1 (demotion on timeout)";
            EXPECT_GE(m.pending_to_deregistered_total, 1u)
                << "pending_to_deregistered_total should be >=1 (pending timeout)";

            bh.stop();
            broker.stop_and_join();
        },
        "role_state.metrics_reclaim_cycle",
        logger_module(), crypto_module(), zmq_module());
}

// ============================================================================
// pending_recovers_to_ready — demote, then heartbeat restores Ready
// ============================================================================

int pending_recovers_to_ready()
{
    return run_gtest_worker(
        []() {
            BrokerService::Config cfg;
            cfg.endpoint                         = "tcp://127.0.0.1:0";
            cfg.use_curve                        = true;
            cfg.ready_timeout_override           = std::chrono::milliseconds(150);
            cfg.pending_timeout_override         = std::chrono::seconds(10); // long -> won't dereg
            cfg.consumer_liveness_check_interval = std::chrono::seconds(0);
            auto broker = start_broker_with_cfg(std::move(cfg));

            const std::string ch  = make_test_channel_name("role_state.recover");
            const std::string uid = "prod." + ch;

            BrcHandle bh;
            bh.start(broker.endpoint, broker.pubkey, uid);

            auto reg = bh.brc.register_channel(make_reg_opts(ch, uid), 3000);
            ASSERT_TRUE(reg.has_value());
            bh.brc.send_heartbeat(ch, uid, "producer", {});  // -> Ready

            // Wait for demotion to Pending.
            auto demoted = [&]() {
                auto m = broker.service->query_role_state_metrics();
                return m.ready_to_pending_total >= 1;
            };
            ASSERT_TRUE(poll_until(demoted, std::chrono::seconds(2)))
                << "Ready -> Pending demotion did not fire";

            // Heartbeat again -> should transition Pending -> Ready (counter +1).
            auto before = broker.service->query_role_state_metrics().pending_to_ready_total;
            bh.brc.send_heartbeat(ch, uid, "producer", {});
            auto recovered = [&]() {
                auto m = broker.service->query_role_state_metrics();
                return m.pending_to_ready_total > before;
            };
            ASSERT_TRUE(poll_until(recovered, std::chrono::seconds(1)))
                << "pending_to_ready_total did not increment on heartbeat";

            auto m = broker.service->query_role_state_metrics();
            EXPECT_EQ(m.pending_to_deregistered_total, 0u)
                << "Should not have deregistered — pending_timeout was long";

            bh.stop();
            broker.stop_and_join();
        },
        "role_state.pending_recovers_to_ready",
        logger_module(), crypto_module(), zmq_module());
}

// ============================================================================
// stuck_in_pending_reclaimed — role never heartbeats, pending_timeout fires
// ============================================================================

int stuck_in_pending_reclaimed()
{
    return run_gtest_worker(
        []() {
            BrokerService::Config cfg;
            cfg.endpoint                         = "tcp://127.0.0.1:0";
            cfg.use_curve                        = true;
            // Per HEP-CORE-0023 §2.1 the registered-but-never-heartbeat
            // case is `Connected` with `first_heartbeat_seen=false`
            // (sub-state "registering").  Reclamation goes through both
            // sweep passes: Connected → Pending after `ready_timeout`,
            // then Pending → Disconnected after `pending_timeout`.  So
            // both timeouts apply here — keep them short so the total
            // (~150 ms) fits in a 2 s poll window.
            cfg.ready_timeout_override           = std::chrono::milliseconds(50);
            cfg.pending_timeout_override         = std::chrono::milliseconds(100);
            cfg.consumer_liveness_check_interval = std::chrono::seconds(0);
            auto broker = start_broker_with_cfg(std::move(cfg));

            const std::string ch  = make_test_channel_name("role_state.stuck_pending");
            const std::string uid = "prod." + ch;

            BrcHandle bh;
            bh.start(broker.endpoint, broker.pubkey, uid);

            auto reg = bh.brc.register_channel(make_reg_opts(ch, uid), 3000);
            ASSERT_TRUE(reg.has_value());
            // Deliberately NO heartbeat sent — presence is registered but
            // remains in the "registering" sub-state until ready_timeout
            // demotes it.

            auto reclaimed = [&]() {
                auto m = broker.service->query_role_state_metrics();
                return m.pending_to_deregistered_total >= 1;
            };
            ASSERT_TRUE(poll_until(reclaimed, std::chrono::seconds(2)))
                << "Registered-no-heartbeat role was not reclaimed within 2s";

            auto m = broker.service->query_role_state_metrics();
            EXPECT_EQ(m.pending_to_ready_total, 0u)
                << "Should never have transitioned to Ready (no heartbeat sent)";
            EXPECT_GE(m.ready_to_pending_total, 1u)
                << "Connected -> Pending demotion should have fired (no "
                   "heartbeats within ready_timeout)";

            bh.stop();
            broker.stop_and_join();
        },
        "role_state.stuck_in_pending_reclaimed",
        logger_module(), crypto_module(), zmq_module());
}

// ============================================================================
// band_membership_cleaned_on_role_close — on_channel_closed hook removes role
// ============================================================================

int band_membership_cleaned_on_role_close()
{
    return run_gtest_worker(
        []() {
            BrokerService::Config cfg;
            cfg.endpoint                         = "tcp://127.0.0.1:0";
            cfg.use_curve                        = true;
            cfg.consumer_liveness_check_interval = std::chrono::seconds(0);
            auto broker = start_broker_with_cfg(std::move(cfg));

            const std::string ch_a    = make_test_channel_name("role_state.band_a");
            const std::string ch_b    = make_test_channel_name("role_state.band_b");
            const std::string uid_a   = "prod.band.a.uid00000001";
            const std::string uid_b   = "prod.band.b.uid00000002";
            const std::string band    = "!" + make_test_channel_name("test.band");

            BrcHandle a, b;
            a.start(broker.endpoint, broker.pubkey, uid_a);
            b.start(broker.endpoint, broker.pubkey, uid_b);

            ASSERT_TRUE(a.brc.register_channel(make_reg_opts(ch_a, uid_a), 3000).has_value());
            ASSERT_TRUE(b.brc.register_channel(make_reg_opts(ch_b, uid_b), 3000).has_value());
            a.brc.send_heartbeat(ch_a, uid_a, "producer", {});
            b.brc.send_heartbeat(ch_b, uid_b, "producer", {});

            ASSERT_TRUE(a.brc.band_join(band, 3000).has_value());
            ASSERT_TRUE(b.brc.band_join(band, 3000).has_value());

            auto members1 = b.brc.band_members(band, 3000);
            ASSERT_TRUE(members1.has_value());
            ASSERT_TRUE(members1->contains("members"));
            EXPECT_EQ((*members1)["members"].size(), 2u) << "Expected 2 members after join";

            // Voluntarily deregister A's channel — on_channel_closed fires,
            // cleanup hook removes A from the band.  Post-Bucket-C: assert
            // status="success" explicitly.
            {
                auto dereg = a.brc.deregister_channel(ch_a);
                ASSERT_TRUE(dereg.has_value());
                EXPECT_EQ(dereg->value("status", std::string{}), "success");
            }

            auto only_b_left = [&]() {
                auto m = b.brc.band_members(band, 1000);
                return m.has_value() && m->contains("members") &&
                       (*m)["members"].size() == 1;
            };
            ASSERT_TRUE(poll_until(only_b_left, std::chrono::seconds(2)))
                << "Band membership was not cleaned up after producer dereg";

            auto members2 = b.brc.band_members(band, 3000);
            ASSERT_TRUE(members2.has_value());
            ASSERT_TRUE(members2->contains("members"));
            bool has_b = false;
            for (const auto& m : (*members2)["members"])
                if (m.value("role_uid", "") == uid_b) has_b = true;
            EXPECT_TRUE(has_b) << "Remaining member should be uid_b";

            a.stop();
            b.stop();
            broker.stop_and_join();
        },
        "role_state.band_membership_cleaned_on_role_close",
        logger_module(), crypto_module(), zmq_module());
}

// ============================================================================
// role_entry_terminal_cleanup_on_last_presence_dereg
//   Wave M3 step 5b (2026-05-11): a producer DEREG that empties the
//   channel must trigger `_dispatch_role_disconnected_if_dead` →
//   `_set_role_disconnected`, erasing the RoleEntry.  Without the
//   wiring, the entry lingers with all-Disconnected presences (the
//   "stale residue" failure mode from Wave M2.5 §6.2).
// ============================================================================

int role_entry_terminal_cleanup_on_last_presence_dereg()
{
    return run_gtest_worker(
        []() {
            BrokerService::Config cfg;
            cfg.endpoint                         = "tcp://127.0.0.1:0";
            cfg.use_curve                        = true;
            cfg.consumer_liveness_check_interval = std::chrono::seconds(0);
            auto broker = start_broker_with_cfg(std::move(cfg));

            const std::string ch  = make_test_channel_name("role_state.cleanup_prod");
            const std::string uid = "prod." + ch;

            BrcHandle bh;
            bh.start(broker.endpoint, broker.pubkey, uid);

            auto reg = bh.brc.register_channel(make_reg_opts(ch, uid), 3000);
            ASSERT_TRUE(reg.has_value());

            // Pre-condition: role entry exists post-REG.
            ASSERT_TRUE(broker.hub_state->role(uid).has_value())
                << "Role entry must exist after REG_REQ";

            // Voluntary DEREG — last-producer path inside
            // `_on_producer_dropped` falls through to
            // `_on_channel_closed`, which marks the producer-presence
            // Disconnected and dispatches terminal cleanup.
            auto dereg = bh.brc.deregister_channel(ch);
            ASSERT_TRUE(dereg.has_value());
            ASSERT_EQ(dereg->value("status", std::string{}), "success");

            // Post-condition: role entry must be GONE.  Broker handles
            // dispatch on its IO thread; poll up to 2 s.
            auto entry_gone = [&]() {
                return !broker.hub_state->role(uid).has_value();
            };
            ASSERT_TRUE(poll_until(entry_gone, std::chrono::seconds(2)))
                << "Role entry was not erased after last producer DEREG "
                   "(H1 wiring gap — _dispatch_role_disconnected_if_dead "
                   "did not fire).";

            bh.stop();
            broker.stop_and_join();
        },
        "role_state.role_entry_terminal_cleanup_on_last_presence_dereg",
        logger_module(), crypto_module(), zmq_module());
}

// ============================================================================
// role_entry_terminal_cleanup_on_consumer_left_last
//   Wave M3 step 5b (2026-05-11): a consumer DEREG whose role had
//   only that one presence must erase the role entry via
//   `_on_consumer_left` → `_dispatch_role_disconnected_if_dead`.
// ============================================================================

int role_entry_terminal_cleanup_on_consumer_left_last()
{
    return run_gtest_worker(
        []() {
            BrokerService::Config cfg;
            cfg.endpoint                         = "tcp://127.0.0.1:0";
            cfg.use_curve                        = true;
            cfg.consumer_liveness_check_interval = std::chrono::seconds(0);
            auto broker = start_broker_with_cfg(std::move(cfg));

            const std::string ch         = make_test_channel_name("role_state.cleanup_cons");
            const std::string prod_uid   = "prod." + ch;
            const std::string cons_uid   = "cons." + ch;

            // Producer creates the channel; we only test the
            // consumer-side cleanup, so prod_uid stays alive.
            BrcHandle pb;
            pb.start(broker.endpoint, broker.pubkey, prod_uid);
            ASSERT_TRUE(pb.brc.register_channel(make_reg_opts(ch, prod_uid), 3000)
                            .has_value());

            // Consumer registers on the same channel with its own uid.
            BrcHandle cb;
            cb.start(broker.endpoint, broker.pubkey, cons_uid);

            nlohmann::json cons_opts;
            cons_opts["channel_name"]  = ch;
            cons_opts["consumer_uid"]  = cons_uid;
            cons_opts["consumer_name"] = "role_state_test_consumer";
            cons_opts["consumer_pid"]  = ::getpid();
            auto cons_reg = cb.brc.register_consumer(cons_opts, 3000);
            ASSERT_TRUE(cons_reg.has_value())
                << "CONSUMER_REG_REQ failed: " <<
                   (cons_reg.has_value() ? "(none)" : "no response");

            ASSERT_TRUE(broker.hub_state->role(cons_uid).has_value())
                << "Consumer role entry must exist after CONSUMER_REG_REQ";

            // Voluntary CONSUMER_DEREG_REQ → `_on_consumer_left` →
            // dispatch.  Last-and-only presence on this role → erase.
            auto dereg = cb.brc.deregister_consumer(ch, 3000);
            ASSERT_TRUE(dereg.has_value());
            ASSERT_EQ(dereg->value("status", std::string{}), "success");

            auto entry_gone = [&]() {
                return !broker.hub_state->role(cons_uid).has_value();
            };
            ASSERT_TRUE(poll_until(entry_gone, std::chrono::seconds(2)))
                << "Consumer role entry was not erased after "
                   "CONSUMER_DEREG_REQ (H1 wiring gap).";

            // Producer side must be untouched (still alive).
            EXPECT_TRUE(broker.hub_state->role(prod_uid).has_value())
                << "Producer role entry must be unaffected by consumer "
                   "DEREG on a separate uid.";

            cb.stop();
            pb.stop();
            broker.stop_and_join();
        },
        "role_state.role_entry_terminal_cleanup_on_consumer_left_last",
        logger_module(), crypto_module(), zmq_module());
}

// ============================================================================
// consumer_heartbeat_timeout_fires_consumer_died_notify
//   Wave-B M2 (3/3): a consumer presence that stops heartbeating must
//   transition Connected → Pending → Disconnected on the broker side,
//   triggering `CONSUMER_DIED_NOTIFY` with `reason="heartbeat_timeout"`
//   to every producer on the channel.  Symmetric with the PID-death
//   path (`check_dead_consumers`, `reason="process_dead"`).  The
//   channel itself must NOT close (HEP-CORE-0023 §2.1.1).
// ============================================================================

int consumer_heartbeat_timeout_fires_consumer_died_notify()
{
    return run_gtest_worker(
        []() {
            BrokerService::Config cfg;
            cfg.endpoint                         = "tcp://127.0.0.1:0";
            cfg.use_curve                        = true;
            // Match the other state-machine tests: 150 ms each side
            // gives ~300 ms minimum + ~100 ms broker sweep cadence.
            // Poll window below is 3 s — generous against CI jitter.
            cfg.ready_timeout_override           = std::chrono::milliseconds(150);
            cfg.pending_timeout_override         = std::chrono::milliseconds(150);
            // Disable the PID-death sweep to keep the notification path
            // unambiguous — only the heartbeat-timeout path can fire.
            cfg.consumer_liveness_check_interval = std::chrono::seconds(0);
            auto broker = start_broker_with_cfg(std::move(cfg));

            const std::string ch       = make_test_channel_name("role_state.cons_hb_timeout");
            const std::string prod_uid = "prod." + ch;
            const std::string cons_uid = "cons." + ch;

            // ── Producer BRC: needs a notification callback BEFORE the
            //    poll loop starts, so we build it inline rather than via
            //    BrcHandle (which doesn't expose the callback hook).
            std::mutex                  notify_mu;
            std::vector<nlohmann::json> consumer_died_notifies;

            BrokerRequestComm prod_brc;
            prod_brc.on_notification(
                [&](const std::string &msg_type, const nlohmann::json &body) {
                    if (msg_type == "CONSUMER_DIED_NOTIFY")
                    {
                        std::lock_guard<std::mutex> lk(notify_mu);
                        consumer_died_notifies.push_back(body);
                    }
                });

            BrokerRequestComm::Config prod_cfg;
            prod_cfg.broker_endpoint = broker.endpoint;
            prod_cfg.broker_pubkey   = broker.pubkey;
            prod_cfg.role_uid        = prod_uid;
            ASSERT_TRUE(prod_brc.connect(prod_cfg));

            std::atomic<bool> prod_poll_running{true};
            std::thread       prod_poll_thread([&]() {
                prod_brc.run_poll_loop([&]() { return prod_poll_running.load(); });
            });

            ASSERT_TRUE(prod_brc.register_channel(make_reg_opts(ch, prod_uid), 3000)
                            .has_value());
            prod_brc.send_heartbeat(ch, prod_uid, "producer", {});

            // ── Consumer BRC: standard BrcHandle.
            BrcHandle cons_bh;
            cons_bh.start(broker.endpoint, broker.pubkey, cons_uid);

            nlohmann::json cons_opts;
            cons_opts["channel_name"]  = ch;
            cons_opts["consumer_uid"]  = cons_uid;
            cons_opts["consumer_name"] = "role_state_test_consumer";
            cons_opts["consumer_pid"]  = ::getpid();
            ASSERT_TRUE(cons_bh.brc.register_consumer(cons_opts, 3000).has_value());
            cons_bh.brc.send_heartbeat(ch, cons_uid, "consumer", {});

            // Keep the producer's own presence alive throughout the
            // poll window — without this, the producer would itself
            // be reclaimed (ready_timeout fires after 150 ms) before
            // we observe the consumer's CONSUMER_DIED_NOTIFY arrival.
            std::atomic<bool> prod_hb_running{true};
            std::thread       prod_hb_thread([&]() {
                while (prod_hb_running.load())
                {
                    prod_brc.send_heartbeat(ch, prod_uid, "producer", {});
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            });

            // From this point the consumer stops heartbeating.  After
            // ready_timeout + pending_timeout (≈300 ms + broker sweep
            // cadence), broker reclaims the consumer-presence and
            // emits CONSUMER_DIED_NOTIFY to the producer.
            auto notify_arrived = [&]() {
                std::lock_guard<std::mutex> lk(notify_mu);
                return !consumer_died_notifies.empty();
            };
            ASSERT_TRUE(poll_until(notify_arrived, std::chrono::seconds(3)))
                << "CONSUMER_DIED_NOTIFY did not arrive at producer within 3 s "
                   "after consumer's heartbeat timeout (Wave-B M2 3/3 gap).";

            // Stop the producer's heartbeat thread before asserting on
            // the captured notification body.
            prod_hb_running.store(false);
            if (prod_hb_thread.joinable()) prod_hb_thread.join();

            // Body shape: reason="heartbeat_timeout" distinguishes this
            // path from the PID-death path (reason="process_dead").  The
            // consumer_uid field disambiguates which consumer (across
            // restarts on the same pid) died — producers cannot reliably
            // correlate notifications on pid alone.
            {
                std::lock_guard<std::mutex> lk(notify_mu);
                ASSERT_FALSE(consumer_died_notifies.empty());
                const auto &body = consumer_died_notifies.front();
                EXPECT_EQ(body.value("channel_name", std::string{}), ch);
                EXPECT_EQ(body.value("reason", std::string{}), "heartbeat_timeout")
                    << "CONSUMER_DIED_NOTIFY must carry reason='heartbeat_timeout' "
                       "for the broker-sweep path; reason='process_dead' is "
                       "reserved for PID-death (check_dead_consumers).";
                EXPECT_EQ(body.value("consumer_uid", std::string{}), cons_uid)
                    << "CONSUMER_DIED_NOTIFY must carry consumer_uid so "
                       "producers can disambiguate which consumer died "
                       "(pid alone is not unique across role restarts).";
                EXPECT_EQ(body.value("consumer_pid", uint64_t{0}),
                          static_cast<uint64_t>(::getpid()));
            }

            // Channel survives — consumer-presence disconnect MUST NOT
            // close the channel (HEP-CORE-0023 §2.1.1).
            EXPECT_TRUE(broker.hub_state->channel(ch).has_value())
                << "Channel must survive a consumer-only heartbeat timeout — "
                   "channel teardown is producer-side only.";

            // Producer still alive (its own presence kept fresh).
            EXPECT_TRUE(broker.hub_state->role(prod_uid).has_value());

            // Consumer role entry erased — last alive presence
            // transitioned Disconnected, so the role-disconnect cascade
            // fired (`_dispatch_role_disconnected_if_dead`).
            auto cons_entry_gone = [&]() {
                return !broker.hub_state->role(cons_uid).has_value();
            };
            EXPECT_TRUE(poll_until(cons_entry_gone, std::chrono::seconds(1)))
                << "Consumer role entry must be erased after its last "
                   "presence transitions Disconnected (H1 wiring cascade).";

            // Counter bumped — pending_to_deregistered_total covers
            // both producer and consumer per-presence transitions.
            auto m = broker.service->query_role_state_metrics();
            EXPECT_GE(m.pending_to_deregistered_total, 1u)
                << "pending_to_deregistered_total must bump on the "
                   "consumer-presence Pending→Disconnected path.";

            // Tear down.
            cons_bh.stop();
            prod_poll_running.store(false);
            prod_brc.stop();
            if (prod_poll_thread.joinable()) prod_poll_thread.join();
            prod_brc.disconnect();
            broker.stop_and_join();
        },
        "role_state.consumer_heartbeat_timeout_fires_consumer_died_notify",
        logger_module(), crypto_module(), zmq_module());
}

// ============================================================================
// role_handler_connections_start_stop_smoke
//   Wave-B M4a — state-only verification of RoleHandler's network
//   surface.  Allocates BRCs + connects them; verifies state; releases.
//   NO threads spawned by the handler (handler is a state holder; the
//   role host owns thread spawning, which lands in M4c).
// ============================================================================

int role_handler_connections_start_stop_smoke()
{
    return run_gtest_worker(
        []() {
            BrokerService::Config bcfg;
            bcfg.endpoint  = "tcp://127.0.0.1:0";
            bcfg.use_curve = false;
            auto broker = start_broker_with_cfg(std::move(bcfg));

            // RoleAPIBase carries the role-side identity (uid, name,
            // auth) — read by RoleHandler::start_connections to build
            // each BRC's Config.  No threads are spawned by either
            // RoleAPIBase or RoleHandler in this test; only the BRC's
            // DEALER socket gets connected.
            pylabhub::scripting::RoleHostCore core;
            pylabhub::scripting::RoleAPIBase  api(
                core, "prod", "prod.handler_smoke.uid00000001");
            api.set_name("handler_smoke");
            api.set_auth("", "");

            pylabhub::config::HubRefConfig hub_cfg;
            hub_cfg.broker        = broker.endpoint;
            hub_cfg.broker_pubkey = broker.pubkey;

            std::vector<pylabhub::scripting::Presence> presences;
            {
                pylabhub::scripting::Presence p;
                p.hub       = hub_cfg;
                p.channel   = make_test_channel_name("role_handler.smoke");
                p.role_kind = pylabhub::scripting::RoleKind::Producer;
                presences.push_back(std::move(p));
            }
            pylabhub::scripting::RoleHandler handler(std::move(presences));

            ASSERT_EQ(handler.presence_count(),   1u);
            ASSERT_EQ(handler.connection_count(), 1u);
            EXPECT_FALSE(handler.connections_started());
            EXPECT_EQ(handler.connections()[0].brc.get(), nullptr);

            ASSERT_TRUE(handler.start_connections(api));
            EXPECT_TRUE(handler.connections_started());
            ASSERT_NE(handler.connections()[0].brc.get(), nullptr);
            EXPECT_TRUE(handler.connections()[0].brc->is_connected());

            handler.stop_connections();
            EXPECT_FALSE(handler.connections_started());
            EXPECT_EQ(handler.connections()[0].brc.get(), nullptr);

            handler.stop_connections();  // idempotent
            EXPECT_FALSE(handler.connections_started());

            broker.stop_and_join();
        },
        "role_state.role_handler_connections_start_stop_smoke",
        logger_module(), crypto_module(), zmq_module());
}

// ============================================================================
// role_handler_connections_dual_hub
//   Wave-B M4a — dual-broker topology pins the M8-payoff state shape
//   at the network layer (state-only): two HubConnections, two BRCs
//   each connected to a different broker.  No thread spawning by the
//   handler; M4c will hook the role host's spawn loop into
//   `handler.connections()`.
// ============================================================================

int role_handler_connections_dual_hub()
{
    return run_gtest_worker(
        []() {
            BrokerService::Config bcfg_a;
            bcfg_a.endpoint  = "tcp://127.0.0.1:0";
            bcfg_a.use_curve = false;
            auto broker_a = start_broker_with_cfg(std::move(bcfg_a));

            BrokerService::Config bcfg_b;
            bcfg_b.endpoint  = "tcp://127.0.0.1:0";
            bcfg_b.use_curve = false;
            auto broker_b = start_broker_with_cfg(std::move(bcfg_b));

            pylabhub::scripting::RoleHostCore core;
            pylabhub::scripting::RoleAPIBase  api(
                core, "proc", "proc.dual_hub.uid00000001");
            api.set_name("dual_hub_smoke");
            api.set_auth("", "");

            pylabhub::config::HubRefConfig hub_a;
            hub_a.broker = broker_a.endpoint;
            pylabhub::config::HubRefConfig hub_b;
            hub_b.broker = broker_b.endpoint;

            std::vector<pylabhub::scripting::Presence> presences;
            {
                pylabhub::scripting::Presence p_in;
                p_in.hub       = hub_a;
                p_in.channel   = make_test_channel_name("dual.in");
                p_in.role_kind = pylabhub::scripting::RoleKind::Consumer;
                presences.push_back(std::move(p_in));
                pylabhub::scripting::Presence p_out;
                p_out.hub       = hub_b;
                p_out.channel   = make_test_channel_name("dual.out");
                p_out.role_kind = pylabhub::scripting::RoleKind::Producer;
                presences.push_back(std::move(p_out));
            }
            pylabhub::scripting::RoleHandler handler(std::move(presences));
            ASSERT_EQ(handler.connection_count(), 2u);

            ASSERT_TRUE(handler.start_connections(api));
            EXPECT_TRUE(handler.connections_started());
            EXPECT_TRUE(handler.connections()[0].brc->is_connected());
            EXPECT_TRUE(handler.connections()[1].brc->is_connected());

            handler.stop_connections();
            EXPECT_FALSE(handler.connections_started());
            EXPECT_EQ(handler.connections()[0].brc.get(), nullptr);
            EXPECT_EQ(handler.connections()[1].brc.get(), nullptr);

            broker_a.stop_and_join();
            broker_b.stop_and_join();
        },
        "role_state.role_handler_connections_dual_hub",
        logger_module(), crypto_module(), zmq_module());
}

// ============================================================================
// role_handler_connections_double_start_rejected
//   Wave-B M4a — `start_connections()` refuses a second call without
//   intervening `stop_connections()`.  Pins the docstring idempotency
//   contract.
// ============================================================================

int role_handler_connections_double_start_rejected()
{
    return run_gtest_worker(
        []() {
            BrokerService::Config bcfg;
            bcfg.endpoint  = "tcp://127.0.0.1:0";
            bcfg.use_curve = false;
            auto broker = start_broker_with_cfg(std::move(bcfg));

            pylabhub::scripting::RoleHostCore core;
            pylabhub::scripting::RoleAPIBase  api(
                core, "prod", "prod.double_start.uid00000001");
            api.set_name("double_start");
            api.set_auth("", "");

            pylabhub::config::HubRefConfig hub_cfg;
            hub_cfg.broker = broker.endpoint;

            std::vector<pylabhub::scripting::Presence> presences;
            {
                pylabhub::scripting::Presence p;
                p.hub       = hub_cfg;
                p.channel   = make_test_channel_name("role_handler.double");
                p.role_kind = pylabhub::scripting::RoleKind::Producer;
                presences.push_back(std::move(p));
            }
            pylabhub::scripting::RoleHandler handler(std::move(presences));

            ASSERT_TRUE(handler.start_connections(api));
            EXPECT_TRUE(handler.connections_started());

            EXPECT_FALSE(handler.start_connections(api))
                << "Double-start without intervening stop must be rejected.";
            EXPECT_TRUE(handler.connections_started())
                << "Rejected second start must NOT clear the started state.";

            handler.stop_connections();
            broker.stop_and_join();
        },
        "role_state.role_handler_connections_double_start_rejected",
        logger_module(), crypto_module(), zmq_module());
}

} // namespace pylabhub::tests::worker::broker_role_state

namespace
{

struct BrokerRoleStateWorkerRegistrar
{
    BrokerRoleStateWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char** argv) -> int {
                if (argc < 2) return -1;
                std::string_view mode = argv[1];
                const auto dot = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "broker_role_state")
                    return -1;
                std::string scenario(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::broker_role_state;
                if (scenario == "metrics_reclaim_cycle")       return metrics_reclaim_cycle();
                if (scenario == "pending_recovers_to_ready")   return pending_recovers_to_ready();
                if (scenario == "stuck_in_pending_reclaimed")  return stuck_in_pending_reclaimed();
                if (scenario == "band_membership_cleaned_on_role_close")
                    return band_membership_cleaned_on_role_close();
                if (scenario == "role_entry_terminal_cleanup_on_last_presence_dereg")
                    return role_entry_terminal_cleanup_on_last_presence_dereg();
                if (scenario == "role_entry_terminal_cleanup_on_consumer_left_last")
                    return role_entry_terminal_cleanup_on_consumer_left_last();
                if (scenario == "consumer_heartbeat_timeout_fires_consumer_died_notify")
                    return consumer_heartbeat_timeout_fires_consumer_died_notify();
                if (scenario == "role_handler_connections_start_stop_smoke")
                    return role_handler_connections_start_stop_smoke();
                if (scenario == "role_handler_connections_dual_hub")
                    return role_handler_connections_dual_hub();
                if (scenario == "role_handler_connections_double_start_rejected")
                    return role_handler_connections_double_start_rejected();
                return 1;
            });
    }
};

static BrokerRoleStateWorkerRegistrar g_broker_role_state_registrar; // NOLINT(cert-err58-cpp)

} // anon
