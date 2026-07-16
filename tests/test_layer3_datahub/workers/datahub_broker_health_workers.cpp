// tests/test_layer3_datahub/workers/datahub_broker_health_workers.cpp
//
// Broker health and notification tests via BrokerRequestComm.
//
// Strict-CURVE migration (#154 AUTH-6 batch-2a C4, 2026-06-30):
//   - All test brokers come up CURVE-only via the canonical
//     `pylabhub::tests::start_hubhost_broker` (production assembly with
//     real HubConfig) or `start_direct_broker` (when a test needs
//     direct BrokerService::Config timing overrides) per HEP-CORE-0035
//     §2 + §4.6.5.  No bypass switches.
//   - Per-test `seed_curve_identities()` seeds `hub_identity` + every
//     `role.<uid>` the test exercises as a BRC client.
//   - REG_REQ / CONSUMER_REG_REQ payloads come from the canonical
//     `make_reg_opts` / `make_cons_opts` helpers in
//     `tests/test_framework/broker_test_harness.{h,cpp}` — they carry
//     the §5b canonical fields the broker now requires (#290).
//   - The legacy `BrokerHandle` / `start_broker_with_cfg` / `start_broker`
//     local helpers are gone; the legacy `BrcHandle::start(ep,pk,uid)`
//     3-arg shape is replaced by the canonical 4-arg form including
//     `role_keystore_name(uid)` (HEP-CORE-0040 §172).
//   - `ctrl_zap_deny_path` reworked to seed an unknown-but-real CURVE
//     keypair on the test side WITHOUT pushing it to `known_roles` —
//     the broker's empty allowlist then denies that pubkey, which is
//     what the test pins.
//   - `dead_consumer_*` two-subprocess test extends its temp-file
//     handoff to ALSO carry the consumer's Z85 keypair so the exiter
//     subprocess can reconstruct its own `seed_curve_identities()` (each
//     subprocess has its own SecureSubsystem + KeyStore per
//     HEP-CORE-0040 §4.5 + §5.1).
//
// ── HubHostBrokerHandle sweep disposition (task #52 Round 3) ─────────────────
// Migrated to Pattern 4 (`test_pattern4_broker_health.cpp`):
// ProducerGetsClosingNotify, ProducerAutoDeregisters, SchemaMismatchNotify,
// DeadConsumerDetected, ConsumerHeartbeatTimeout_NotifyBodyShape.  Of what
// remains, two are genuine KEEPs and two are MIGRATION CANDIDATES:
//
//   KEEP — RATIONALE (broker-internal state, no wire/log observable):
//     · ctrl_zap_deny_path — reads the process-singleton
//       `ZapRouter::instance().denied_count()` (before/after) to prove a REG
//       failure came through the ZAP deny gate, not another timeout path.  An
//       aggregate counter with no ACK field; the DirectBrokerHandle here is
//       broker-only (one ZAP pump) driving a raw BrokerRequestComm.
//     · multi_producer_partial_pending_timeout — reads
//       `query_channel_snapshot().channels[].producer_uids.size()` to prove a
//       SILENT presence-timeout removal (B) leaves the channel alive for the
//       survivor (A).  A silent timeout emits no ACK; the count lives only in
//       the in-process snapshot.  (Its paired negative — A gets no
//       CHANNEL_CLOSING_NOTIFY — is wire and already asserted.)
//
//   MIGRATION CANDIDATE (NOT a keep — do not RATIONALE; Round-6b follow-up):
//     · two_snapshot_invariant — observes ONLY the CHANNEL_CLOSING_NOTIFY wire
//       message and its timing (fires ≥800ms after last heartbeat, not at
//       ~500ms).  The sole in-process need is setting ready/pending timeout
//       knobs — which Pattern 4 already does via named broker-config profiles.
//     · consumer_auto_deregisters — the `consumer_count==0` snapshot read is
//       redundant with the already-asserted CONSUMER_DEREG_ACK and is
//       wire-observable via CHANNEL_LIST_REQ (the path list_channels_* now
//       use).  Migratable with no new broker observability.
//   Tracked in docs/todo/TESTING_TODO.md Round 6b.

#include "datahub_broker_health_workers.h"
#include "broker_test_harness.h"  // HubHostBrokerHandle + DirectBrokerHandle + BrcHandle + make_reg_opts/make_cons_opts
#include "test_entrypoint.h"
#include "test_process_utils.h"
#include "shared_test_helpers.h"

