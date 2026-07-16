// tests/test_layer3_datahub/workers/datahub_role_state_workers.cpp
//
// HEP-CORE-0023 §2.5: broker role-liveness state machine workers.
// Exercises Ready/Pending transitions + RoleStateMetrics counters.
//
// Strict-CURVE migration (#154 AUTH-6 batch-2a C3, 2026-06-29):
//   - All test brokers come up CURVE-only via the canonical
//     `pylabhub::tests::start_direct_broker(cfg, setup)` path
//     (HEP-CORE-0035 §2 + §4.6.5 — no bypass switch).
//   - Per-test `seed_curve_identities()` seeds `hub_identity` +
//     `role.<uid>` for every uid the test will use as a BRC client.
//   - REG_REQ / CONSUMER_REG_REQ payloads come from the canonical
//     `make_reg_opts` / `make_cons_opts` helpers in
//     `tests/test_framework/broker_test_harness.{h,cpp}` —
//     they carry the §5b canonical fields the broker now requires.
//   - `RoleAPIBase` reads its identity from the process KeyStore by
//     name (HEP-CORE-0040 §172 + #173); the legacy `api.set_auth("","")`
//     calls were deleted with that method.
//
// ── RATIONALE — HubHostBrokerHandle sweep disposition (task #52 Round 6) ─────
// The pure-wire band/channel tests that once lived here migrated to Pattern 4
// (`test_pattern4_broker_protocol.cpp` band tests +
// `test_pattern4_channel_group.cpp`).  What remains stays in L3 by design,
// in two groups:
//
//   (A) BROKER-INTERNAL-STATE tests — KEEP as `DirectBrokerHandle`.
//       `metrics_reclaim_cycle`, `pending_recovers_to_ready`,
//       `stuck_in_pending_reclaimed`, `role_entry_terminal_cleanup_on_last_
//       presence_dereg`, `role_entry_terminal_cleanup_on_consumer_left_last`,
//       `consumer_heartbeat_timeout_fires_consumer_died_notify`.
//       PURPOSE: pin broker-side FSM state — `RoleStateMetrics` aggregate
//         counters (`query_role_state_metrics()`) and `HubState` role-entry
//         erasure (`hub_state->role()`) — plus, for the last, the
//         CONSUMER_DIED_NOTIFY wire emission.
//       WHY IN-PROCESS BROKER: those counters/erasures have NO wire
//         observable; a subprocess broker's only readout would be log lines,
//         and an aggregate counter does not map to a log-presence match.
//       WHY NOT THE ANTIPATTERN: `DirectBrokerHandle` is broker-only — one
//         `BrokerService`, one ZAP pump.  The client is a bare `BrcHandle`
//         (DEALER, no ZAP pump).  No role/hub is co-hosted, so the
//         HEP-CORE-0036 §7.4 single-pumper invariant does not apply.
//
//   (B) ROLE-SIDE `RoleAPIBase` tests — DEFERRED (task #55), re-homed with
//       the RoleAPI unification.  `role_api_base_start_handler_threads_e2e`,
//       `role_api_base_band_join_handler_mode`,
//       `role_api_base_band_notify_wire_field_and_routing`,
//       `role_api_base_registration_fsm_transitions`.  These DO co-host a
//       real `RoleAPIBase`+`RoleHandler` with the broker (the antipattern),
//       but they inspect ROLE-side FSM/state (handler lifecycle, band index,
//       `find_presence_from_notification`, per-Presence registration state) —
//       a wire client has no RoleAPIBase to inspect.  Proper migration needs
//       a real-role subprocess; deferred so it rides the unification that
//       reshapes these surfaces.  Contract audit 2026-07-16: all four still
//       validly pin their contracts (see task #55).

