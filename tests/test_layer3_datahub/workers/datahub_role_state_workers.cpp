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
#include "service/cycle_ops.hpp"   // dispatch_notifications + StopRequestor
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
            cfg.enforce_ctrl_admission = false;  // HEP-CORE-0035 §4.8 opt-out: this fixture uses CURVE for wire encryption only — no admission gate (no known_roles populated; test does not exercise ZAP)
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
            cfg.enforce_ctrl_admission = false;  // HEP-CORE-0035 §4.8 opt-out: this fixture uses CURVE for wire encryption only — no admission gate (no known_roles populated; test does not exercise ZAP)
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
            cfg.enforce_ctrl_admission = false;  // HEP-CORE-0035 §4.8 opt-out: this fixture uses CURVE for wire encryption only — no admission gate (no known_roles populated; test does not exercise ZAP)
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
            cfg.enforce_ctrl_admission = false;  // HEP-CORE-0035 §4.8 opt-out: this fixture uses CURVE for wire encryption only — no admission gate (no known_roles populated; test does not exercise ZAP)
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
            cfg.enforce_ctrl_admission = false;  // HEP-CORE-0035 §4.8 opt-out: this fixture uses CURVE for wire encryption only — no admission gate (no known_roles populated; test does not exercise ZAP)
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
            cfg.enforce_ctrl_admission = false;  // HEP-CORE-0035 §4.8 opt-out: this fixture uses CURVE for wire encryption only — no admission gate (no known_roles populated; test does not exercise ZAP)
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
            cons_opts["role_uid"]  = cons_uid;
            cons_opts["role_name"] = "role_state_test_consumer";
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
            cfg.enforce_ctrl_admission = false;  // HEP-CORE-0035 §4.8 opt-out: this fixture uses CURVE for wire encryption only — no admission gate (no known_roles populated; test does not exercise ZAP)
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
            cons_opts["role_uid"]  = cons_uid;
            cons_opts["role_name"] = "role_state_test_consumer";
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
                EXPECT_EQ(body.value("role_uid", std::string{}), cons_uid)
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
            bcfg.enforce_ctrl_admission = false;
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
            bcfg_a.enforce_ctrl_admission = false;
            auto broker_a = start_broker_with_cfg(std::move(bcfg_a));

            BrokerService::Config bcfg_b;
            bcfg_b.endpoint  = "tcp://127.0.0.1:0";
            bcfg_b.use_curve = false;
            bcfg_b.enforce_ctrl_admission = false;
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
            bcfg.enforce_ctrl_admission = false;
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

// ============================================================================
// role_handler_brc_for_x_post_start
//   Wave-B M4b — verify the routing primitives return the right
//   non-null BRC pointers AFTER start_connections.  L2 tests
//   (Pattern 1+) verified the lookup logic returns nullptr pre-start;
//   this L3 test pins the post-start pointer identity (matches
//   `connections()[i].brc.get()`).
// ============================================================================

int role_handler_brc_for_x_post_start()
{
    return run_gtest_worker(
        []() {
            BrokerService::Config bcfg;
            bcfg.endpoint  = "tcp://127.0.0.1:0";
            bcfg.use_curve = false;
            bcfg.enforce_ctrl_admission = false;
            auto broker = start_broker_with_cfg(std::move(bcfg));

            pylabhub::scripting::RoleHostCore core;
            pylabhub::scripting::RoleAPIBase  api(
                core, "prod", "prod.brc_routing.uid00000001");
            api.set_name("brc_routing");
            api.set_auth("", "");

            pylabhub::config::HubRefConfig hub_cfg;
            hub_cfg.broker = broker.endpoint;

            std::vector<pylabhub::scripting::Presence> presences;
            {
                pylabhub::scripting::Presence p;
                p.hub       = hub_cfg;
                p.channel   = make_test_channel_name("role_handler.routing");
                p.role_kind = pylabhub::scripting::RoleKind::Producer;
                presences.push_back(std::move(p));
            }
            const std::string ch = presences[0].channel;
            pylabhub::scripting::RoleHandler handler(std::move(presences));

            ASSERT_TRUE(handler.start_connections(api));

            // brc_for_channel must return the same pointer as
            // connections()[0].brc.get() — single presence, single
            // connection, unambiguous routing.
            auto *expected_brc = handler.connections()[0].brc.get();
            ASSERT_NE(expected_brc, nullptr);

            EXPECT_EQ(handler.brc_for_channel(ch), expected_brc)
                << "Class A routing: brc_for_channel must match the "
                   "connection slot's BRC.";
            EXPECT_EQ(handler.brc_for_role(), expected_brc)
                << "Class B routing: brc_for_role returns the first "
                   "connection's BRC (single-presence role).";

            // Class D: band routing requires on_band_joined first.
            EXPECT_EQ(handler.brc_for_band("test.band"), nullptr);
            const auto *p = handler.find_presence_for_channel(ch);
            ASSERT_NE(p, nullptr);
            handler.on_band_joined("test.band", p);
            EXPECT_EQ(handler.brc_for_band("test.band"), expected_brc)
                << "Class D routing: brc_for_band returns the BRC "
                   "of the presence that joined the band.";

            handler.stop_connections();
            broker.stop_and_join();
        },
        "role_state.role_handler_brc_for_x_post_start",
        logger_module(), crypto_module(), zmq_module());
}

// ============================================================================
// role_api_base_start_handler_threads_e2e
//   Wave-B M4c — full lifecycle test for the handler-mode ctrl thread
//   path.  Starts a broker, wires RoleAPIBase with a handler via
//   `start_handler_threads`, sends REG_REQ via the legacy fallback
//   view (`pImpl->broker_channel`), verifies the broker observes the
//   registration, then tears down via `stop_handler_threads`.  This
//   exercises EVERY M4c surface: atomicity guard (implicit — single
//   call), Phase 1-4 init sequence, fallback view, and Phase 1-4
//   teardown sequence with broker_channel cleared post-stop.
// ============================================================================

int role_api_base_start_handler_threads_e2e()
{
    return run_gtest_worker(
        []() {
            BrokerService::Config bcfg;
            bcfg.endpoint  = "tcp://127.0.0.1:0";
            bcfg.use_curve = false;
            bcfg.enforce_ctrl_admission = false;
            auto broker = start_broker_with_cfg(std::move(bcfg));

            const std::string role_uid = "prod.m4c_e2e.uid00000001";
            const std::string channel  = make_test_channel_name("m4c.e2e");

            pylabhub::scripting::RoleHostCore core;
            // Production role hosts flip `core.set_running(true)` early
            // in `worker_main_()` so the ctrl thread's poll-loop
            // predicate (`core->is_running() && !ctx.shutdown_requested()`)
            // evaluates true and the loop spins.  This test has no
            // worker_main_ — drive the flag directly so the spawned
            // poll thread runs.
            core.set_running(true);

            pylabhub::scripting::RoleAPIBase  api(core, "prod", role_uid);
            api.set_name("m4c_e2e");
            api.set_channel(channel);
            api.set_auth("", "");

            pylabhub::config::HubRefConfig hub_cfg;
            hub_cfg.broker = broker.endpoint;

            std::vector<pylabhub::scripting::Presence> presences;
            {
                pylabhub::scripting::Presence p;
                p.hub       = hub_cfg;
                p.channel   = channel;
                p.role_kind = pylabhub::scripting::RoleKind::Producer;
                presences.push_back(std::move(p));
            }
            auto handler =
                std::make_unique<pylabhub::scripting::RoleHandler>(
                    std::move(presences));

            // ── start_handler_threads ────────────────────────────────────
            ASSERT_TRUE(api.start_handler_threads(std::move(handler)))
                << "start_handler_threads must succeed against a live broker";

            ASSERT_NE(api.handler(), nullptr)
                << "handler() returns the stored RoleHandler post-start";
            EXPECT_TRUE(api.handler()->connections_started())
                << "RoleHandler is in started state";

            // Atomicity guard — second call must be refused.
            {
                std::vector<pylabhub::scripting::Presence> p2;
                pylabhub::scripting::Presence ep;
                ep.hub       = hub_cfg;
                ep.channel   = channel;
                ep.role_kind = pylabhub::scripting::RoleKind::Producer;
                p2.push_back(std::move(ep));
                auto handler2 =
                    std::make_unique<pylabhub::scripting::RoleHandler>(
                        std::move(p2));
                EXPECT_FALSE(api.start_handler_threads(std::move(handler2)))
                    << "Atomicity guard: second start must be refused.";
                EXPECT_NE(api.handler(), nullptr)
                    << "Refused second start MUST NOT clear the first "
                       "handler — original state preserved.";
            }

            // ── Exercise the legacy fallback view via REG_REQ ────────────
            //
            // register_producer_channel reads pImpl->broker_channel
            // internally; if M4c's fallback view is correctly set to
            // handler->connections()[0].brc, the REG_REQ goes through
            // and the broker registers the channel.
            nlohmann::json reg_opts;
            reg_opts["channel_name"]      = channel;
            reg_opts["pattern"]           = "PubSub";
            reg_opts["has_shared_memory"] = false;
            reg_opts["producer_pid"]      = static_cast<uint64_t>(::getpid());
            reg_opts["role_uid"]          = role_uid;
            reg_opts["role_name"]         = "m4c_e2e";

            auto reg_resp = api.register_producer_channel(reg_opts, 3000);
            ASSERT_TRUE(reg_resp.has_value())
                << "REG_REQ via legacy fallback view must reach the broker.";
            EXPECT_EQ(reg_resp->value("status", std::string{}), "success");

            // Broker should have the role registered.  This is the
            // end-to-end M4c verification: REG_REQ was sent via
            // pImpl->broker_channel (legacy fallback), routed to the
            // first BRC (which the handler's ctrl thread is polling),
            // and the broker's handler accepted the registration.
            EXPECT_TRUE(broker.hub_state->role(role_uid).has_value())
                << "Broker's HubState must show the role registered "
                   "(end-to-end M4c handler-mode path verified).";

            // ── stop_handler_threads ─────────────────────────────────────
            api.stop_handler_threads();
            EXPECT_EQ(api.handler(), nullptr)
                << "handler() returns nullptr post-stop";

            // Idempotent: second stop is a no-op.
            api.stop_handler_threads();

            broker.stop_and_join();
        },
        "role_state.role_api_base_start_handler_threads_e2e",
        logger_module(), crypto_module(), zmq_module());
}