#include "utils/broker_request_comm.hpp"
#include "utils/broker_service.hpp"
#include "utils/file_lock.hpp"
#include "utils/hub_state.hpp"
#include "utils/json_config.hpp"
#include "utils/security/key_store.hpp"
#include "utils/security/secure_subsystem.hpp"
#include "utils/security/zap_router.hpp"  // HEP-CORE-0035 §4.2 deny-path observability
#include "plh_datahub.hpp"

#include <zmq.h>  // zmq_curve_keypair for the deny-path test

#include <gtest/gtest.h>
#include <fmt/core.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <thread>

using namespace pylabhub::tests::helper;
using namespace pylabhub::hub;
using pylabhub::broker::BrokerService;
using pylabhub::broker::ChannelSnapshot;
using pylabhub::tests::BrcHandle;
using pylabhub::tests::CurveKeypair;
using pylabhub::tests::CurveSetup;
using pylabhub::tests::DirectBrokerHandle;
using pylabhub::tests::HubHostBrokerHandle;
using pylabhub::tests::role_keystore_name;

namespace pylabhub::tests::worker::broker_health
{

static auto logger_module()    { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto file_lock_module() { return ::pylabhub::utils::FileLock::GetLifecycleModule(); }
static auto json_module()      { return ::pylabhub::utils::JsonConfig::GetLifecycleModule(); }
static auto zmq_module()       { return ::pylabhub::hub::GetZMQContextModule(); }

namespace
{

// Hub-config overrides that map the legacy `BrokerService::Config`
// timing knobs into the HubConfig JSON.  Most tests use defaults; tests
// that need custom heartbeat / pending-timeout values pass them via
// `j_overrides` to `start_hubhost_broker`.  When a test needs the
// direct (non-HubHost) broker (e.g. `ctrl_zap_deny_path`), it builds
// a `BrokerService::Config` directly and uses `start_direct_broker`.

// Baseline hub.json overrides for the L3 health tests.  Mirrors the
// shape used by `broker_consumer_workers.cpp::hubhost_overrides()`:
// disable admin (no vault-side admin_token in this fixture) + disable
// script (no plh_pyenv setup).  Tests with timing requirements layer
// `ready_timeout_ms` / `pending_timeout_ms` on top via the helper
// below.
nlohmann::json hub_overrides_baseline()
{
    nlohmann::json j;
    j["network"]["broker_endpoint"] = "tcp://127.0.0.1:0";
    j["admin"]["enabled"]           = false;
    j["script"]["path"]             = "";
    return j;
}

nlohmann::json hub_overrides_with_timeouts(int ready_timeout_ms,
                                            int pending_timeout_ms)
{
    nlohmann::json j = hub_overrides_baseline();
    j["broker"]["ready_timeout_ms"]   = ready_timeout_ms;
    j["broker"]["pending_timeout_ms"] = pending_timeout_ms;
    // NOTE on `consumer_liveness_check_interval`: this knob lives on
    // `BrokerService::Config` (`broker_service.hpp:189`) but is NOT
    // exposed through `HubBrokerConfig` — there is no hub.json
    // counterpart.  Tests that need a specific value (e.g. =0 to
    // disable the PID-death path while pinning the heartbeat-timeout
    // path, or =1s to detect a dead PID quickly) must use
    // `start_direct_broker` instead of HubHost so they can set the
    // field directly on the BrokerService::Config.  Tests that don't
    // depend on it use the default (5s); since their assertion
    // windows are short (<5s), the PID-death sweep doesn't fire.
    return j;
}

// start_health_direct_broker (consumer_liveness_check_interval wrapper)
// removed with its only callers — dead_consumer_* + consumer_heartbeat_
// timeout_notify — when they migrated to Pattern 4 (task #52 Round 2).
// The surviving direct-broker test (ctrl_zap_deny_path) builds its own
// BrokerService::Config inline.

} // anon

// producer_gets_closing_notify MIGRATED to tests/test_layer3_pattern4/
// test_pattern4_broker_health.cpp (task #52 Round 2, "fast_reclaim" profile).

// ============================================================================
// consumer_auto_deregisters — Consumer::close() → CONSUMER_DEREG_REQ
// ============================================================================

// MIGRATION CANDIDATE (task #52 Round 6b, NOT a keep): consumer_count==0
// snapshot read is redundant with CONSUMER_DEREG_ACK + wire-observable via
// CHANNEL_LIST_REQ.  See file-header disposition block.
int consumer_auto_deregisters(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            const std::string ch_name  = make_test_channel_name("health.consumer_dereg");
            const std::string prod_uid = "prod." + ch_name;
            const std::string cons_uid = "cons." + ch_name;

            auto curve = pylabhub::tests::make_curve_setup({prod_uid, cons_uid});
            pylabhub::tests::seed_curve_identities(curve);

            auto broker = pylabhub::tests::start_hubhost_broker(
                hub_overrides_baseline(), curve, "HealthConsumerDeregHub");

            BrcHandle prod_bh;
            prod_bh.start(broker.endpoint, broker.pubkey, prod_uid,
                          role_keystore_name(prod_uid));
            auto reg = prod_bh.brc.register_channel(
                pylabhub::tests::make_reg_opts(ch_name, prod_uid), 3000);
            ASSERT_TRUE(reg.has_value());

            // HEP-CORE-0036 §5.2 R6: producer must hit kLive before
            // CONSUMER_REG_REQ is admitted.
            prod_bh.brc.send_heartbeat(ch_name, prod_uid, "producer", {});
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            BrcHandle cons_bh;
            cons_bh.start(broker.endpoint, broker.pubkey, cons_uid,
                          role_keystore_name(cons_uid));
            auto cons_reg = cons_bh.brc.register_consumer(
                pylabhub::tests::make_cons_opts(ch_name, cons_uid), 3000);
            ASSERT_TRUE(cons_reg.has_value());

            // Consumer voluntarily deregisters.
            {
                auto dereg = cons_bh.brc.deregister_consumer(ch_name);
                ASSERT_TRUE(dereg.has_value());
                EXPECT_EQ(dereg->value("status", std::string{}), "success");
            }

            // Wait for broker to reflect consumer_count=0.
            const auto consumer_count_for = [&](const std::string &name) -> int {
                ChannelSnapshot snap = broker.service().query_channel_snapshot();
                for (const auto &ch : snap.channels)
                    if (ch.name == name) return ch.consumer_count;
                return -1;
            };
            const auto cdeadline =
                std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (consumer_count_for(ch_name) != 0 &&
                   std::chrono::steady_clock::now() < cdeadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            EXPECT_EQ(consumer_count_for(ch_name), 0)
                << "broker did not reflect consumer_count=0 within 2s "
                   "after deregister_consumer";

            cons_bh.stop();
            prod_bh.stop();
            broker.stop_and_join();
        },
        "broker_health.consumer_auto_deregisters",
        logger_module(), file_lock_module(), json_module(),
        ::pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(), zmq_module());
}

// producer_auto_deregisters MIGRATED to tests/test_layer3_pattern4/
// test_pattern4_broker_health.cpp (task #52 Round 2, "long_reclaim" profile).

// dead_consumer_orchestrator + dead_consumer_exiter MIGRATED to
// tests/test_layer3_pattern4/test_pattern4_broker_health.cpp
// (DeadConsumerDetected: a "pattern4_broker_protocol.dying_consumer"
// subprocess registers then std::_Exit(0); broker "dead_consumer" profile
// detects the dead PID).  consumer_heartbeat_timeout_notify MIGRATED
// likewise (ConsumerHeartbeatTimeout, "consumer_timeout" profile).

// schema_mismatch_notify MIGRATED to tests/test_layer3_pattern4/
// test_pattern4_broker_health.cpp (task #52 Round 2).

// ============================================================================
// HEP-CORE-0039 P8 prerequisites — heartbeat-timeout sweep invariants
// ============================================================================

// Multi-producer partial pending-timeout: one producer stops heartbeating,
// the OTHER keeps heartbeating; that producer's presence times out and is
// removed but the channel SURVIVES (no last-producer atomic teardown).
// RATIONALE (task #52 Round 3, KEEP): reads query_channel_snapshot()
// producer_uids.size() to prove a SILENT presence-timeout removal (no ACK).
// See file-header disposition block.
int multi_producer_partial_pending_timeout(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            const std::string ch_name =
                make_test_channel_name("health.multi_prod_partial_timeout");
            const std::string prod_a_uid = "prod.a." + ch_name;
            const std::string prod_b_uid = "prod.b." + ch_name;

            auto curve = pylabhub::tests::make_curve_setup({prod_a_uid, prod_b_uid});
            pylabhub::tests::seed_curve_identities(curve);

            auto broker = pylabhub::tests::start_hubhost_broker(
                hub_overrides_with_timeouts(/*ready=*/500, /*pending=*/500),
                curve, "HealthMultiProdPartialHub");

            // Producer A: the survivor.
            std::atomic<bool> a_got_closing{false};
            BrcHandle prod_a;
            prod_a.brc.on_notification(
                [&](const std::string &type, const nlohmann::json &) {
                    if (type == "CHANNEL_CLOSING_NOTIFY")
                        a_got_closing.store(true);
                });
            prod_a.start(broker.endpoint, broker.pubkey, prod_a_uid,
                         role_keystore_name(prod_a_uid));
            auto reg_a = prod_a.brc.register_channel(
                pylabhub::tests::make_reg_opts(ch_name, prod_a_uid), 3000);
            ASSERT_TRUE(reg_a.has_value()) << "register A failed";

            // Producer B: times out after one heartbeat.
            BrcHandle prod_b;
            prod_b.start(broker.endpoint, broker.pubkey, prod_b_uid,
                         role_keystore_name(prod_b_uid));
            auto reg_b = prod_b.brc.register_channel(
                pylabhub::tests::make_reg_opts(ch_name, prod_b_uid), 3000);
            ASSERT_TRUE(reg_b.has_value()) << "register B failed";

            prod_a.brc.send_heartbeat(ch_name, prod_a_uid, "producer", {});
            prod_b.brc.send_heartbeat(ch_name, prod_b_uid, "producer", {});
            std::this_thread::sleep_for(std::chrono::milliseconds(50));

            // Survivor heartbeats every 100ms; B never again.
            std::atomic<bool> a_stop{false};
            std::thread a_thread([&] {
                while (!a_stop.load()) {
                    prod_a.brc.send_heartbeat(ch_name, prod_a_uid, "producer", {});
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            });

            const auto producer_count_for =
                [&](const std::string &name) -> int {
                ChannelSnapshot snap =
                    broker.service().query_channel_snapshot();
                for (const auto &ch : snap.channels)
                    if (ch.name == name)
                        return static_cast<int>(ch.producer_uids.size());
                return -1;
            };

            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(5);
            while (producer_count_for(ch_name) != 1 &&
                   std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            EXPECT_EQ(producer_count_for(ch_name), 1)
                << "B's presence must be removed; survivor A remains";
            EXPECT_FALSE(a_got_closing.load())
                << "Channel must SURVIVE — surviving producer A must NOT "
                   "receive CHANNEL_CLOSING_NOTIFY when B times out";

            a_stop.store(true);
            a_thread.join();
            prod_a.stop();
            prod_b.stop();
            broker.stop_and_join();
        },
        "broker_health.multi_producer_partial_pending_timeout",
        logger_module(), file_lock_module(), json_module(),
        ::pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(), zmq_module());
}


// Two-snapshot invariant: a presence that demotes Connected→Pending in
// tick T MUST NOT also be terminated (Pending→Disconnected) in the same
// tick T.  Timing pin: CHANNEL_CLOSING_NOTIFY must fire AFTER
// (ready_timeout + pending_timeout - sweep slop) ≈ 800ms.
// MIGRATION CANDIDATE (task #52 Round 6b, NOT a keep): observes only the
// CHANNEL_CLOSING_NOTIFY wire message + timing; the sole in-process need is
// timeout knobs, which Pattern 4 profiles provide.  See file-header block.
int two_snapshot_invariant(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            const std::string ch_name =
                make_test_channel_name("health.two_snapshot");
            const std::string prod_uid = "prod." + ch_name;

            auto curve = pylabhub::tests::make_curve_setup({prod_uid});
            pylabhub::tests::seed_curve_identities(curve);

            auto broker = pylabhub::tests::start_hubhost_broker(
                hub_overrides_with_timeouts(/*ready=*/500, /*pending=*/500),
                curve, "HealthTwoSnapshotHub");

            std::atomic<bool>                     closing_fired{false};
            std::chrono::steady_clock::time_point closing_at{};
            std::mutex                            t_mu;

            BrcHandle prod;
            prod.brc.on_notification(
                [&](const std::string &type, const nlohmann::json &) {
                    if (type == "CHANNEL_CLOSING_NOTIFY") {
                        std::lock_guard<std::mutex> lk(t_mu);
                        closing_at = std::chrono::steady_clock::now();
                        closing_fired.store(true);
                    }
                });
            prod.start(broker.endpoint, broker.pubkey, prod_uid,
                       role_keystore_name(prod_uid));
            auto reg = prod.brc.register_channel(
                pylabhub::tests::make_reg_opts(ch_name, prod_uid), 3000);
            ASSERT_TRUE(reg.has_value());

            prod.brc.send_heartbeat(ch_name, prod_uid, "producer", {});
            const auto t0 = std::chrono::steady_clock::now();

            // Lower bound: NOTIFY must NOT fire before
            // (ready_timeout + pending_timeout) ≈ 1000ms.  Sleep 700ms
            // — well inside the legitimate Pending phase.
            std::this_thread::sleep_for(std::chrono::milliseconds(700));
            EXPECT_FALSE(closing_fired.load())
                << "CHANNEL_CLOSING_NOTIFY fired too early — Pass-2 "
                   "termination must NOT happen in the same sweep tick "
                   "as the Pass-1 Connected→Pending demotion.  This is "
                   "the two-snapshot invariant; a single-snapshot "
                   "migration would fail here.";

            // Upper bound: NOTIFY must fire by t0 + 3s.
            const auto deadline = t0 + std::chrono::seconds(3);
            while (!closing_fired.load() &&
                   std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            ASSERT_TRUE(closing_fired.load())
                << "CHANNEL_CLOSING_NOTIFY did not fire within 3s";

            // Lower-bound pin: fire time ≥ 800ms after last heartbeat.
            std::chrono::milliseconds elapsed_at_close{};
            {
                std::lock_guard<std::mutex> lk(t_mu);
                elapsed_at_close =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        closing_at - t0);
            }
            EXPECT_GE(elapsed_at_close, std::chrono::milliseconds(800))
                << "NOTIFY fired at " << elapsed_at_close.count()
                << "ms after last heartbeat — expected ≥800ms "
                   "(ready_timeout + pending_timeout - sweep slop). "
                   "A single-snapshot migration would produce ≈500-600ms.";

            prod.stop();
            broker.stop_and_join();
        },
        "broker_health.two_snapshot_invariant",
        logger_module(), file_lock_module(), json_module(),
        ::pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(), zmq_module());
}