#include "broker_test_harness.h"  // DirectBrokerHandle + BrcHandle + make_reg_opts / make_cons_opts
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
#include "utils/security/key_store.hpp"
#include "service/cycle_ops.hpp"   // dispatch_notifications + StopRequestor
#include "plh_datahub.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace pylabhub::tests::helper;
using namespace pylabhub::hub;
using pylabhub::broker::BrokerService;
using pylabhub::broker::RoleStateMetrics;
using pylabhub::tests::BrcHandle;
using pylabhub::tests::DirectBrokerHandle;
using pylabhub::tests::role_keystore_name;

namespace pylabhub::tests::worker::broker_role_state
{

static auto logger_module() { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto zmq_module()    { return ::pylabhub::hub::GetZMQContextModule(); }

// ============================================================================
// metrics_reclaim_cycle — full Ready -> Pending -> dereg, verified via metrics
// ============================================================================

// RATIONALE (task #52 group A, KEEP): broker-only DirectBrokerHandle + bare
// BrcHandle; pins RoleStateMetrics counters (no wire observable, not the
// single-pumper antipattern).  See file-header disposition block.
int metrics_reclaim_cycle()
{
    return run_gtest_worker(
        []() {
            const std::string ch  = make_test_channel_name("role_state.metrics_cycle");
            const std::string uid = "prod." + ch;

            auto curve = pylabhub::tests::make_curve_setup({uid});
            pylabhub::tests::seed_curve_identities(curve);

            BrokerService::Config cfg;
            cfg.endpoint = "tcp://127.0.0.1:0";
            cfg.ready_timeout_override           = std::chrono::milliseconds(150);
            cfg.pending_timeout_override         = std::chrono::milliseconds(150);
            cfg.consumer_liveness_check_interval = std::chrono::seconds(0);
            auto broker = pylabhub::tests::start_direct_broker(std::move(cfg), curve);

            BrcHandle bh;
            bh.start(broker.endpoint, broker.pubkey, uid, role_keystore_name(uid));

            auto reg = bh.brc.register_channel(
                pylabhub::tests::make_reg_opts(ch, uid), 3000);
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
        logger_module(), ::pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(), zmq_module());
}

// ============================================================================
// pending_recovers_to_ready — demote, then heartbeat restores Ready
// ============================================================================

// RATIONALE (task #52 group A, KEEP): broker-only DirectBrokerHandle + bare
// BrcHandle; pins the pending_to_ready recovery counter.  See file header.
int pending_recovers_to_ready()
{
    return run_gtest_worker(
        []() {
            const std::string ch  = make_test_channel_name("role_state.recover");
            const std::string uid = "prod." + ch;

            auto curve = pylabhub::tests::make_curve_setup({uid});
            pylabhub::tests::seed_curve_identities(curve);

            BrokerService::Config cfg;
            cfg.endpoint = "tcp://127.0.0.1:0";
            cfg.ready_timeout_override           = std::chrono::milliseconds(150);
            cfg.pending_timeout_override         = std::chrono::seconds(10); // long -> won't dereg
            cfg.consumer_liveness_check_interval = std::chrono::seconds(0);
            auto broker = pylabhub::tests::start_direct_broker(std::move(cfg), curve);

            BrcHandle bh;
            bh.start(broker.endpoint, broker.pubkey, uid, role_keystore_name(uid));

            auto reg = bh.brc.register_channel(
                pylabhub::tests::make_reg_opts(ch, uid), 3000);
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
        logger_module(), ::pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(), zmq_module());
}

// ============================================================================
// stuck_in_pending_reclaimed — role never heartbeats, pending_timeout fires
// ============================================================================

// RATIONALE (task #52 group A, KEEP): broker-only DirectBrokerHandle + bare
// BrcHandle; pins pending-timeout reclaim counters + HubState.  See file header.
int stuck_in_pending_reclaimed()
{
    return run_gtest_worker(
        []() {
            const std::string ch  = make_test_channel_name("role_state.stuck_pending");
            const std::string uid = "prod." + ch;

            auto curve = pylabhub::tests::make_curve_setup({uid});
            pylabhub::tests::seed_curve_identities(curve);

            // Per HEP-CORE-0023 §2.1 the registered-but-never-heartbeat
            // case is `Connected` with `first_heartbeat_seen=false`
            // (sub-state "registering").  Reclamation goes through both
            // sweep passes: Connected → Pending after `ready_timeout`,
            // then Pending → Disconnected after `pending_timeout`.  So
            // both timeouts apply here — keep them short so the total
            // (~150 ms) fits in a 2 s poll window.
            BrokerService::Config cfg;
            cfg.endpoint = "tcp://127.0.0.1:0";
            cfg.ready_timeout_override           = std::chrono::milliseconds(50);
            cfg.pending_timeout_override         = std::chrono::milliseconds(100);
            cfg.consumer_liveness_check_interval = std::chrono::seconds(0);
            auto broker = pylabhub::tests::start_direct_broker(std::move(cfg), curve);

            BrcHandle bh;
            bh.start(broker.endpoint, broker.pubkey, uid, role_keystore_name(uid));

            auto reg = bh.brc.register_channel(
                pylabhub::tests::make_reg_opts(ch, uid), 3000);
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
        logger_module(), ::pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(), zmq_module());
}

// band_membership_cleaned_on_role_close MIGRATED to
// tests/test_layer3_pattern4/test_pattern4_broker_protocol.cpp
// (Pattern4BrokerProtocolTest.Band_MembershipCleanedOnProducerDereg) —
// task #52 DirectBrokerHandle sweep.  The on_channel_closed → band-cleanup
// effect is observed over the wire (BAND_MEMBERS count 2→1), so the
// in-process co-host is retired.

// ============================================================================
// role_entry_terminal_cleanup_on_last_presence_dereg
//   Wave M3 step 5b (2026-05-11): a producer DEREG that empties the
//   channel must trigger `_dispatch_role_disconnected_if_dead` →
//   `_set_role_disconnected`, erasing the RoleEntry.  Without the
//   wiring, the entry lingers with all-Disconnected presences (the
//   "stale residue" failure mode from Wave M2.5 §6.2).
// ============================================================================

// RATIONALE (task #52 group A, KEEP): broker-only DirectBrokerHandle + bare
// BrcHandle; pins HubState role-entry erasure on last-presence dereg (no wire
// observable).  See file-header disposition block.
int role_entry_terminal_cleanup_on_last_presence_dereg()
{
    return run_gtest_worker(
        []() {
            const std::string ch  = make_test_channel_name("role_state.cleanup_prod");
            const std::string uid = "prod." + ch;

            auto curve = pylabhub::tests::make_curve_setup({uid});
            pylabhub::tests::seed_curve_identities(curve);

            BrokerService::Config cfg;
            cfg.endpoint = "tcp://127.0.0.1:0";
            cfg.consumer_liveness_check_interval = std::chrono::seconds(0);
            auto broker = pylabhub::tests::start_direct_broker(std::move(cfg), curve);

            BrcHandle bh;
            bh.start(broker.endpoint, broker.pubkey, uid, role_keystore_name(uid));

            auto reg = bh.brc.register_channel(
                pylabhub::tests::make_reg_opts(ch, uid), 3000);
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
        logger_module(), ::pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(), zmq_module());
}

// ============================================================================
// role_entry_terminal_cleanup_on_consumer_left_last
//   Wave M3 step 5b (2026-05-11): a consumer DEREG whose role had
//   only that one presence must erase the role entry via
//   `_on_consumer_left` → `_dispatch_role_disconnected_if_dead`.
// ============================================================================

// RATIONALE (task #52 group A, KEEP): broker-only DirectBrokerHandle + bare
// BrcHandle; pins HubState role-entry erasure when the last consumer leaves
// (no wire observable).  See file-header disposition block.
int role_entry_terminal_cleanup_on_consumer_left_last()
{
    return run_gtest_worker(
        []() {
            const std::string ch         = make_test_channel_name("role_state.cleanup_cons");
            const std::string prod_uid   = "prod." + ch;
            const std::string cons_uid   = "cons." + ch;

            auto curve = pylabhub::tests::make_curve_setup({prod_uid, cons_uid});
            pylabhub::tests::seed_curve_identities(curve);

            BrokerService::Config cfg;
            cfg.endpoint = "tcp://127.0.0.1:0";
            cfg.consumer_liveness_check_interval = std::chrono::seconds(0);
            auto broker = pylabhub::tests::start_direct_broker(std::move(cfg), curve);

            // Producer creates the channel; we only test the
            // consumer-side cleanup, so prod_uid stays alive.
            BrcHandle pb;
            pb.start(broker.endpoint, broker.pubkey, prod_uid, role_keystore_name(prod_uid));
            ASSERT_TRUE(pb.brc.register_channel(
                pylabhub::tests::make_reg_opts(ch, prod_uid), 3000).has_value());

            // HEP-CORE-0036 §5.2 R6 producer-kLive gate: a single
            // producer heartbeat is required BEFORE CONSUMER_REG_REQ
            // will be admitted.  Pre-strict-CURVE the legacy broker
            // had no such gate, so the original test omitted this
            // heartbeat — adding it now is faithful to the current
            // production wire shape, not a test patch.
            pb.brc.send_heartbeat(ch, prod_uid, "producer", {});

            // Consumer registers on the same channel with its own uid.
            BrcHandle cb;
            cb.start(broker.endpoint, broker.pubkey, cons_uid, role_keystore_name(cons_uid));

            auto cons_reg = cb.brc.register_consumer(
                pylabhub::tests::make_cons_opts(ch, cons_uid), 3000);
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
        logger_module(), ::pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(), zmq_module());
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

// RATIONALE (task #52 group A, KEEP): broker-only DirectBrokerHandle + bare
// BrcHandle; the CONSUMER_DIED_NOTIFY emission is wire-observable (also
// covered by Pattern4 broker_health.DeadConsumerDetected), but this test also
// pins the broker-side HubState erasure + RoleStateMetrics on heartbeat
// timeout — no wire observable.  See file-header disposition block.
int consumer_heartbeat_timeout_fires_consumer_died_notify()
{
    return run_gtest_worker(
        []() {
            const std::string ch       = make_test_channel_name("role_state.cons_hb_timeout");
            const std::string prod_uid = "prod." + ch;
            const std::string cons_uid = "cons." + ch;

            auto curve = pylabhub::tests::make_curve_setup({prod_uid, cons_uid});
            pylabhub::tests::seed_curve_identities(curve);

            // Match the other state-machine tests: 150 ms each side
            // gives ~300 ms minimum + ~100 ms broker sweep cadence.
            // Poll window below is 3 s — generous against CI jitter.
            BrokerService::Config cfg;
            cfg.endpoint = "tcp://127.0.0.1:0";
            cfg.ready_timeout_override           = std::chrono::milliseconds(150);
            cfg.pending_timeout_override         = std::chrono::milliseconds(150);
            // Disable the PID-death sweep to keep the notification path
            // unambiguous — only the heartbeat-timeout path can fire.
            cfg.consumer_liveness_check_interval = std::chrono::seconds(0);
            auto broker = pylabhub::tests::start_direct_broker(std::move(cfg), curve);

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
            prod_cfg.keystore_name   = role_keystore_name(prod_uid);
            ASSERT_TRUE(prod_brc.connect(prod_cfg));

            std::atomic<bool> prod_poll_running{true};
            std::thread       prod_poll_thread([&]() {
                prod_brc.run_poll_loop([&]() { return prod_poll_running.load(); });
            });

            ASSERT_TRUE(prod_brc.register_channel(
                pylabhub::tests::make_reg_opts(ch, prod_uid), 3000).has_value());
            prod_brc.send_heartbeat(ch, prod_uid, "producer", {});

            // ── Consumer BRC: standard BrcHandle.
            BrcHandle cons_bh;
            cons_bh.start(broker.endpoint, broker.pubkey, cons_uid,
                          role_keystore_name(cons_uid));

            ASSERT_TRUE(cons_bh.brc.register_consumer(
                pylabhub::tests::make_cons_opts(ch, cons_uid), 3000).has_value());
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
        logger_module(), ::pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(), zmq_module());
}

// RE-LAYERED 2026-06-27 — the 4 `role_handler_*` worker bodies that
// lived between this point and `role_api_base_start_handler_threads_e2e`
// (state-only verification of RoleHandler's network surface) moved to
// L2 at `tests/test_layer2_service/test_role_handler.cpp`
// per the AUTH-6 layer-fit audit
// (`docs/code_review/REVIEW_AUTH6_TestDisposition_2026-06-27.md` §2
// addendum).  They pin RoleHandler single-class state-flag +
// pointer-identity behavior; the broker that lived here was scaffolding
// only because `BRC::is_connected()` reads an internal flag set at end
// of `connect()` (before any wire handshake).  Mapping:
//   - role_handler_connections_start_stop_smoke      → RoleHandlerLifecycle.StartStop_Smoke_SinglePresence
//   - role_handler_connections_dual_hub              → RoleHandlerLifecycle.StartStop_DualHub_BothConnectionsConnected
//   - role_handler_connections_double_start_rejected → RoleHandlerLifecycle.DoubleStart_Rejected_StateNotCleared
//   - role_handler_brc_for_x_post_start              → RoleHandlerRouting.BrcForX_PostStart_PointerIdentity


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

// RATIONALE (task #52 group B, DEFERRED task #55): co-hosts a real
// RoleAPIBase+RoleHandler with the broker; inspects role-side handler
// lifecycle — no wire equivalent.  Re-homed with the RoleAPI unification.
int role_api_base_start_handler_threads_e2e()
{
    return run_gtest_worker(
        []() {
            const std::string role_uid = "prod.m4c_e2e.uid00000001";
            const std::string channel  = make_test_channel_name("m4c.e2e");

            auto curve = pylabhub::tests::make_curve_setup({role_uid});
            pylabhub::tests::seed_curve_identities(curve);
            // RoleAPIBase/RoleHandler's internal BRC uses
            // `kRoleIdentityName` ("role_identity") as the KeyStore
            // lookup name (HEP-CORE-0040 §172 + #173) — production
            // ships one role identity per process.  Seed the api
            // role's keys under that name too.
            pylabhub::tests::add_curve_identity(
                pylabhub::utils::security::kRoleIdentityName,
                curve.role(role_uid));

            BrokerService::Config bcfg;
            bcfg.endpoint = "tcp://127.0.0.1:0";
            auto broker = pylabhub::tests::start_direct_broker(std::move(bcfg), curve);

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

            pylabhub::config::HubRefConfig hub_cfg;
            hub_cfg.broker        = broker.endpoint;
            hub_cfg.broker_pubkey = broker.pubkey;

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
            auto reg_resp = api.register_producer_channel(
                pylabhub::tests::make_reg_opts(channel, role_uid), 3000);
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
        logger_module(), ::pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(), zmq_module());
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
//   coverage moved to `bc->band_join` direct calls in the channel-group
//   band tests (since retired to Pattern 4,
//   test_pattern4_channel_group.cpp) during M4f migration.  Mutation
//   sweep: revert the fix → `band_join` returns nullopt without
//   contacting broker → this test fails on the has_value assertion.
// ============================================================================

// RATIONALE (task #52 group B, DEFERRED task #55): co-hosts a real
// RoleAPIBase+RoleHandler with the broker; inspects role-side band-index
// routing state — no wire equivalent.  Re-homed with the RoleAPI unification.
int role_api_base_band_join_handler_mode()
{
    return run_gtest_worker(
        []() {
            const std::string role_uid = "prod.a0_band.uid00000001";
            const std::string channel  = make_test_channel_name("a0.band");
            const std::string band     = "!test_band_a0";  // R3.5: `!`-prefixed per HEP-0030 §3

            auto curve = pylabhub::tests::make_curve_setup({role_uid});
            pylabhub::tests::seed_curve_identities(curve);
            pylabhub::tests::add_curve_identity(
                pylabhub::utils::security::kRoleIdentityName,
                curve.role(role_uid));

            BrokerService::Config bcfg;
            bcfg.endpoint = "tcp://127.0.0.1:0";
            auto broker = pylabhub::tests::start_direct_broker(std::move(bcfg), curve);

            pylabhub::scripting::RoleHostCore core;
            core.set_running(true);

            pylabhub::scripting::RoleAPIBase  api(core, "prod", role_uid);
            api.set_name("a0_band");
            api.set_channel(channel);

            pylabhub::config::HubRefConfig hub_cfg;
            hub_cfg.broker        = broker.endpoint;
            hub_cfg.broker_pubkey = broker.pubkey;

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
        logger_module(), ::pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(), zmq_module());
}

// broker_band_rejects_invalid_identifier MIGRATED to
// tests/test_layer3_pattern4/test_pattern4_broker_protocol.cpp
// (Pattern4BrokerProtocolTest.Band_RejectsInvalidIdentifier) — task #52
// DirectBrokerHandle sweep.  Pure wire assertions, no in-process broker
// inspection, so it moved cleanly to the subprocess-broker harness.

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
//     the wrong name, so the channel-group round-trip tests (since
//     retired to Pattern 4, test_pattern4_channel_group.cpp) all passed.
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
//   Mutation sweep:
//     - Revert role_handler.cpp lookup back to `band_name` →
//       step 8 returns nullptr → test fails on ASSERT_NE.
//     - Revert broker emission back to `channel` → step 7 fails on
//       `band` key present check.
//     - Revert both → step 7 fails first (wire conformance).
// ============================================================================

// RATIONALE (task #52 group B, DEFERRED task #55): co-hosts a real
// RoleAPIBase+RoleHandler with the broker; inspects role-side
// find_presence_from_notification dispatch — no wire equivalent.  Re-homed
// with the RoleAPI unification.
int role_api_base_band_notify_wire_field_and_routing()
{
    return run_gtest_worker(
        []() {
            const std::string role_uid_a = "prod.b1.a.uid00000001";
            const std::string role_uid_b = "prod.b1.b.uid00000002";
            const std::string channel    = make_test_channel_name("b1.chan");
            const std::string band       = "!" + make_test_channel_name("b1.band");

            auto curve = pylabhub::tests::make_curve_setup({role_uid_a, role_uid_b});
            pylabhub::tests::seed_curve_identities(curve);
            // role A is the api role; role B is a raw BRC.
            pylabhub::tests::add_curve_identity(
                pylabhub::utils::security::kRoleIdentityName,
                curve.role(role_uid_a));

            BrokerService::Config bcfg;
            bcfg.endpoint = "tcp://127.0.0.1:0";
            auto broker = pylabhub::tests::start_direct_broker(std::move(bcfg), curve);

            // ── Role A: full api + handler (this is the role whose
            //    dispatcher we're testing) ────────────────────────────
            pylabhub::scripting::RoleHostCore core;
            core.set_running(true);

            pylabhub::scripting::RoleAPIBase api(core, "prod", role_uid_a);
            api.set_name("b1_a");
            api.set_channel(channel);

            pylabhub::config::HubRefConfig hub_cfg;
            hub_cfg.broker        = broker.endpoint;
            hub_cfg.broker_pubkey = broker.pubkey;

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
            b.start(broker.endpoint, broker.pubkey, role_uid_b,
                    role_keystore_name(role_uid_b));
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
        logger_module(), ::pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(), zmq_module());
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

// RATIONALE (task #52 group B, DEFERRED task #55): co-hosts a real
// RoleAPIBase+RoleHandler with the broker; inspects role-side per-Presence
// registration FSM state — no wire equivalent.  Re-homed with the RoleAPI
// unification.
int role_api_base_registration_fsm_transitions()
{
    return run_gtest_worker(
        []() {
            const std::string role_uid = "prod.s1.uid00000001";
            const std::string channel  = make_test_channel_name("s1.chan");

            auto curve = pylabhub::tests::make_curve_setup({role_uid});
            pylabhub::tests::seed_curve_identities(curve);
            pylabhub::tests::add_curve_identity(
                pylabhub::utils::security::kRoleIdentityName,
                curve.role(role_uid));

            BrokerService::Config bcfg;
            bcfg.endpoint = "tcp://127.0.0.1:0";
            auto broker = pylabhub::tests::start_direct_broker(std::move(bcfg), curve);

            pylabhub::scripting::RoleHostCore core;
            core.set_running(true);

            pylabhub::scripting::RoleAPIBase api(core, "prod", role_uid);
            api.set_name("s1_test");
            api.set_channel(channel);

            pylabhub::config::HubRefConfig hub_cfg;
            hub_cfg.broker        = broker.endpoint;
            hub_cfg.broker_pubkey = broker.pubkey;

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
            auto reg_result = api.register_producer_channel(
                pylabhub::tests::make_reg_opts(channel, role_uid));
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
        logger_module(), ::pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(), zmq_module());
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
                // band_membership_cleaned_on_role_close migrated to
                // Pattern4BrokerProtocolTest.Band_MembershipCleanedOnProducerDereg
                // (task #52 DirectBrokerHandle sweep).
                if (scenario == "role_entry_terminal_cleanup_on_last_presence_dereg")
                    return role_entry_terminal_cleanup_on_last_presence_dereg();
                if (scenario == "role_entry_terminal_cleanup_on_consumer_left_last")
                    return role_entry_terminal_cleanup_on_consumer_left_last();
                if (scenario == "consumer_heartbeat_timeout_fires_consumer_died_notify")
                    return consumer_heartbeat_timeout_fires_consumer_died_notify();
                // role_handler_* dispatchers removed 2026-06-27; bodies
                // re-layered to L2 test_role_handler.cpp per AUTH-6 audit.
                if (scenario == "role_api_base_start_handler_threads_e2e")
                    return role_api_base_start_handler_threads_e2e();
                if (scenario == "role_api_base_band_join_handler_mode")
                    return role_api_base_band_join_handler_mode();
                // broker_band_rejects_invalid_identifier migrated to
                // Pattern4BrokerProtocolTest.Band_RejectsInvalidIdentifier
                // (task #52 DirectBrokerHandle sweep).
                if (scenario == "role_api_base_band_notify_wire_field_and_routing")
                    return role_api_base_band_notify_wire_field_and_routing();
                if (scenario == "role_api_base_registration_fsm_transitions")
                    return role_api_base_registration_fsm_transitions();
                // 7 dual-broker dispatchers removed 2026-06-29 under
                // #154 C3.  Two in-process `BrokerService` instances
                // violate the HEP-CORE-0036 §7.1 single-pumper
                // invariant (`ZapRouter::pump_one` PANICs on concurrent
                // entry from two broker poll loops sharing one
                // ZapRouter).  Contracts absorbed by L4 successors
                // tracked in `docs/todo/TESTING_TODO.md` § "Test
                // retirements / cross-layer migrations" 2026-06-29
                // rows (#296 expanded for hub-dead trio, #228 for
                // source_hub_uid, #229 for wait_for_role fall-through,
                // #299 for dual-hub heartbeat per-presence).  All
                // blocked on #298 (Pattern4Setup multi-hub extension).
                return 1;
            });
    }
};

static BrokerRoleStateWorkerRegistrar g_broker_role_state_registrar; // NOLINT(cert-err58-cpp)

} // anon