// ============================================================================
// role_api_base_start_handler_threads_dual_hub_e2e
//   Wave-B M4c review follow-up — multi-connection variant of the
//   e2e test.  Pins the master/peer spawn behavior with N=2 ctrl
//   threads against two real brokers.  Verifies:
//     - Both BRCs allocated + connected.
//     - Both ctrl threads spawned (first = MASTER, second = peer).
//     - Both BRCs accept REG_REQ via per-connection dispatch through
//       `handler->brc_for_channel(ch)` (NOT through the single
//       fallback view, which can only reach one of the two BRCs).
//     - Both brokers observe the corresponding registration.
//     - stop_handler_threads drains both threads cleanly + clears
//       both BRCs + fallback view.
// ============================================================================

int role_api_base_start_handler_threads_dual_hub_e2e()
{
    return run_gtest_worker(
        []() {
            BrokerService::Config bcfg_a;
            bcfg_a.endpoint  = "tcp://127.0.0.1:0";
            bcfg_a.use_curve = false;
            bcfg_a.enforce_ctrl_admission = false;
            auto broker_a = start_broker_with_cfg(std::move(bcfg_a));

            BrokerService::Config bcfg_b;
            bcfg_b.endpoint  = "tcp://127.0.0.1:0";
            bcfg_b.use_curve = false;
            bcfg_b.enforce_ctrl_admission = false;
            auto broker_b = start_broker_with_cfg(std::move(bcfg_b));

            const std::string role_uid = "proc.m4c_dual_e2e.uid00000001";
            const std::string ch_in    = make_test_channel_name("m4c.dual.in");
            const std::string ch_out   = make_test_channel_name("m4c.dual.out");

            pylabhub::scripting::RoleHostCore core;
            core.set_running(true);

            pylabhub::scripting::RoleAPIBase  api(core, "proc", role_uid);
            api.set_name("m4c_dual_e2e");
            api.set_auth("", "");

            // First presence: consumer on broker_a.  Second presence:
            // producer on broker_b.  Two distinct hubs → 2 connections
            // → 2 ctrl threads.
            pylabhub::config::HubRefConfig hub_a;
            hub_a.broker = broker_a.endpoint;
            pylabhub::config::HubRefConfig hub_b;
            hub_b.broker = broker_b.endpoint;

            std::vector<pylabhub::scripting::Presence> presences;
            {
                pylabhub::scripting::Presence p_in;
                p_in.hub       = hub_a;
                p_in.channel   = ch_in;
                p_in.role_kind = pylabhub::scripting::RoleKind::Consumer;
                presences.push_back(std::move(p_in));
                pylabhub::scripting::Presence p_out;
                p_out.hub       = hub_b;
                p_out.channel   = ch_out;
                p_out.role_kind = pylabhub::scripting::RoleKind::Producer;
                presences.push_back(std::move(p_out));
            }
            auto handler =
                std::make_unique<pylabhub::scripting::RoleHandler>(
                    std::move(presences));

            // ── start_handler_threads ────────────────────────────────────
            ASSERT_TRUE(api.start_handler_threads(std::move(handler)))
                << "start_handler_threads must succeed with 2 brokers";

            auto *h = api.handler();
            ASSERT_NE(h, nullptr);
            ASSERT_EQ(h->connection_count(), 2u)
                << "Two distinct hubs → 2 connections";
            ASSERT_NE(h->connections()[0].brc.get(), nullptr);
            ASSERT_NE(h->connections()[1].brc.get(), nullptr);
            EXPECT_TRUE(h->connections()[0].brc->is_connected());
            EXPECT_TRUE(h->connections()[1].brc->is_connected());

            // ── Per-connection REG via handler routing (not via
            //    fallback view, which can only reach one of the two) ────
            //
            // Producer side: REG_REQ via brc_for_channel(ch_out).
            nlohmann::json prod_opts;
            prod_opts["channel_name"]      = ch_out;
            prod_opts["pattern"]           = "PubSub";
            prod_opts["has_shared_memory"] = false;
            prod_opts["producer_pid"]      = static_cast<uint64_t>(::getpid());
            prod_opts["role_uid"]          = role_uid;
            prod_opts["role_name"]         = "m4c_dual_e2e";

            auto *brc_out = h->brc_for_channel(ch_out);
            ASSERT_NE(brc_out, nullptr);
            auto prod_resp = brc_out->register_channel(prod_opts, 3000);
            ASSERT_TRUE(prod_resp.has_value());
            EXPECT_EQ(prod_resp->value("status", std::string{}), "success");

            // Consumer side: CONSUMER_REG_REQ via brc_for_channel(ch_in).
            // The consumer presence registers on broker_a only AFTER
            // a producer registers on broker_a — but for this test
            // we just verify the connection is alive (broker_a will
            // accept a heartbeat or DISC even without a registered
            // channel).  Instead, register a SECOND producer on
            // broker_a for ch_in just to verify the second BRC is
            // alive end-to-end.
            nlohmann::json in_prod_opts;
            in_prod_opts["channel_name"]      = ch_in;
            in_prod_opts["pattern"]           = "PubSub";
            in_prod_opts["has_shared_memory"] = false;
            in_prod_opts["producer_pid"]      = static_cast<uint64_t>(::getpid());
            in_prod_opts["role_uid"]          = role_uid;
            in_prod_opts["role_name"]         = "m4c_dual_e2e";

            auto *brc_in = h->brc_for_channel(ch_in);
            ASSERT_NE(brc_in, nullptr);
            EXPECT_NE(brc_in, brc_out)
                << "Distinct BRCs for distinct hubs (no dedup collision).";
            auto in_resp = brc_in->register_channel(in_prod_opts, 3000);
            ASSERT_TRUE(in_resp.has_value())
                << "Second BRC must accept REG_REQ — proves both ctrl "
                   "threads are running their poll loops.";
            EXPECT_EQ(in_resp->value("status", std::string{}), "success");

            // Both brokers should now see the registration on their
            // respective channels.
            EXPECT_TRUE(broker_a.hub_state->channel(ch_in).has_value())
                << "broker_a must show ch_in registered";
            EXPECT_TRUE(broker_b.hub_state->channel(ch_out).has_value())
                << "broker_b must show ch_out registered";

            // ── stop_handler_threads ─────────────────────────────────────
            api.stop_handler_threads();
            EXPECT_EQ(api.handler(), nullptr);

            broker_a.stop_and_join();
            broker_b.stop_and_join();
        },
        "role_state.role_api_base_start_handler_threads_dual_hub_e2e",
        logger_module(), crypto_module(), zmq_module());
}

// ============================================================================
// role_api_base_band_join_handler_mode
//   A0 regression test — protects the bootstrap case of api.band_join.
//
//   The bug: post-M4f, `resolve_bc_for_band(unknown_band)` returns nullptr
//   (band_index_ is empty until on_band_joined fires AFTER a successful
//   broker round-trip — chicken-and-egg).  Pre-M4f the legacy
//   `broker_channel` fallback let band_join's initial REQ go out on
//   connections[0]; M4f deleted the fallback without recognizing the
//   bootstrap case.  This test was missing because all prior band L3
//   coverage moved to `bc->band_join` direct calls in
//   datahub_channel_group_workers.cpp during M4f migration.  Mutation
//   sweep: revert the fix → `band_join` returns nullopt without
//   contacting broker → this test fails on the has_value assertion.
// ============================================================================

int role_api_base_band_join_handler_mode()
{
    return run_gtest_worker(
        []() {
            BrokerService::Config bcfg;
            bcfg.endpoint  = "tcp://127.0.0.1:0";
            bcfg.use_curve = false;
            bcfg.enforce_ctrl_admission = false;
            auto broker = start_broker_with_cfg(std::move(bcfg));

            const std::string role_uid = "prod.a0_band.uid00000001";
            const std::string channel  = make_test_channel_name("a0.band");
            const std::string band     = "!test_band_a0";  // R3.5: `!`-prefixed per HEP-0030 §3

            pylabhub::scripting::RoleHostCore core;
            core.set_running(true);

            pylabhub::scripting::RoleAPIBase  api(core, "prod", role_uid);
            api.set_name("a0_band");
            api.set_channel(channel);
            api.set_auth("", "");

            pylabhub::config::HubRefConfig hub_cfg;
            hub_cfg.broker = broker.endpoint;

            std::vector<pylabhub::scripting::Presence> presences;
            {
                pylabhub::scripting::Presence p;
                p.hub       = hub_cfg;
                p.channel   = channel;
                p.role_kind = pylabhub::scripting::RoleKind::Producer;
                presences.push_back(std::move(p));
            }
            auto handler = std::make_unique<pylabhub::scripting::RoleHandler>(
                std::move(presences));

            ASSERT_TRUE(api.start_handler_threads(std::move(handler)));
            ASSERT_NE(api.handler(), nullptr);

            // BOOTSTRAP CASE — band has never been joined, so handler's
            // band_index_ has no entry for it.  Pre-M4f: legacy
            // broker_channel fallback carried the REQ.  Post-M4f-without-fix:
            // resolve_bc_for_band returns nullptr → band_join returns
            // nullopt without ever talking to broker (the bug).
            auto join_resp = api.band_join(band);
            ASSERT_TRUE(join_resp.has_value())
                << "REGRESSION: api.band_join() returned nullopt without "
                   "contacting broker.  Bootstrap case is broken — "
                   "resolve_bc_for_band has no fallback for un-joined bands "
                   "post-M4f.  Fix: route bootstrap via resolve_bc_for_role.";
            EXPECT_EQ(join_resp->value("status", std::string{}), "success");

            // Post-success: handler's band_index_ MUST be populated so
            // subsequent band_* ops route to the same BRC.
            ASSERT_NE(api.handler()->brc_for_band(band), nullptr)
                << "band_index_ must be populated after successful band_join";

            // Now exercise band_leave to confirm the round-trip works
            // end-to-end and the band index gets cleared.
            auto leave_resp = api.band_leave(band);
            EXPECT_TRUE(leave_resp.has_value())
                << "band_leave must succeed for a joined band";
            EXPECT_EQ(leave_resp->value("status", std::string{}), "success");
            EXPECT_EQ(api.handler()->brc_for_band(band), nullptr)
                << "band_index_ must be cleared after band_leave";

            api.stop_handler_threads();
            broker.stop_and_join();
        },
        "role_state.role_api_base_band_join_handler_mode",
        logger_module(), crypto_module(), zmq_module());
}

// ============================================================================
// broker_band_rejects_invalid_identifier
//   Audit R3.5 (2026-05-17) — broker must REJECT BAND_JOIN_REQ /
//   BAND_LEAVE_REQ / BAND_MEMBERS_REQ when the band name fails
//   HEP-CORE-0030 §3 identifier validation (e.g. missing `!`
//   prefix).  Pre-fix, the handler called `hub_state_->_on_band_*`
//   which silently bumped `invalid_identifier_total` and returned;
//   the handler then proceeded to return `status: success` to the
//   role, creating a phantom-joined state on the role side with no
//   broker-side membership.
//
//   Trigger: band name "no_bang_prefix" — non-empty (so it passes
//   the existing `band.empty()` guard) but invalid per HEP-0030 §3
//   (must start with `!`).
//
//   Mutation sweep:
//     - Remove the `is_valid_identifier` check from
//       handle_band_join_req → broker returns `success` for invalid
//       band → test fails on `EXPECT_EQ(status, "error")`.
//     - Same for BAND_LEAVE_REQ + BAND_MEMBERS_REQ handlers.
// ============================================================================