// `channel_torn_down` short-circuit (HEP-0039 P8 Step B prerequisite).
// Producer + Consumer both go silent at the same moment; the producer
// Pass-2 evicts the channel; the consumer Pass-2 MUST be skipped — no
// stray CONSUMER_DIED_NOTIFY to a vanished channel.
int channel_torn_down_consumer_pass2_skipped(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            const std::string ch_name =
                make_test_channel_name("health.channel_torn_down");
            const std::string prod_uid = "prod." + ch_name;
            const std::string cons_uid = "cons." + ch_name;

            auto curve = pylabhub::tests::make_curve_setup({prod_uid, cons_uid});
            pylabhub::tests::seed_curve_identities(curve);

            auto broker = pylabhub::tests::start_hubhost_broker(
                hub_overrides_with_timeouts(/*ready=*/500, /*pending=*/500),
                curve, "HealthChannelTornDownHub");

            std::atomic<int> closing_count{0};
            std::atomic<int> consumer_died_count{0};

            BrcHandle prod;
            prod.brc.on_notification(
                [&](const std::string &type, const nlohmann::json &) {
                    if (type == "CHANNEL_CLOSING_NOTIFY")
                        closing_count.fetch_add(1);
                    if (type == "CONSUMER_DIED_NOTIFY")
                        consumer_died_count.fetch_add(1);
                });
            prod.start(broker.endpoint, broker.pubkey, prod_uid,
                       role_keystore_name(prod_uid));
            auto reg = prod.brc.register_channel(
                pylabhub::tests::make_reg_opts(ch_name, prod_uid), 3000);
            ASSERT_TRUE(reg.has_value());

            BrcHandle cons;
            cons.start(broker.endpoint, broker.pubkey, cons_uid,
                       role_keystore_name(cons_uid));
            auto creg = cons.brc.register_consumer(
                pylabhub::tests::make_cons_opts(ch_name, cons_uid), 3000);
            ASSERT_TRUE(creg.has_value());

            // Both heartbeat once, then silent — same start moment so
            // both enter Pending in the same sweep tick, both time out
            // in the same Pass-2 tick.
            prod.brc.send_heartbeat(ch_name, prod_uid, "producer", {});
            cons.brc.send_heartbeat(ch_name, cons_uid, "consumer", {});

            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(5);
            while (closing_count.load() == 0 &&
                   std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            ASSERT_GE(closing_count.load(), 1)
                << "CHANNEL_CLOSING_NOTIFY did not fire after the "
                   "last-producer atomic teardown";

            // Steady-state window: a buggy migration would fire a
            // stray CONSUMER_DIED_NOTIFY in a later sweep iteration.
            std::this_thread::sleep_for(std::chrono::milliseconds(700));

            EXPECT_EQ(consumer_died_count.load(), 0)
                << "CONSUMER_DIED_NOTIFY must NOT fire — the consumer "
                   "Pass-2 iteration was supposed to be skipped via the "
                   "`channel_torn_down` short-circuit because the "
                   "producer Pass-2 already evicted the channel in the "
                   "same sweep tick.  A migration that drops this "
                   "short-circuit would fail this assertion.";

            cons.stop();
            prod.stop();
            broker.stop_and_join();
        },
        "broker_health.channel_torn_down_consumer_pass2_skipped",
        logger_module(), file_lock_module(), json_module(),
        ::pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(), zmq_module());
}