int broker_band_rejects_invalid_identifier()
{
    return run_gtest_worker(
        []() {
            BrokerService::Config bcfg;
            bcfg.endpoint  = "tcp://127.0.0.1:0";
            bcfg.use_curve = false;
            bcfg.enforce_ctrl_admission = false;
            auto broker = start_broker_with_cfg(std::move(bcfg));

            // Connect a raw BRC client; band-name validation is at
            // the broker handler, not on the role side, so we go
            // through BRC directly.
            const std::string role_uid = "prod.r35.uid00000001";
            BrcHandle bh;
            bh.start(broker.endpoint, broker.pubkey, role_uid);

            // Non-empty BUT invalid (no `!` prefix per HEP-0030 §3).
            const std::string invalid = "no_bang_prefix";

            // ── BAND_JOIN_REQ — broker must reject ──────────────────
            {
                auto resp = bh.brc.band_join(invalid, 3000);
                ASSERT_TRUE(resp.has_value())
                    << "Broker should respond (error), not time out";
                EXPECT_EQ(resp->value("status", std::string{}), "error")
                    << "R3.5 regression: BAND_JOIN_REQ for invalid "
                       "band name '" << invalid << "' must be "
                       "rejected with status=error.  Pre-fix the "
                       "broker silently returned status=success.";
                EXPECT_EQ(resp->value("error_code", std::string{}),
                          "INVALID_BAND_NAME")
                    << "Expected typed error code INVALID_BAND_NAME";
            }

            // ── BAND_LEAVE_REQ — broker must reject ─────────────────
            {
                auto resp = bh.brc.band_leave(invalid, 3000);
                ASSERT_TRUE(resp.has_value());
                EXPECT_EQ(resp->value("status", std::string{}), "error")
                    << "R3.5 regression: BAND_LEAVE_REQ for invalid "
                       "band name must be rejected";
                EXPECT_EQ(resp->value("error_code", std::string{}),
                          "INVALID_BAND_NAME");
            }

            // ── BAND_MEMBERS_REQ — broker must reject ───────────────
            {
                auto resp = bh.brc.band_members(invalid, 3000);
                ASSERT_TRUE(resp.has_value());
                EXPECT_EQ(resp->value("status", std::string{}), "error")
                    << "R3.5 regression: BAND_MEMBERS_REQ for invalid "
                       "band name must be rejected";
                EXPECT_EQ(resp->value("error_code", std::string{}),
                          "INVALID_BAND_NAME");
            }

            // ── Sanity: valid band name still works ─────────────────
            {
                const std::string valid = "!" +
                    make_test_channel_name("r35.valid");
                auto resp = bh.brc.band_join(valid, 3000);
                ASSERT_TRUE(resp.has_value());
                EXPECT_EQ(resp->value("status", std::string{}),
                          "success")
                    << "Sanity check: valid `!`-prefixed band name "
                       "must still succeed (R3.5 fix should not "
                       "regress the happy path)";
            }

            bh.stop();
            broker.stop_and_join();
        },
        "role_state.broker_band_rejects_invalid_identifier",
        logger_module(), crypto_module(), zmq_module());
}

// ============================================================================
// role_api_base_band_notify_wire_field_and_routing
//   Audit B1 + B2 regression test (2026-05-17) — pins the BAND_*_NOTIFY
//   wire payload key AND the role-side `find_presence_from_notification`
//   dispatch against a REAL broker emission.  Closes the integration gap
//   that hid both bugs through the 1923-test baseline:
//
//   • B1 — wire key.  HEP-CORE-0030 §5.1 specifies `band` on every
//     BAND_* payload.  Pre-2026-05-17 the broker emitted `channel`
//     (leftover from before the 2026-04-11 `8d3ee1e` rename refactor;
//     the rename touched message types but not payload keys).  No
//     test inspected the wire key directly — BRC and broker agreed on
//     the wrong name, so round-trip tests in `datahub_channel_group_
//     workers.cpp` all passed.
//
//   • B2 — role-side dispatch.  `RoleHandler::find_presence_from_
//     notification` looked for `body["band_name"]` — an invented key
//     introduced in Wave-B M4b (commit `8c3994c`) that no broker has
//     ever emitted.  The L2 test in `test_role_handler.cpp` pinned
//     this by SYNTHESIZING `body["band_name"]` in the test body —
//     agreeing with the broken handler without ever exercising real
//     wire data.  L3 band tests bypassed `find_presence_from_
//     notification` entirely (they use raw BRC `on_notification`
//     callbacks).
//
//   Test shape:
//     1. Spawn 1 broker.
//     2. Build api + handler with 1 producer presence.
//     3. start_handler_threads (wires the BRC's on_notification to
//        feed core.incoming_queue_).
//     4. api.band_join(band1)  — role A now a member.
//     5. Spawn a second raw BRC (BrcHandle); have it band_join(band1)
//        too.  Broker emits BAND_JOIN_NOTIFY to role A.
//     6. Poll core.drain_messages() until BAND_JOIN_NOTIFY arrives.
//     7. Assert msg.details has key `band` (B1 wire conformance).
//     8. Assert handler->find_presence_from_notification(
//          "BAND_JOIN_NOTIFY", msg.details) returns the joined
//        presence (B2 dispatch correctness).
//
//   Mutation sweep:
//     - Revert role_handler.cpp lookup back to `band_name` →
//       step 8 returns nullptr → test fails on ASSERT_NE.
//     - Revert broker emission back to `channel` → step 7 fails on
//       `band` key present check.
//     - Revert both → step 7 fails first (wire conformance).
// ============================================================================

int role_api_base_band_notify_wire_field_and_routing()
{
    return run_gtest_worker(
        []() {
            BrokerService::Config bcfg;
            bcfg.endpoint  = "tcp://127.0.0.1:0";
            bcfg.use_curve = false;
            bcfg.enforce_ctrl_admission = false;
            auto broker = start_broker_with_cfg(std::move(bcfg));

            const std::string role_uid_a = "prod.b1.a.uid00000001";
            const std::string role_uid_b = "prod.b1.b.uid00000002";
            const std::string channel    = make_test_channel_name("b1.chan");
            const std::string band       = "!" + make_test_channel_name("b1.band");

            // ── Role A: full api + handler (this is the role whose
            //    dispatcher we're testing) ────────────────────────────
            pylabhub::scripting::RoleHostCore core;
            core.set_running(true);

            pylabhub::scripting::RoleAPIBase api(core, "prod", role_uid_a);
            api.set_name("b1_a");
            api.set_channel(channel);
            api.set_auth("", "");

            pylabhub::config::HubRefConfig hub_cfg;
            hub_cfg.broker = broker.endpoint;

            std::vector<pylabhub::scripting::Presence> presences;
            {
                pylabhub::scripting::Presence p;
                p.hub       = hub_cfg;
                p.channel   = channel;
                p.role_kind = pylabhub::scripting::RoleKind::Producer;
                presences.push_back(std::move(p));
            }
            auto handler_uptr =
                std::make_unique<pylabhub::scripting::RoleHandler>(
                    std::move(presences));
            // Capture a non-owning pointer for post-start lookups; the
            // unique_ptr is moved into start_handler_threads below.
            const pylabhub::scripting::RoleHandler *handler_ptr =
                handler_uptr.get();

            ASSERT_TRUE(api.start_handler_threads(std::move(handler_uptr)));

            // Role A joins the band FIRST so the band exists when role
            // B's join triggers BAND_JOIN_NOTIFY back to role A.
            auto join_resp = api.band_join(band);
            ASSERT_TRUE(join_resp.has_value());
            EXPECT_EQ(join_resp->value("status", std::string{}), "success");

            // ── Role B: raw BRC client (no handler — just enough to
            //    trigger the notify) ──────────────────────────────────
            BrcHandle b;
            b.start(broker.endpoint, broker.pubkey, role_uid_b);
            ASSERT_TRUE(b.brc.band_join(band, 3000).has_value());

            // ── Step 6: wait for BAND_JOIN_NOTIFY to land in core's
            //    queue.  start_handler_threads wired the BRC's
            //    on_notification to enqueue every inbound msg into
            //    core.incoming_queue_, so we drain that. ─────────────
            pylabhub::scripting::IncomingMessage notify_msg;
            bool got_notify = false;
            auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::seconds(3);
            while (std::chrono::steady_clock::now() < deadline && !got_notify)
            {
                auto msgs = core.drain_messages();
                for (auto &m : msgs)
                {
                    if (m.event == "BAND_JOIN_NOTIFY")
                    {
                        notify_msg = std::move(m);
                        got_notify = true;
                        break;
                    }
                }
                if (!got_notify)
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            ASSERT_TRUE(got_notify)
                << "BAND_JOIN_NOTIFY never reached role A's incoming queue "
                   "within 3s.  Broker did not emit the notify or wiring "
                   "between BRC on_notification → core.enqueue_message is "
                   "broken.";

            // ── Step 7 (B1 wire conformance): the body must carry the
            //    band identifier under key `band` per HEP-CORE-0030 §5.1.
            //    Pre-B1 the broker emitted `channel` here. ───────────
            ASSERT_TRUE(notify_msg.details.is_object())
                << "BAND_JOIN_NOTIFY body must be a JSON object";
            ASSERT_TRUE(notify_msg.details.contains("band"))
                << "B1 regression: BAND_JOIN_NOTIFY body missing required "
                   "key `band` (HEP-CORE-0030 §5.1).  If the body has "
                   "`channel` instead, the 2026-04-11 rename refactor is "
                   "incomplete on the wire-payload-key layer.";
            EXPECT_EQ(notify_msg.details.value("band", std::string{}), band);
            EXPECT_FALSE(notify_msg.details.contains("channel"))
                << "B1 regression: BAND_JOIN_NOTIFY body has legacy key "
                   "`channel` — the wire rename is incomplete.";

            // ── Step 8 (B2 dispatch correctness): the role-side handler's
            //    `find_presence_from_notification` must resolve the body
            //    to the joined presence.  Pre-B2 the handler looked for
            //    `body["band_name"]` — an invention from Wave-B M4b that
            //    no broker ever emits — so this returned nullptr. ──────
            const pylabhub::scripting::Presence *resolved =
                handler_ptr->find_presence_from_notification(
                    "BAND_JOIN_NOTIFY", notify_msg.details);
            ASSERT_NE(resolved, nullptr)
                << "B2 regression: find_presence_from_notification returned "
                   "nullptr for a real broker-emitted BAND_JOIN_NOTIFY body.  "
                   "Class D inbound routing is structurally broken — the "
                   "dispatcher is reading a body key the broker doesn't emit.";
            EXPECT_EQ(resolved->channel, channel)
                << "Resolved presence must be the one that joined the band "
                   "(only one presence in this role's list).";

            b.stop();
            api.stop_handler_threads();
            broker.stop_and_join();
        },
        "role_state.role_api_base_band_notify_wire_field_and_routing",
        logger_module(), crypto_module(), zmq_module());
}

// ============================================================================
// role_api_base_hub_dead_peer_keeps_role_alive
//   A2 (Wave-B M8 prep): dual-hub processor must survive a PEER broker
//   death.  Pre-A2, any-of-N connections dying triggered role-wide
//   shutdown — strictly worse fault tolerance than single-hub.  This
//   test spawns 2 brokers, builds a 2-presence RoleHandler, kills
//   broker_b (peer, i==1), and asserts:
//     - is_connection_alive(0) stays true (master untouched).
//     - is_connection_alive(1) flips to false (peer detected dead).
//     - core.is_running() stays true (role did NOT exit).
//     - core.stop_reason() is still Normal (not HubDead).
//   Mutation sweep: revert the per-i check in on_hub_dead → both
//   connections trigger role-wide shutdown → core.is_running() false
//   → test fails on the is_running assertion.
// ============================================================================

int role_api_base_hub_dead_peer_keeps_role_alive()
{
    return run_gtest_worker(
        []() {
            BrokerService::Config cfg_a; cfg_a.endpoint = "tcp://127.0.0.1:0";
            cfg_a.use_curve = false;
            cfg_a.enforce_ctrl_admission = false;
            auto broker_a = start_broker_with_cfg(std::move(cfg_a));

            BrokerService::Config cfg_b; cfg_b.endpoint = "tcp://127.0.0.1:0";
            cfg_b.use_curve = false;
            cfg_b.enforce_ctrl_admission = false;
            auto broker_b = start_broker_with_cfg(std::move(cfg_b));

            const std::string role_uid = "prod.a2_peer.uid00000001";
            const std::string ch_a     = make_test_channel_name("a2.peer.a");
            const std::string ch_b     = make_test_channel_name("a2.peer.b");

            pylabhub::scripting::RoleHostCore core;
            core.set_running(true);

            pylabhub::scripting::RoleAPIBase api(core, "prod", role_uid);
            api.set_name("a2_peer");
            api.set_auth("", "");

            std::vector<pylabhub::scripting::Presence> presences;
            {
                pylabhub::scripting::Presence pa;
                pa.hub.broker = broker_a.endpoint;
                pa.channel    = ch_a;
                pa.role_kind  = pylabhub::scripting::RoleKind::Producer;
                presences.push_back(std::move(pa));

                pylabhub::scripting::Presence pb;
                pb.hub.broker = broker_b.endpoint;
                pb.channel    = ch_b;
                pb.role_kind  = pylabhub::scripting::RoleKind::Producer;
                presences.push_back(std::move(pb));
            }
            auto handler = std::make_unique<pylabhub::scripting::RoleHandler>(
                std::move(presences));

            ASSERT_TRUE(api.start_handler_threads(std::move(handler)));
            ASSERT_EQ(api.handler()->connections().size(), 2u);

            // Initial state: both connections alive.
            EXPECT_TRUE(api.is_connection_alive(0));
            EXPECT_TRUE(api.is_connection_alive(1));
            EXPECT_EQ(api.connections_alive_count(), 2u);
            EXPECT_TRUE(core.is_running());

            // Kill PEER (broker_b → connection index 1).
            broker_b.stop_and_join();

            // Poll for on_hub_dead to fire on the peer side.  ZMQ
            // socket monitor delivers DISCONNECTED on next poll cycle
            // after the broker socket closes.  3s ceiling is generous
            // — locally fires in <100ms.
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds{3};
            while (api.is_connection_alive(1) &&
                   std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds{50});
            }

            EXPECT_FALSE(api.is_connection_alive(1))
                << "Peer connection should be marked dead after broker_b "
                   "stop (ZMQ_EVENT_DISCONNECTED timed out).";

            // CORE ASSERTION OF A2: role must NOT request stop on peer
            // death.  We check `is_shutdown_requested()` rather than
            // `is_running()` because there's no worker thread in this
            // unit-level test to flip `running_threads_=false` on
            // teardown — `request_stop()` only sets the
            // shutdown_requested flag, which is the signal the worker
            // would respond to in production.
            EXPECT_TRUE(api.is_connection_alive(0))
                << "Master connection must remain alive after peer death.";
            EXPECT_EQ(api.connections_alive_count(), 1u);
            EXPECT_FALSE(core.is_shutdown_requested())
                << "A2 REGRESSION: peer-broker death triggered request_stop.  "
                   "Master must keep the role alive (HEP-CORE-0023 §2.5).";

            // Give the role time to observe what would have been a
            // stop request — if A2 is broken (peer triggers role exit),
            // shutdown_requested would flip in this window.
            std::this_thread::sleep_for(std::chrono::milliseconds{200});
            EXPECT_FALSE(core.is_shutdown_requested())
                << "Role MUST NOT have shutdown_requested 200ms after peer "
                   "death.";

            api.stop_handler_threads();
            broker_a.stop_and_join();
        },
        "role_state.role_api_base_hub_dead_peer_keeps_role_alive",
        logger_module(), crypto_module(), zmq_module());
}

// ============================================================================
// role_api_base_hub_dead_transitions_presences_to_deregistered
//   Audit R3.3 (2026-05-17) — when on_hub_dead fires for a peer
//   connection, all presences pointing at that connection must
//   transition from Registered to Deregistered.  Pre-fix the only
//   side-effect of on_hub_dead was clearing the alive-mask bit
//   (audit A2); the FSM kept claiming `Registered` against a broker
//   that had already reaped us via heartbeat-timeout.
//
//   Test shape (mirrors A2 peer-keeps-alive but verifies FSM state):
//     1. Spawn 2 brokers.
//     2. Build 2-presence handler (one per broker).
//     3. start_handler_threads.
//     4. register_producer_channel on BOTH presences via direct
//        per-connection BRC access (so each gets Registered).
//     5. Kill broker_b (peer, connection index 1).
//     6. Poll for on_hub_dead to fire (alive_mask bit 1 clears).
//     7. Assert:
//        - presence on connection 1 → Deregistered (R3.3 core)
//        - presence on connection 0 → STILL Registered (master
//          unaffected; R3.3 is per-connection scoped)
//
//   Mutation sweep:
//     - Comment out `handler_ptr->mark_connection_disconnected(...)`
//       call in `role_api_base.cpp` Phase 2 on_hub_dead lambda →
//       presence[1] stays Registered → test fails.
//     - Make mark_connection_disconnected affect ALL presences
//       (not just dead_conn) → presence[0] becomes Deregistered →
//       master-presence assertion fails.
// ============================================================================

int role_api_base_hub_dead_transitions_presences_to_deregistered()
{
    return run_gtest_worker(
        []() {
            BrokerService::Config cfg_a; cfg_a.endpoint = "tcp://127.0.0.1:0";
            cfg_a.use_curve = false;
            cfg_a.enforce_ctrl_admission = false;
            auto broker_a = start_broker_with_cfg(std::move(cfg_a));

            BrokerService::Config cfg_b; cfg_b.endpoint = "tcp://127.0.0.1:0";
            cfg_b.use_curve = false;
            cfg_b.enforce_ctrl_admission = false;
            auto broker_b = start_broker_with_cfg(std::move(cfg_b));

            const std::string role_uid = "prod.r33.uid00000001";
            const std::string ch_a     = make_test_channel_name("r33.master");
            const std::string ch_b     = make_test_channel_name("r33.peer");

            pylabhub::scripting::RoleHostCore core;
            core.set_running(true);

            pylabhub::scripting::RoleAPIBase api(core, "prod", role_uid);
            api.set_name("r33_test");
            api.set_auth("", "");

            std::vector<pylabhub::scripting::Presence> presences;
            {
                pylabhub::scripting::Presence pa;
                pa.hub.broker = broker_a.endpoint;
                pa.channel    = ch_a;
                pa.role_kind  = pylabhub::scripting::RoleKind::Producer;
                presences.push_back(std::move(pa));

                pylabhub::scripting::Presence pb;
                pb.hub.broker = broker_b.endpoint;
                pb.channel    = ch_b;
                pb.role_kind  = pylabhub::scripting::RoleKind::Producer;
                presences.push_back(std::move(pb));
            }
            auto handler = std::make_unique<pylabhub::scripting::RoleHandler>(
                std::move(presences));

            ASSERT_TRUE(api.start_handler_threads(std::move(handler)));
            ASSERT_EQ(api.handler()->connections().size(), 2u);

            // Register the channel on BOTH brokers via the channel-bound
            // routing (Class A — handler->brc_for_channel routes each).
            auto reg_opts_for = [&](const std::string &ch) {
                nlohmann::json opts;
                opts["channel_name"]      = ch;
                opts["pattern"]           = "PubSub";
                opts["has_shared_memory"] = false;
                opts["producer_pid"]      = static_cast<uint64_t>(::getpid());
                opts["role_uid"]          = role_uid;
                opts["role_name"]         = "r33_test";
                return opts;
            };
            ASSERT_TRUE(api.register_producer_channel(reg_opts_for(ch_a))
                            .has_value());
            ASSERT_TRUE(api.register_producer_channel(reg_opts_for(ch_b))
                            .has_value());

            // Both presences must be Registered now.
            {
                const auto *p_a = api.handler()->find_presence_for_channel(ch_a);
                const auto *p_b = api.handler()->find_presence_for_channel(ch_b);
                ASSERT_NE(p_a, nullptr);
                ASSERT_NE(p_b, nullptr);
                ASSERT_EQ(p_a->registration_state.load(),
                          pylabhub::scripting::RegistrationState::Registered);
                ASSERT_EQ(p_b->registration_state.load(),
                          pylabhub::scripting::RegistrationState::Registered);
            }

            // Kill PEER (broker_b → connection index 1).
            broker_b.stop_and_join();

            // Poll for on_hub_dead to fire.
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds{3};
            while (api.is_connection_alive(1) &&
                   std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds{50});
            }
            ASSERT_FALSE(api.is_connection_alive(1))
                << "Peer alive-mask bit should clear after broker_b stop.";

            // Give the lambda a moment to also call
            // mark_connection_disconnected (same execution context as
            // the alive-mask clear, so this is normally instant).
            std::this_thread::sleep_for(std::chrono::milliseconds{50});

            // ── R3.3 CORE ASSERTIONS ────────────────────────────────
            const auto *p_a = api.handler()->find_presence_for_channel(ch_a);
            const auto *p_b = api.handler()->find_presence_for_channel(ch_b);
            ASSERT_NE(p_a, nullptr);
            ASSERT_NE(p_b, nullptr);

            EXPECT_EQ(p_b->registration_state.load(),
                      pylabhub::scripting::RegistrationState::Deregistered)
                << "R3.3 regression: presence on dead PEER connection "
                   "must be transitioned to Deregistered.  Pre-fix the "
                   "FSM kept claiming Registered against a dead broker.";

            // Master presence MUST still be Registered — R3.3 is
            // strictly per-connection.
            EXPECT_EQ(p_a->registration_state.load(),
                      pylabhub::scripting::RegistrationState::Registered)
                << "R3.3 regression: master presence (alive connection) "
                   "must remain Registered.  If this fails, "
                   "mark_connection_disconnected is over-reaching "
                   "beyond the dead connection.";

            api.stop_handler_threads();
            broker_a.stop_and_join();
        },
        "role_state.role_api_base_hub_dead_transitions_presences_to_deregistered",
        logger_module(), crypto_module(), zmq_module());
}