// ============================================================================
// ctrl_zap_deny_path — D2 default-deny security gate
// ============================================================================
//
// Pin that the broker's CTRL ROUTER ZAP gate (HEP-CORE-0035 §4.2 + §4.8
// + PeerAdmission Phase D step D2) actually fires DENY when a CURVE
// peer's pubkey is NOT in `known_roles`.  Without this, the
// deny-by-default contract has zero path-level coverage (audit B2).
//
// Strict-CURVE migration (2026-06-30): pre-#172 the BRC took
// `client_pubkey` + `client_seckey` directly on its Config.  Post-#172
// the BRC reads keys from the process KeyStore by name (HEP-CORE-0040
// §172).  The test now:
//   - Generates a fresh CURVE keypair via `make_curve_setup({deny_uid})`
//     and seeds it in the process KeyStore via `seed_curve_identities()`
//     under name `role.<deny_uid>`.
//   - Builds the broker with a SEPARATE `CurveSetup` (the `setup` arg
//     to `start_direct_broker`) whose `role_keys` map is EMPTY.  That
//     means `apply_curve_to` pushes ZERO entries to `known_roles`, so
//     no pubkey is in the broker's allowlist — every CURVE handshake
//     is DENIED by the broker's L1 ZAP gate.
//   - Connects the BRC with `keystore_name = role.<deny_uid>`.  libzmq
//     sends our pubkey on the CURVE handshake; the broker's ZAP gate
//     sees it's not in `known_roles` → DENY → handshake fails →
//     REG_REQ times out.
//   - Asserts `ZapRouter::denied_count()` increased: the
//     PATH-DISCRIMINATING pin that the timeout came from the ZAP gate,
//     not from another path.
// ============================================================================