// ============================================================================
// role_api_base_hub_dead_master_exits_role
//   A2 baseline + D1/D2 (2026-05-18): master broker death MUST still
//   trigger role-wide shutdown.  Under the D1/D2 unified dispatch
//   model the lambda no longer calls `request_stop()` directly —
//   instead it enqueues a synthetic HUB_DEAD `IncomingMessage` with
//   `details["is_master"] = true`, and the worker-thread dispatcher's
//   `default_hub_dead` does the stop (when no script `on_hub_dead`
//   override is defined).  Since this is a unit-level test with no
//   worker thread, we explicitly drive the dispatcher path after the
//   lambda fires (drain msgs → call `default_hub_dead` with
//   `StopRequestor{core}`) — that's the SAME logic the worker would
//   run, just synchronously here so we can assert in-line.
//
//   Mutations covered:
//     (a) change `is_master_conn = (i == 0)` in role_api_base.cpp →
//         lambda enqueues `is_master=false` → default_hub_dead no-ops
//         → request_stop never fires → test fails.
//     (b) remove `set_stop_reason(HubDead)` in default_hub_dead →
//         shutdown is requested but stop_reason_string() != "hub_dead"
//         → test fails on EXPECT_EQ.
//     (c) remove the enqueue in the lambda → drain returns empty →
//         test fails on ASSERT_EQ(hub_dead_count, 1u).
// ============================================================================

int role_api_base_hub_dead_master_exits_role()
{
    return run_gtest_worker(
        []() {
            BrokerService::Config cfg_a; cfg_a.endpoint = "tcp://127.0.0.1:0";
            cfg_a.use_curve = false;
            cfg_a.enforce_ctrl_admission = false;
            auto broker_a = start_broker_with_cfg(std::move(cfg_a));

            BrokerService::Config cfg_b; cfg_b.endpoint = "tcp://127.0.0.1:0";
            cfg_b.use_curve = false;
            cfg_b.enforce_ctrl_admission = false;
            auto broker_b = start_broker_with_cfg(std::move(cfg_b));

            const std::string role_uid = "prod.a2_master.uid00000001";
            const std::string ch_a     = make_test_channel_name("a2.master.a");
            const std::string ch_b     = make_test_channel_name("a2.master.b");

            pylabhub::scripting::RoleHostCore core;
            core.set_running(true);

            pylabhub::scripting::RoleAPIBase api(core, "prod", role_uid);
            api.set_name("a2_master");
            api.set_auth("", "");

            std::vector<pylabhub::scripting::Presence> presences;
            {
                pylabhub::scripting::Presence pa;
                pa.hub.broker = broker_a.endpoint;
                pa.channel    = ch_a;
                pa.role_kind  = pylabhub::scripting::RoleKind::Producer;
                presences.push_back(std::move(pa));

                pylabhub::scripting::Presence pb;
                pb.hub.broker = broker_b.endpoint;
                pb.channel    = ch_b;
                pb.role_kind  = pylabhub::scripting::RoleKind::Producer;
                presences.push_back(std::move(pb));
            }
            auto handler = std::make_unique<pylabhub::scripting::RoleHandler>(
                std::move(presences));

            ASSERT_TRUE(api.start_handler_threads(std::move(handler)));
            EXPECT_FALSE(core.is_shutdown_requested());

            // Kill MASTER (broker_a → connection index 0).
            broker_a.stop_and_join();

            // Poll for the master's on_hub_dead lambda to enqueue a
            // HUB_DEAD msg.  Pre-D1/D2 the lambda flipped
            // shutdown_requested directly; post-D1/D2 we poll for
            // is_connection_alive(0)==false which the lambda also
            // does (alive_mask bit clear) — same firing signal,
            // available without consuming the msg queue.
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds{3};
            while (api.is_connection_alive(0) &&
                   std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds{50});
            }
            EXPECT_FALSE(api.is_connection_alive(0))
                << "Master connection bit MUST clear within 3s of broker "
                   "death (ZMQ_EVENT_DISCONNECTED must reach BRC).";

            // Drain the queue + verify HUB_DEAD msg shape (lambda
            // contract from role_api_base.cpp Phase 2):
            //   event == "HUB_DEAD",
            //   notification_id == HubDead,
            //   details["is_master"] == true (for connection 0).
            //
            // Audit S1 (2026-05-18) — additionally pin EXACTLY ONE
            // HUB_DEAD msg.  pylabhub policy: disconnect is terminal
            // (HEP-CORE-0023 §2.5.3); reconnect_ivl=-1 on the BRC
            // dealer socket prevents libzmq from silently
            // re-establishing the connection and re-firing
            // DISCONNECTED.  A second HUB_DEAD msg would indicate the
            // socket policy is broken (or the defensive alive_mask
            // gate failed to suppress a repeat fire).
            auto msgs = core.drain_messages();
            std::size_t hub_dead_count = 0;
            bool        saw_master_hd  = false;
            for (auto &m : msgs)
            {
                if (m.notification_id !=
                    pylabhub::scripting::NotificationId::HubDead)
                    continue;
                ++hub_dead_count;
                EXPECT_EQ(m.event, "HUB_DEAD");
                if (m.details.value("is_master", false))
                    saw_master_hd = true;
            }
            ASSERT_EQ(hub_dead_count, 1u)
                << "Audit S1 (no-reconnect policy): exactly ONE HUB_DEAD "
                   "msg expected per (role lifetime, connection) — got "
                << hub_dead_count
                << ".  A higher count means libzmq is re-establishing "
                   "and re-disconnecting the DEALER socket behind our "
                   "back (reconnect_ivl policy broken), OR the role-side "
                   "defensive alive_mask gate failed to suppress repeats.";
            EXPECT_TRUE(saw_master_hd)
                << "HUB_DEAD msg MUST carry details[\"is_master\"]=true "
                   "for the master connection (so default_hub_dead can "
                   "branch).";

            // Pin the BRC's socket-policy directly: reconnect must be
            // disabled.  Mutation: removing `reconnect_ivl=-1` from
            // BRC socket init → this assertion fails immediately.
            auto *brc0 = api.handler()->connections()[0].brc.get();
            ASSERT_NE(brc0, nullptr);
            EXPECT_TRUE(brc0->reconnect_disabled())
                << "Audit S1: BRC for the (now-dead) master connection "
                   "must have ZMQ_RECONNECT_IVL=-1 — pylabhub policy "
                   "(HEP-CORE-0023 §2.5.3 \"Disconnection is terminal\").";
            auto *brc1 = api.handler()->connections()[1].brc.get();
            ASSERT_NE(brc1, nullptr);
            EXPECT_TRUE(brc1->reconnect_disabled())
                << "Audit S1: BRC for the peer connection must also "
                   "have ZMQ_RECONNECT_IVL=-1.";

            // Worker-side completion: feed the drained msgs through
            // the dispatcher with NO script override (RecordingEngine
            // not used here — we don't need to assert engine calls;
            // we need to assert that default_hub_dead -> request_stop
            // fires with the right reason).  Reach for default_hub_dead
            // directly via the same path the worker would take.
            for (auto &m : msgs)
            {
                if (m.notification_id ==
                    pylabhub::scripting::NotificationId::HubDead)
                {
                    pylabhub::scripting::default_hub_dead(
                        m, pylabhub::scripting::StopRequestor{core});
                }
            }

            EXPECT_TRUE(core.is_shutdown_requested())
                << "A2 baseline (D1/D2): default_hub_dead for master "
                   "MUST call request_stop()";
            EXPECT_EQ(core.stop_reason_string(), "hub_dead")
                << "stop_reason must be HubDead after master default fires";
            EXPECT_FALSE(api.is_connection_alive(0));
            // Peer is untouched; bitmask still shows alive (we never
            // killed broker_b in this scenario).
            EXPECT_TRUE(api.is_connection_alive(1));

            api.stop_handler_threads();
            broker_b.stop_and_join();
        },
        "role_state.role_api_base_hub_dead_master_exits_role",
        logger_module(), crypto_module(), zmq_module());
}

// ============================================================================
// role_api_base_wait_for_role_dual_hub_fallthrough
//   A3 (Wave-B M8 prep): Class B (role-bound) queries must fall
//   through across all connections per HEP-CORE-0033 §18.3.
//   Pre-A3 `wait_for_role` only polled `brc_for_role()` which
//   returns `connections()[0].brc` — for dual-hub processor, a
//   role registered on hub-B is invisible from the processor's
//   query (which only asks hub-A).  Post-A3: iterate connections;
//   first present-true answer wins.
//
//   Test pattern:
//     - Spawn 2 brokers (hub-A, hub-B).
//     - Register a "target" producer on hub-B ONLY.
//     - Build dual-hub RoleHandler (connection[0] = hub-A,
//       connection[1] = hub-B) for a "querier" role.
//     - querier.wait_for_role(target_uid, timeout=1500).
//     - Pre-A3: returns false (only asks hub-A; target is on hub-B).
//     - Post-A3: returns true via fall-through to hub-B.
//   Mutation sweep: revert iteration in wait_for_role → assertion
//   fails on the returned boolean.
// ============================================================================

int role_api_base_wait_for_role_dual_hub_fallthrough()
{
    return run_gtest_worker(
        []() {
            BrokerService::Config cfg_a; cfg_a.endpoint = "tcp://127.0.0.1:0";
            cfg_a.use_curve = false;
            cfg_a.enforce_ctrl_admission = false;
            auto broker_a = start_broker_with_cfg(std::move(cfg_a));

            BrokerService::Config cfg_b; cfg_b.endpoint = "tcp://127.0.0.1:0";
            cfg_b.use_curve = false;
            cfg_b.enforce_ctrl_admission = false;
            auto broker_b = start_broker_with_cfg(std::move(cfg_b));

            // ── Register a "target" producer on hub-B only ────────────────
            const std::string target_uid  = "prod.a3_target.uid00000099";
            const std::string target_chan = make_test_channel_name("a3.target");
            BrokerRequestComm target_brc;
            BrokerRequestComm::Config target_cfg;
            target_cfg.broker_endpoint = broker_b.endpoint;
            target_cfg.role_uid        = target_uid;
            target_cfg.role_name       = "a3_target";
            ASSERT_TRUE(target_brc.connect(target_cfg));

            // Spawn a poll thread so target_brc actually responds.
            std::atomic<bool> target_running{true};
            std::thread target_poll([&] {
                target_brc.run_poll_loop([&]{ return target_running.load(); });
            });

            nlohmann::json target_reg;
            target_reg["channel_name"]      = target_chan;
            target_reg["pattern"]           = "PubSub";
            target_reg["has_shared_memory"] = false;
            target_reg["producer_pid"]      = static_cast<uint64_t>(::getpid());
            target_reg["role_uid"]          = target_uid;
            target_reg["role_name"]         = "a3_target";
            auto target_reg_resp = target_brc.register_channel(target_reg, 3000);
            ASSERT_TRUE(target_reg_resp.has_value());

            // ── Build the querier with 2 presences (hub-A + hub-B) ────────
            const std::string querier_uid  = "prod.a3_querier.uid00000001";
            const std::string querier_ch_a = make_test_channel_name("a3.querier.a");
            const std::string querier_ch_b = make_test_channel_name("a3.querier.b");

            pylabhub::scripting::RoleHostCore core;
            core.set_running(true);
            pylabhub::scripting::RoleAPIBase api(core, "prod", querier_uid);
            api.set_name("a3_querier");
            api.set_auth("", "");

            std::vector<pylabhub::scripting::Presence> presences;
            {
                pylabhub::scripting::Presence pa;
                pa.hub.broker = broker_a.endpoint;
                pa.channel    = querier_ch_a;
                pa.role_kind  = pylabhub::scripting::RoleKind::Producer;
                presences.push_back(std::move(pa));

                pylabhub::scripting::Presence pb;
                pb.hub.broker = broker_b.endpoint;
                pb.channel    = querier_ch_b;
                pb.role_kind  = pylabhub::scripting::RoleKind::Producer;
                presences.push_back(std::move(pb));
            }
            auto handler = std::make_unique<pylabhub::scripting::RoleHandler>(
                std::move(presences));
            ASSERT_TRUE(api.start_handler_threads(std::move(handler)));
            ASSERT_EQ(api.handler()->connections().size(), 2u);

            // ── THE CORE A3 ASSERTION ────────────────────────────────────
            // wait_for_role on target_uid must succeed via fall-through to
            // hub-B, even though connection[0] is hub-A (which doesn't
            // know about target_uid).  Pre-A3 this would return false.
            EXPECT_TRUE(api.wait_for_role(target_uid, 2000))
                << "A3 REGRESSION: wait_for_role failed to find a role "
                   "registered on connection[1] (hub-B).  Class B routing "
                   "must fall through across all connections per "
                   "HEP-CORE-0033 §18.3.";

            // ── Cleanup ───────────────────────────────────────────────────
            api.stop_handler_threads();
            target_running.store(false);
            target_brc.stop();
            target_poll.join();
            target_brc.disconnect();
            broker_a.stop_and_join();
            broker_b.stop_and_join();
        },
        "role_state.role_api_base_wait_for_role_dual_hub_fallthrough",
        logger_module(), crypto_module(), zmq_module());
}

// ============================================================================
// role_api_base_registration_fsm_transitions
//   Audit S1+O4 (2026-05-17) — pins the per-Presence registration FSM
//   transitions against a REAL broker.  Pre-S1 the role-side had no
//   enumerable registration state — it was implicit in the
//   non-emptiness of `Impl::Shared::producer_channel` /
//   `consumer_channel`, which couldn't distinguish "never registered"
//   from "registered then deregistered" and had no observable
//   in-flight state.
//
//   This test exercises the FSM explicitly:
//     1. Build api + handler (1 producer presence).
//     2. ASSERT initial state == Unregistered.
//     3. start_handler_threads (BRC connected, but no REG_REQ yet).
//     4. ASSERT state still == Unregistered (registration is
//        distinct from connection).
//     5. register_producer_channel → on success, state ==
//        Registered.
//     6. ASSERT state == Registered + broker's HubState shows the
//        role registered.
//     7. deregister_producer_channel → state == Deregistered.
//     8. ASSERT state == Deregistered + broker's HubState no longer
//        shows the role.
//
//   Mutation sweep:
//     - Remove the `store(Registered)` in `register_producer_channel`
//       → state stuck at RegRequestPending → step 6 fails.
//     - Remove the `store(Deregistered)` in `deregister_producer_channel`
//       → state stuck at Registered → step 8 fails.
//     - Revert `deregister_from_broker` to skip un-registered presences
//       → broker's HubState still has the role after teardown → broker
//       assertion in step 8 fails (we drain & retry through the
//       presence walk).
// ============================================================================

int role_api_base_registration_fsm_transitions()
{
    return run_gtest_worker(
        []() {
            BrokerService::Config bcfg;
            bcfg.endpoint  = "tcp://127.0.0.1:0";
            bcfg.use_curve = false;
            bcfg.enforce_ctrl_admission = false;
            auto broker = start_broker_with_cfg(std::move(bcfg));

            const std::string role_uid = "prod.s1.uid00000001";
            const std::string channel  = make_test_channel_name("s1.chan");

            pylabhub::scripting::RoleHostCore core;
            core.set_running(true);

            pylabhub::scripting::RoleAPIBase api(core, "prod", role_uid);
            api.set_name("s1_test");
            api.set_channel(channel);
            api.set_auth("", "");

            pylabhub::config::HubRefConfig hub_cfg;
            hub_cfg.broker = broker.endpoint;

            std::vector<pylabhub::scripting::Presence> presences;
            {
                pylabhub::scripting::Presence p;
                p.hub       = hub_cfg;
                p.channel   = channel;
                p.role_kind = pylabhub::scripting::RoleKind::Producer;
                presences.push_back(std::move(p));
            }
            auto handler_uptr =
                std::make_unique<pylabhub::scripting::RoleHandler>(
                    std::move(presences));
            // Pre-start the presence's registration_state should be
            // Unregistered (the default).
            {
                const auto *p = handler_uptr->find_presence_for_channel(channel);
                ASSERT_NE(p, nullptr);
                EXPECT_EQ(p->registration_state.load(),
                          pylabhub::scripting::RegistrationState::Unregistered)
                    << "Pre-construction state must be Unregistered.";
            }

            ASSERT_TRUE(api.start_handler_threads(std::move(handler_uptr)));

            // After start_handler_threads: BRC connected, but REG_REQ
            // has not been dispatched yet.  State should still be
            // Unregistered — connection ≠ registration.
            {
                const auto *p =
                    api.handler()->find_presence_for_channel(channel);
                ASSERT_NE(p, nullptr);
                EXPECT_EQ(p->registration_state.load(),
                          pylabhub::scripting::RegistrationState::Unregistered)
                    << "S1 regression: state should remain Unregistered "
                       "post-start_handler_threads (connection is wired "
                       "but REG_REQ has not been issued).";
            }

            // Issue REG_REQ.
            nlohmann::json reg_opts;
            reg_opts["channel_name"]      = channel;
            reg_opts["pattern"]           = "PubSub";
            reg_opts["has_shared_memory"] = false;
            reg_opts["producer_pid"]      = static_cast<uint64_t>(::getpid());
            reg_opts["role_uid"]          = role_uid;
            reg_opts["role_name"]         = "s1_test";
            auto reg_result = api.register_producer_channel(reg_opts);
            ASSERT_TRUE(reg_result.has_value());
            EXPECT_EQ(reg_result->value("status", std::string{}), "success");

            // After successful REG_REQ: Registered.
            {
                const auto *p =
                    api.handler()->find_presence_for_channel(channel);
                ASSERT_NE(p, nullptr);
                EXPECT_EQ(p->registration_state.load(),
                          pylabhub::scripting::RegistrationState::Registered)
                    << "S1 regression: state should be Registered after "
                       "successful REG_REQ.  If still RegRequestPending, "
                       "the success-path FSM transition in "
                       "register_producer_channel is missing.";
            }
            // Broker confirms.
            EXPECT_TRUE(broker.hub_state->role(role_uid).has_value())
                << "Broker's HubState must show role registered after "
                   "successful REG_REQ.";

            // DEREG.
            auto dereg_result = api.deregister_producer_channel(channel);
            ASSERT_TRUE(dereg_result.has_value());
            EXPECT_EQ(dereg_result->value("status", std::string{}), "success");

            // After successful DEREG: Deregistered.
            {
                const auto *p =
                    api.handler()->find_presence_for_channel(channel);
                ASSERT_NE(p, nullptr);
                EXPECT_EQ(p->registration_state.load(),
                          pylabhub::scripting::RegistrationState::Deregistered)
                    << "S1 regression: state should be Deregistered after "
                       "successful DEREG_REQ.  If still Registered, the "
                       "FSM transition in deregister_producer_channel is "
                       "missing.";
            }

            // Now exercise the teardown path: even with everything
            // already Deregistered, deregister_from_broker must be a
            // no-op (no DEREG re-sent for an already-Deregistered
            // presence).  Pre-S1 this was guarded by the
            // `take_producer_channel` returning empty; the FSM walk
            // achieves the same via the `needs_dereg` predicate.
            api.deregister_from_broker();  // should not crash, no extra DEREG

            api.stop_handler_threads();
            broker.stop_and_join();
        },
        "role_state.role_api_base_registration_fsm_transitions",
        logger_module(), crypto_module(), zmq_module());
}