// RATIONALE (task #52 Round 3, KEEP): reads the process-singleton
// ZapRouter::denied_count() (before/after) to prove the REG failure came
// through the ZAP deny gate — an aggregate counter with no wire observable.
// DirectBrokerHandle is broker-only (one ZAP pump).  See file-header block.
int ctrl_zap_deny_path(int /*argc*/, char ** /*argv*/)
{
    return run_gtest_worker(
        []() {
            namespace sec = pylabhub::utils::security;

            const std::string deny_uid = "prod.deny.test.uid00000001";

            // Step 1: generate a fresh CURVE keypair for the test
            // client and seed it in the process KeyStore.  This is
            // what the BRC will use as its client identity.
            CurveSetup client_setup;
            client_setup.hub = pylabhub::tests::gen_curve_keypair();
            client_setup.role_keys.emplace(
                deny_uid, pylabhub::tests::gen_curve_keypair());
            pylabhub::tests::seed_curve_identities(client_setup);

            // Step 2: build the broker's CurveSetup with EMPTY
            // role_keys — so `apply_curve_to` pushes ZERO entries to
            // `known_roles`.  Empty allowlist → every CURVE handshake
            // is DENY.  Note we reuse `client_setup.hub` for the
            // broker's hub identity so both processes agree on the
            // hub's CURVE serverkey.
            CurveSetup broker_setup;
            broker_setup.hub = client_setup.hub;
            // broker_setup.role_keys deliberately empty.

            // Capture pre-test denied counter (process-singleton).
            // SpawnWorker runs each test in its own subprocess so the
            // counter starts at 0, but reading before-state is the
            // more robust pattern.
            const auto before_denied =
                sec::ZapRouter::instance().denied_count();

            // Step 3: spin up broker via the direct-handle harness.
            // We use `start_direct_broker` rather than HubHost because
            // we need the direct BrokerService::Config to set
            // `enforce_ctrl_admission` (HubHost derives that from
            // hub.json; for an L3 ZAP-path pin the direct path is
            // simpler).
            // Post-#172 strict-CURVE: admission enforcement is
            // unconditional (the legacy `enforce_ctrl_admission` knob
            // was deleted).  Empty `known_roles` = deny-all allowlist
            // by construction.
            BrokerService::Config bcfg;
            bcfg.endpoint = "tcp://127.0.0.1:0";
            auto broker = pylabhub::tests::start_direct_broker(
                std::move(bcfg), broker_setup);

            // Step 4: connect a BRC using the seeded client identity.
            // Post-#172 BRC takes `keystore_name`, NOT raw keys.
            BrokerRequestComm brc;
            BrokerRequestComm::Config ccfg;
            ccfg.broker_endpoint = broker.endpoint;
            ccfg.broker_pubkey   = broker.pubkey;
            ccfg.role_uid        = deny_uid;
            ccfg.role_name       = "deny_role";
            ccfg.keystore_name   = role_keystore_name(deny_uid);
            EXPECT_TRUE(brc.connect(ccfg))
                << "BRC TCP connect should succeed even when ZAP denies; "
                   "the CURVE handshake fails on first send, not on connect.";

            // Step 5: attempt REG_REQ — handshake fires, ZAP denies,
            // REG_REQ times out.
            const auto reg_result = brc.register_channel(
                pylabhub::tests::make_reg_opts("ch.deny.path", deny_uid),
                /*timeout_ms=*/1500);
            EXPECT_FALSE(reg_result.has_value())
                << "register_channel succeeded under deny-all allowlist — "
                   "the ZAP gate did NOT fire.";

            // Step 6: PATH PIN — the denial fired through ZapRouter.
            // This is what distinguishes a real ZAP deny from a
            // happens-to-time-out failure on some other path.
            const auto after_denied =
                sec::ZapRouter::instance().denied_count();
            EXPECT_GT(after_denied, before_denied)
                << "ZapRouter::denied_count() did not increase: "
                << before_denied << " -> " << after_denied
                << " — the deny did not come from the ZAP gate.";

            brc.disconnect();
            broker.stop_and_join();
        },
        "broker_health.ctrl_zap_deny_path",
        logger_module(), file_lock_module(), json_module(),
        ::pylabhub::utils::security::SecureSubsystem::GetLifecycleModule(), zmq_module());
}

} // namespace pylabhub::tests::worker::broker_health