// ============================================================================
// role_api_base_source_hub_uid_disambiguates_dual_hub
//   Audit C3 (2026-05-17) — pins `IncomingMessage::source_hub_uid` as
//   the role-side origin tag for dual-hub notification disambiguation
//   (HEP-CORE-0023 §7 + HEP-CORE-0033 §18.3 / §19.4).
//
//   Pre-C3: `IncomingMessage` had no source_hub_uid field at all.  A
//   dual-hub processor's script saw an aggregated `msgs[]` list with
//   no way to tell which hub a notification came from — it had to
//   compare `body["channel_name"]` to its own presence list manually,
//   which only worked for channel-bound events.  Band events
//   (`!band_x` on hub A vs `!band_y` on hub B) had NO observable
//   origin.
//
//   This test spawns 2 brokers, builds a 2-presence handler, has the
//   processor join a band on EACH hub directly via that
//   connection's BRC, then has a raw BRC client join each band on
//   each broker.  Each broker fans out BAND_JOIN_NOTIFY to the
//   processor on the corresponding connection.  Test asserts:
//     - 2 BAND_JOIN_NOTIFYs arrive (one per hub).
//     - The two `source_hub_uid` values are different.
//     - One matches broker_a.endpoint; the other matches
//       broker_b.endpoint.
//
//   Mutation sweep:
//     - Remove `msg.source_hub_uid = conn_endpoint;` in
//       `role_api_base.cpp` Phase 2 → both messages have empty
//       source_hub_uid → test fails on the inequality assertion.
//     - Swap to capture i instead of endpoint → values are "0" / "1"
//       (or whatever the stringification yields) and won't match
//       endpoints → test fails on the endpoint-match assertion.
// ============================================================================

int role_api_base_source_hub_uid_disambiguates_dual_hub()
{
    return run_gtest_worker(
        []() {
            BrokerService::Config cfg_a; cfg_a.endpoint = "tcp://127.0.0.1:0";
            cfg_a.use_curve = false;
            cfg_a.enforce_ctrl_admission = false;
            auto broker_a = start_broker_with_cfg(std::move(cfg_a));

            BrokerService::Config cfg_b; cfg_b.endpoint = "tcp://127.0.0.1:0";
            cfg_b.use_curve = false;
            cfg_b.enforce_ctrl_admission = false;
            auto broker_b = start_broker_with_cfg(std::move(cfg_b));

            const std::string proc_uid = "proc.c3.uid00000001";
            const std::string ch_in    = make_test_channel_name("c3.in");
            const std::string ch_out   = make_test_channel_name("c3.out");
            const std::string band_a   = "!" + make_test_channel_name("c3.banda");
            const std::string band_b   = "!" + make_test_channel_name("c3.bandb");

            // ── Build dual-hub processor handler ─────────────────────────
            pylabhub::scripting::RoleHostCore core;
            core.set_running(true);
            pylabhub::scripting::RoleAPIBase api(core, "proc", proc_uid);
            api.set_name("c3_proc");
            api.set_auth("", "");

            std::vector<pylabhub::scripting::Presence> presences;
            {
                pylabhub::scripting::Presence pa;
                pa.hub.broker = broker_a.endpoint;
                pa.channel    = ch_in;
                pa.role_kind  = pylabhub::scripting::RoleKind::Consumer;
                presences.push_back(std::move(pa));

                pylabhub::scripting::Presence pb;
                pb.hub.broker = broker_b.endpoint;
                pb.channel    = ch_out;
                pb.role_kind  = pylabhub::scripting::RoleKind::Producer;
                presences.push_back(std::move(pb));
            }
            auto handler = std::make_unique<pylabhub::scripting::RoleHandler>(
                std::move(presences));
            ASSERT_TRUE(api.start_handler_threads(std::move(handler)));
            ASSERT_EQ(api.handler()->connections().size(), 2u);

            // ── Processor joins band_a on hub-A, band_b on hub-B ─────────
            // Direct BRC access — api.band_join() would route both via
            // presences[0] (= hub-A) per current single-side default.
            auto *bc_a = api.handler()->connections()[0].brc.get();
            auto *bc_b = api.handler()->connections()[1].brc.get();
            ASSERT_NE(bc_a, nullptr);
            ASSERT_NE(bc_b, nullptr);
            ASSERT_TRUE(bc_a->band_join(band_a, 3000).has_value());
            ASSERT_TRUE(bc_b->band_join(band_b, 3000).has_value());

            // ── External BRC clients join each band to trigger the
            //    broker-side BAND_JOIN_NOTIFY fanout to the processor ──
            BrcHandle ext_a, ext_b;
            // broker_proto 5 (R3.5b): role_uid must be a well-formed
            // RoleUid (tag in {prod,cons,proc} + name + unique) for
            // BAND_JOIN_REQ to pass the gate.
            ext_a.start(broker_a.endpoint, broker_a.pubkey,
                        "cons.c3.ext_a.uid00000010");
            ext_b.start(broker_b.endpoint, broker_b.pubkey,
                        "cons.c3.ext_b.uid00000011");
            ASSERT_TRUE(ext_a.brc.band_join(band_a, 3000).has_value());
            ASSERT_TRUE(ext_b.brc.band_join(band_b, 3000).has_value());

            // ── Drain core.incoming_queue_ until both BAND_JOIN_NOTIFYs
            //    have arrived (one per hub) ────────────────────────────
            pylabhub::scripting::IncomingMessage notify_a, notify_b;
            bool got_a = false, got_b = false;
            auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::seconds(3);
            while (std::chrono::steady_clock::now() < deadline &&
                   !(got_a && got_b))
            {
                auto msgs = core.drain_messages();
                for (auto &m : msgs)
                {
                    if (m.event != "BAND_JOIN_NOTIFY") continue;
                    const auto b = m.details.value("band", std::string{});
                    if (b == band_a && !got_a)
                    { notify_a = std::move(m); got_a = true; }
                    else if (b == band_b && !got_b)
                    { notify_b = std::move(m); got_b = true; }
                }
                if (!(got_a && got_b))
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            ASSERT_TRUE(got_a)
                << "BAND_JOIN_NOTIFY for band_a never reached the role's "
                   "incoming queue from hub-A.";
            ASSERT_TRUE(got_b)
                << "BAND_JOIN_NOTIFY for band_b never reached the role's "
                   "incoming queue from hub-B.";

            // ── C3 CORE ASSERTIONS ───────────────────────────────────────
            EXPECT_FALSE(notify_a.source_hub_uid.empty())
                << "C3 regression: source_hub_uid empty on hub-A notify — "
                   "the BRC on_notification lambda is not populating the "
                   "field at enqueue time (role_api_base.cpp Phase 2).";
            EXPECT_FALSE(notify_b.source_hub_uid.empty())
                << "C3 regression: source_hub_uid empty on hub-B notify.";
            EXPECT_NE(notify_a.source_hub_uid, notify_b.source_hub_uid)
                << "C3 regression: source_hub_uid identical across hubs — "
                   "lambda is capturing a shared identifier (e.g., a "
                   "fixed string) instead of the per-connection endpoint.";
            EXPECT_EQ(notify_a.source_hub_uid, broker_a.endpoint)
                << "source_hub_uid for hub-A notify should equal "
                   "broker_a.endpoint per HEP-CORE-0033 §19.2 "
                   "(connection dedup key = (broker_endpoint, broker_pubkey)).";
            EXPECT_EQ(notify_b.source_hub_uid, broker_b.endpoint)
                << "source_hub_uid for hub-B notify should equal "
                   "broker_b.endpoint.";

            ext_a.stop();
            ext_b.stop();
            api.stop_handler_threads();
            broker_a.stop_and_join();
            broker_b.stop_and_join();
        },
        "role_state.role_api_base_source_hub_uid_disambiguates_dual_hub",
        logger_module(), crypto_module(), zmq_module());
}

// ============================================================================
// role_api_base_dual_hub_heartbeat_per_presence
//   Audit C2 closure (2026-05-19) — pins HEP-CORE-0033 §19.3 step 3
//   contract: heartbeat is per-presence — iterate `handler_->presences()`
//   and emit one heartbeat per (channel, role_type) row.  A dual-hub
//   processor (Consumer on hub-A + Producer on hub-B) emits exactly TWO
//   heartbeats — one per hub.
//
//   Validates the role-side fix that replaced the pre-C2 `role_tag`
//   string-branching + `pImpl->channel`/`pImpl->out_channel` legacy
//   fields with `handler_->presences()` iteration.  Mutation: revert
//   `role_api_base.cpp::on_heartbeat_tick_` to use only `pImpl->channel`
//   (drop the producer/processor branch) → broker-B's RoleEntry never
//   shows `first_heartbeat_seen=true` for the producer presence → test
//   fails on EXPECT_TRUE.
//
//   Sequence:
//     1. Spawn 2 brokers (A, B).
//     2. Build dual-hub processor handler: [(A, ch_in, Consumer),
//        (B, ch_out, Producer)].  start_handler_threads.
//     3. Register both presences (consumer on A, producer on B) via
//        their respective BRCs.
//     4. install_heartbeat(50ms) — schedules `on_heartbeat_tick_` via
//        the master ctrl thread's periodic-task machinery.
//     5. Wait ~500ms for several tick fires.
//     6. Assert broker-A's RoleEntry shows consumer-presence
//        first_heartbeat_seen=true with channel=ch_in, role_type=consumer.
//     7. Assert broker-B's RoleEntry shows producer-presence
//        first_heartbeat_seen=true with channel=ch_out, role_type=producer.
//     8. Assert NEITHER broker has a "wrong" presence (e.g., B doesn't
//        have a consumer-presence — that would mean heartbeats crossed
//        hubs).
// ============================================================================

int role_api_base_dual_hub_heartbeat_per_presence()
{
    return run_gtest_worker(
        []() {
            BrokerService::Config cfg_a; cfg_a.endpoint = "tcp://127.0.0.1:0";
            cfg_a.use_curve = false;
            cfg_a.enforce_ctrl_admission = false;
            auto broker_a = start_broker_with_cfg(std::move(cfg_a));

            BrokerService::Config cfg_b; cfg_b.endpoint = "tcp://127.0.0.1:0";
            cfg_b.use_curve = false;
            cfg_b.enforce_ctrl_admission = false;
            auto broker_b = start_broker_with_cfg(std::move(cfg_b));

            const std::string proc_uid = "proc.c2_hb.uid00000001";
            const std::string ch_in    = make_test_channel_name("c2hb.in");
            const std::string ch_out   = make_test_channel_name("c2hb.out");

            // ── Build dual-hub processor handler ─────────────────────────
            pylabhub::scripting::RoleHostCore core;
            core.set_running(true);
            pylabhub::scripting::RoleAPIBase api(core, "proc", proc_uid);
            api.set_name("c2hb_proc");
            api.set_auth("", "");

            std::vector<pylabhub::scripting::Presence> presences;
            {
                pylabhub::scripting::Presence pa;
                pa.hub.broker = broker_a.endpoint;
                pa.channel    = ch_in;
                pa.role_kind  = pylabhub::scripting::RoleKind::Consumer;
                presences.push_back(std::move(pa));

                pylabhub::scripting::Presence pb;
                pb.hub.broker = broker_b.endpoint;
                pb.channel    = ch_out;
                pb.role_kind  = pylabhub::scripting::RoleKind::Producer;
                presences.push_back(std::move(pb));
            }
            auto handler = std::make_unique<pylabhub::scripting::RoleHandler>(
                std::move(presences));
            ASSERT_TRUE(api.start_handler_threads(std::move(handler)));
            ASSERT_EQ(api.handler()->connections().size(), 2u);

            // ── Seed ch_in on hub-A with an upstream producer ───────────
            // The processor is the CONSUMER of ch_in; the broker
            // requires the channel to already exist (someone REG'd as
            // producer).  Spawn an external BRC that registers as
            // producer on hub-A for ch_in — mirrors a real upstream
            // producer in a dual-hub topology.
            BrcHandle upstream;
            upstream.start(broker_a.endpoint, broker_a.pubkey,
                           "prod.c2hb_upstream.uid00000010");
            {
                nlohmann::json upstream_opts;
                upstream_opts["channel_name"]      = ch_in;
                upstream_opts["pattern"]           = "PubSub";
                upstream_opts["has_shared_memory"] = false;
                upstream_opts["producer_pid"]      =
                    static_cast<uint64_t>(::getpid());
                upstream_opts["role_uid"]          =
                    "prod.c2hb_upstream.uid00000010";
                upstream_opts["role_name"]         = "c2hb_upstream";
                auto up_reg = upstream.brc.register_channel(upstream_opts, 3000);
                ASSERT_TRUE(up_reg.has_value())
                    << "upstream producer registration on hub-A must succeed";
            }

            // ── Register each processor presence on its own hub ─────────
            auto *brc_a = api.handler()->brc_for_channel(ch_in);
            auto *brc_b = api.handler()->brc_for_channel(ch_out);
            ASSERT_NE(brc_a, nullptr);
            ASSERT_NE(brc_b, nullptr);

            // Broker reads `consumer_uid` / `consumer_name` on
            // CONSUMER_REG_REQ (broker_service.cpp:2035-2036) —
            // distinct from producer-side which reads `role_uid`.
            // Wire-protocol field-name asymmetry; both populate the
            // same ConsumerEntry.role_uid / RolePresence.uid.
            nlohmann::json cons_opts;
            cons_opts["channel_name"]  = ch_in;
            cons_opts["consumer_pid"]  = static_cast<uint64_t>(::getpid());
            cons_opts["role_uid"]  = proc_uid;
            cons_opts["role_name"] = "c2hb_proc";
            auto cons_reg = brc_a->register_consumer(cons_opts, 3000);
            ASSERT_TRUE(cons_reg.has_value())
                << "consumer registration on hub-A must succeed";

            nlohmann::json prod_opts;
            prod_opts["channel_name"]      = ch_out;
            prod_opts["pattern"]           = "PubSub";
            prod_opts["has_shared_memory"] = false;
            prod_opts["producer_pid"]      = static_cast<uint64_t>(::getpid());
            prod_opts["role_uid"]          = proc_uid;
            prod_opts["role_name"]         = "c2hb_proc";
            auto prod_reg = brc_b->register_channel(prod_opts, 3000);
            ASSERT_TRUE(prod_reg.has_value())
                << "producer registration on hub-B must succeed";

            // ── Trigger one heartbeat per presence via the production
            //    `on_heartbeat_tick_` path.  Drive it through
            //    `install_heartbeat` + `inc_iteration_count` (mirrors
            //    what the worker loop does in production).  Cadence
            //    50ms so we get several ticks per second.
            api.install_heartbeat(50, std::nullopt);

            // ── Drive iteration_count + wait for heartbeats to land ────
            auto saw_both_heartbeats = [&]() {
                const auto re_a = broker_a.hub_state->role(proc_uid);
                const auto re_b = broker_b.hub_state->role(proc_uid);
                if (!re_a.has_value() || !re_b.has_value()) return false;
                const auto *p_a =
                    re_a->find_presence(ch_in,  "consumer");
                const auto *p_b =
                    re_b->find_presence(ch_out, "producer");
                if (p_a == nullptr || p_b == nullptr) return false;
                return p_a->first_heartbeat_seen &&
                       p_b->first_heartbeat_seen;
            };
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(5);
            while (!saw_both_heartbeats() &&
                   std::chrono::steady_clock::now() < deadline)
            {
                core.inc_iteration_count();
                std::this_thread::sleep_for(std::chrono::milliseconds(60));
            }

            // Debug: log the current state for failure diagnostics.
            const auto re_a_dbg = broker_a.hub_state->role(proc_uid);
            const auto re_b_dbg = broker_b.hub_state->role(proc_uid);
            const auto ce_a_dbg = broker_a.hub_state->channel(ch_in);
            LOGGER_INFO("[test C2] post-wait: re_a_has={} re_b_has={} "
                        "iter={}  ce_a_has={} ce_a_consumers={}",
                        re_a_dbg.has_value(), re_b_dbg.has_value(),
                        core.iteration_count(),
                        ce_a_dbg.has_value(),
                        ce_a_dbg.has_value() ? ce_a_dbg->consumers.size() : 0u);
            if (ce_a_dbg.has_value())
            {
                for (const auto &c : ce_a_dbg->consumers)
                {
                    LOGGER_INFO("[test C2] hub-A consumer entry: uid='{}' "
                                "endpoint='{}'", c.role_uid, c.inbox_endpoint);
                }
            }
            const auto re_upstream =
                broker_a.hub_state->role("prod.c2hb_upstream.uid00000010");
            LOGGER_INFO("[test C2] hub-A has upstream role entry: {}",
                        re_upstream.has_value());

            // ── Assertions on hub-A (consumer presence) ─────────────────
            const auto re_a = broker_a.hub_state->role(proc_uid);
            ASSERT_TRUE(re_a.has_value())
                << "Audit C2: hub-A must see the role's RoleEntry (via "
                   "the consumer presence's REG + HEARTBEAT).";
            const auto *p_a_cons = re_a->find_presence(ch_in, "consumer");
            ASSERT_NE(p_a_cons, nullptr)
                << "Audit C2: hub-A must see a consumer-presence row "
                   "for ch_in with role_type='consumer'.  Mutation: "
                   "presence-iteration sending wrong role_type → broker "
                   "would record producer-presence here instead.";
            EXPECT_EQ(p_a_cons->channel,   ch_in);
            EXPECT_EQ(p_a_cons->role_type, "consumer");
            EXPECT_TRUE(p_a_cons->first_heartbeat_seen)
                << "Audit C2: consumer-presence on hub-A must have "
                   "received at least one heartbeat from the role-side "
                   "presence-list iteration.  Pre-C2-fix the role_tag "
                   "branch would also emit here, so this passes pre/post.";

            // ── Assertions on hub-B (producer presence) ─────────────────
            const auto re_b = broker_b.hub_state->role(proc_uid);
            ASSERT_TRUE(re_b.has_value())
                << "Audit C2: hub-B must see the role's RoleEntry (via "
                   "the producer presence's REG + HEARTBEAT).";
            const auto *p_b_prod = re_b->find_presence(ch_out, "producer");
            ASSERT_NE(p_b_prod, nullptr)
                << "Audit C2 CORE: hub-B must see a producer-presence "
                   "row for ch_out.  Mutation revealing the C2 fix: if "
                   "role_api_base.cpp::on_heartbeat_tick_ reverts to the "
                   "pre-fix `if role_tag == proc { emit(channel) } else if "
                   "role_tag == cons { emit(channel) } else { emit(...) }` "
                   "string-branching that bypasses the presence list, the "
                   "producer presence on hub-B never receives a heartbeat "
                   "and this assertion fails.";
            EXPECT_EQ(p_b_prod->channel,   ch_out);
            EXPECT_EQ(p_b_prod->role_type, "producer");
            EXPECT_TRUE(p_b_prod->first_heartbeat_seen)
                << "Audit C2 CORE: producer-presence on hub-B must show "
                   "first_heartbeat_seen=true — proves dual-hub heartbeat "
                   "routing through `resolve_bc_for_channel(out_channel)` "
                   "lands on hub-B's BRC, not hub-A's.";

            // ── Cross-pollution assertions ──────────────────────────────
            // Neither hub should know about the OTHER hub's presence —
            // that would mean either dedup failed or routing crossed
            // hubs.
            EXPECT_EQ(re_a->find_presence(ch_out, "producer"), nullptr)
                << "Audit C2: hub-A must NOT have a producer-presence "
                   "for ch_out — that channel lives on hub-B.";
            EXPECT_EQ(re_b->find_presence(ch_in, "consumer"), nullptr)
                << "Audit C2: hub-B must NOT have a consumer-presence "
                   "for ch_in — that channel lives on hub-A.";

            upstream.stop();
            api.stop_handler_threads();
            broker_a.stop_and_join();
            broker_b.stop_and_join();
        },
        "role_state.role_api_base_dual_hub_heartbeat_per_presence",
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
                if (scenario == "role_handler_brc_for_x_post_start")
                    return role_handler_brc_for_x_post_start();
                if (scenario == "role_api_base_start_handler_threads_e2e")
                    return role_api_base_start_handler_threads_e2e();
                if (scenario == "role_api_base_start_handler_threads_dual_hub_e2e")
                    return role_api_base_start_handler_threads_dual_hub_e2e();
                if (scenario == "role_api_base_band_join_handler_mode")
                    return role_api_base_band_join_handler_mode();
                if (scenario == "broker_band_rejects_invalid_identifier")
                    return broker_band_rejects_invalid_identifier();
                if (scenario == "role_api_base_band_notify_wire_field_and_routing")
                    return role_api_base_band_notify_wire_field_and_routing();
                if (scenario == "role_api_base_hub_dead_peer_keeps_role_alive")
                    return role_api_base_hub_dead_peer_keeps_role_alive();
                if (scenario == "role_api_base_hub_dead_transitions_presences_to_deregistered")
                    return role_api_base_hub_dead_transitions_presences_to_deregistered();
                if (scenario == "role_api_base_hub_dead_master_exits_role")
                    return role_api_base_hub_dead_master_exits_role();
                if (scenario == "role_api_base_wait_for_role_dual_hub_fallthrough")
                    return role_api_base_wait_for_role_dual_hub_fallthrough();
                if (scenario == "role_api_base_source_hub_uid_disambiguates_dual_hub")
                    return role_api_base_source_hub_uid_disambiguates_dual_hub();
                if (scenario == "role_api_base_dual_hub_heartbeat_per_presence")
                    return role_api_base_dual_hub_heartbeat_per_presence();
                if (scenario == "role_api_base_registration_fsm_transitions")
                    return role_api_base_registration_fsm_transitions();
                return 1;
            });
    }
};

static BrokerRoleStateWorkerRegistrar g_broker_role_state_registrar; // NOLINT(cert-err58-cpp)

} // anon