// ============================================================================
// Worker dispatcher registrar
// ============================================================================

namespace
{

struct BrokerHealthWorkerRegistrar
{
    BrokerHealthWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                const auto       dot  = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "broker_health")
                {
                    return -1;
                }
                std::string scenario(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::broker_health;
                // producer_gets_closing_notify / producer_auto_deregisters
                // / schema_mismatch_notify MIGRATED to Pattern 4
                // (task #52 Round 2).
                if (scenario == "consumer_auto_deregisters")
                    return consumer_auto_deregisters(argc, argv);
                // dead_consumer_orchestrator / dead_consumer_exiter /
                // consumer_heartbeat_timeout_notify MIGRATED to Pattern 4
                // (task #52 Round 2).
                if (scenario == "multi_producer_partial_pending_timeout")
                    return multi_producer_partial_pending_timeout(argc, argv);
                if (scenario == "two_snapshot_invariant")
                    return two_snapshot_invariant(argc, argv);
                if (scenario == "channel_torn_down_consumer_pass2_skipped")
                    return channel_torn_down_consumer_pass2_skipped(argc, argv);
                if (scenario == "ctrl_zap_deny_path")
                    return ctrl_zap_deny_path(argc, argv);
                fmt::print(stderr, "ERROR: Unknown broker_health scenario '{}'\n", scenario);
                return 1;
            });
    }
};

static BrokerHealthWorkerRegistrar g_broker_health_registrar; // NOLINT(cert-err58-cpp)

} // anon
