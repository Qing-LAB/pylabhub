/**
 * @file pattern4_heartbeat_workers.cpp
 * @brief Subprocess workers for Pattern 4 rung 3 — Pattern4HeartbeatTest.
 *
 * Two scenarios:
 *
 *   - `pattern4_heartbeat.broker` — BrokerService that accepts a
 *     producer-side REG_REQ then stays open while heartbeats flow.
 *     On shutdown, the broker emits per-presence `heartbeat counter`
 *     INFO summaries (added under task #223 in `BrokerServiceImpl::run`).
 *
 *   - `pattern4_heartbeat.producer_role` — constructs a RoleAPIBase +
 *     RoleHandler stack the same way rung 2 does, but instead of
 *     exiting immediately after REG_ACK it stays in a quit-pipe
 *     wait so the periodic heartbeat tick (installed post-REG_ACK by
 *     `RoleAPIBase::install_heartbeat`) keeps firing for the
 *     measurement window the parent test controls.
 *
 * Marker contract (per docs/README/README_testing.md § Pattern 4
 * rung 3 + HEP-CORE-0023 §2.5):
 *
 *     [Broker]                              [Role]
 *     ──────────────────────────            ───────────────────────────────
 *     REG_REQ accepted role='...'           presence Unregistered->
 *     REG_ACK sending channel='X'              RegRequestPending
 *       heartbeat_interval_ms=N             REG_ACK received initial_allowlist
 *                                           presence state ->Registered
 *                                           heartbeat: aligned with hub
 *                                             — role cadence R ms, hub max N ms
 *                                           heartbeat: periodic tick installed
 *                                             at M ms                       (M ≤ N)
 *     first heartbeat received from
 *       role='...' channel='X'              ... heartbeats flow ...
 *
 *     (parent waits a measurement window, then signals quit)
 *
 *     heartbeat counter role='...' …       heartbeat counter: sent=K over …ms
 *       received=K
 *
 * The parent extracts numeric fields from these markers and verifies:
 *   1. install_interval == min(role_cfg, ack_interval)   (negotiation)
 *   2. sent count ≈ window / install_interval ± 30%      (cadence × steady-state)
 *   3. received count ≈ sent count ± 30%                  (broker-side observation)
 *
 * Mutation discipline: disabling `install_heartbeat` → install marker
 * missing, both counters zero, assertion fails.  Changing broker's
 * REG_ACK interval to something the role ignores → install_interval
 * mismatch.  Either failure produces a focused diagnostic.
 */
#include "pattern4_helpers.h"

#include "curve_test_setup.h"
#include "shared_test_helpers.h"
#include "test_entrypoint.h"

#include "utils/broker_request_comm.hpp"
#include "utils/broker_service.hpp"
#include "utils/file_lock.hpp"
#include "utils/hub_state.hpp"
#include "utils/json_config.hpp"
#include "utils/logger.hpp"
#include "utils/role_api_base.hpp"
#include "utils/role_handler.hpp"
#include "utils/role_host_core.hpp"
#include "utils/role_uid.hpp"
#include "utils/security/key_store.hpp"
#include "utils/security/secure_memory_subsystem.hpp"
#include "utils/timeout_constants.hpp"
#include "utils/zmq_context.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace pylabhub::tests::pattern4
{

namespace
{

// Broker bind-retry parameters per Pattern 4 doc § "Bind robustness".
constexpr int  kBindMaxAttempts    = 4;
constexpr auto kBindInitialBackoff = std::chrono::milliseconds{10};

// Safety timeout for both subprocesses — fires ONLY if the parent
// crashed or skipped signal_quit().  Normal exit comes from the parent
// signalling quit after its measurement window.
constexpr auto kHeartbeatSafetyTimeout = std::chrono::seconds{60};

void maybe_redirect_to_shared_log(const Pattern4Setup &setup)
{
    if (!setup.shared_log_path.empty())
        set_shared_log(setup.shared_log_path);
}

} // anon

// ─── pattern4_heartbeat.broker ──────────────────────────────────────────────

int pattern4_heartbeat_broker(const char *temp_dir_arg)
{
    return pylabhub::tests::helper::run_gtest_worker(
        [&]() {
            const fs::path temp_dir = temp_dir_arg;
            const auto     setup    = read_pattern4_setup(temp_dir / "setup.json");

            pylabhub::tests::CurveKeyStoreFixture ks_fixture(
                "pattern4", "heartbeat.broker", setup.curve);

            maybe_redirect_to_shared_log(setup);

            pylabhub::broker::BrokerService::Config cfg;
            cfg.endpoint = setup.broker_endpoint;
            pylabhub::tests::apply_curve_to(cfg, setup.curve);

            std::promise<std::string> ready_promise;
            auto                       ready_future = ready_promise.get_future();
            cfg.on_ready = [&ready_promise](const std::string &ep,
                                             const std::string &pk) {
                LOGGER_INFO("Pattern4Broker: bound endpoint='{}' pubkey='{}'",
                            ep, pk);
                ready_promise.set_value(ep);
            };

            pylabhub::hub::HubState                            state;
            std::unique_ptr<pylabhub::broker::BrokerService> broker;
            auto backoff = kBindInitialBackoff;
            for (int attempt = 1; attempt <= kBindMaxAttempts; ++attempt)
            {
                try
                {
                    broker = std::make_unique<
                        pylabhub::broker::BrokerService>(cfg, state);
                    break;
                }
                catch (const zmq::error_t &e)
                {
                    if (e.num() == EADDRINUSE && attempt < kBindMaxAttempts)
                    {
                        LOGGER_WARN("Pattern4Broker: bind EADDRINUSE on "
                                    "endpoint='{}' - retrying in {}ms "
                                    "(attempt {}/{})",
                                    cfg.endpoint, backoff.count(),
                                    attempt, kBindMaxAttempts);
                        std::this_thread::sleep_for(backoff);
                        backoff *= 5;
                        continue;
                    }
                    throw;
                }
            }
            ASSERT_TRUE(broker);

            std::thread broker_thread([&] { broker->run(); });
            ASSERT_EQ(ready_future.wait_for(std::chrono::seconds{2}),
                      std::future_status::ready);

            // Periodic snapshot + "role done" detector — runs on the
            // main thread.  Every 500 ms:
            //   (a) iterate hub_state_->snapshot() and emit a test-only
            //       log line per presence with the current
            //       `heartbeats_received` counter — the parent reads
            //       the LATEST one for the broker-side count;
            //   (b) check whether the count has stopped growing.  If
            //       it's been stable for `kStableTicks` snapshots AND
            //       at least one heartbeat was received, the role is
            //       done and the broker exits on its own.  This avoids
            //       any dependence on signal_quit() reaching this
            //       subprocess — the broker decides when to stop based
            //       on observed state.
            //
            // Pure test observability — no production-code involvement
            // beyond the HubState snapshot API that already exists.
            constexpr auto kSnapshotInterval = std::chrono::milliseconds{500};
            constexpr int  kStableTicks      = 4;  // 2 s of no growth
            std::uint64_t last_total      = 0;
            std::uint64_t max_seen        = 0;
            int           stable_streak   = 0;
            const auto    overall_deadline =
                std::chrono::steady_clock::now() + kHeartbeatSafetyTimeout;

            LOGGER_INFO("Pattern4Broker: polling for role-done "
                        "(safety deadline {} s)",
                        kHeartbeatSafetyTimeout.count());
            while (std::chrono::steady_clock::now() < overall_deadline)
            {
                std::this_thread::sleep_for(kSnapshotInterval);
                const auto    snap        = state.snapshot();
                std::uint64_t total_count = 0;
                for (const auto &[uid, role_entry] : snap.roles)
                {
                    for (const auto &p : role_entry.presences)
                    {
                        LOGGER_INFO("Pattern4Broker: counter snapshot "
                                    "role='{}' channel='{}' "
                                    "role_type='{}' received={}",
                                    uid, p.channel, p.role_type,
                                    p.heartbeats_received);
                        total_count += p.heartbeats_received;
                    }
                }
                max_seen = std::max(max_seen, total_count);

                if (total_count == last_total)
                    ++stable_streak;
                else
                    stable_streak = 0;
                last_total = total_count;

                if (max_seen > 0 && stable_streak >= kStableTicks)
                {
                    LOGGER_INFO("Pattern4Broker: role done (count stable at "
                                "{} for {} ticks), stopping",
                                total_count, stable_streak);
                    break;
                }
            }
            if (std::chrono::steady_clock::now() >= overall_deadline)
                LOGGER_WARN("Pattern4Broker: safety deadline reached "
                            "(saw max {} heartbeats)", max_seen);
            broker->stop();
            broker_thread.join();
            LOGGER_INFO("Pattern4Broker: exiting cleanly");
        },
        "pattern4_heartbeat.broker",
        pylabhub::utils::Logger::GetLifecycleModule(),
        pylabhub::utils::FileLock::GetLifecycleModule(),
        pylabhub::utils::JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

// ─── pattern4_heartbeat.producer_role ───────────────────────────────────────

int pattern4_heartbeat_producer_role(const char *temp_dir_arg)
{
    return pylabhub::tests::helper::run_gtest_worker(
        [&]() {
            const fs::path temp_dir = temp_dir_arg;
            const auto     setup    = read_pattern4_setup(temp_dir / "setup.json");
            const std::string role_uid = pylabhub::scripting::make_role_uid(
                pylabhub::scripting::RoleUidTag::Producer, "pattern4hb", 1u);
            const std::string channel  = "hb.test";

            pylabhub::tests::CurveKeyStoreFixture ks_fixture(
                "pattern4", "heartbeat.producer_role", setup.curve);
            pylabhub::tests::CurveKeyStoreFixture::add_identity(
                pylabhub::utils::security::kRoleIdentityName,
                setup.curve.role_keys.at(role_uid));

            maybe_redirect_to_shared_log(setup);

            pylabhub::scripting::RoleHostCore core;
            core.set_running(true);
            pylabhub::scripting::RoleAPIBase api(core, "prod", role_uid);
            api.set_name("pattern4_heartbeat");
            api.set_channel(channel);

            pylabhub::config::HubRefConfig hub_cfg;
            hub_cfg.broker        = setup.broker_endpoint;
            hub_cfg.broker_pubkey = setup.curve.hub.public_z85;

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

            ASSERT_TRUE(api.start_handler_threads(std::move(handler)));
            ASSERT_NE(api.handler(), nullptr);

            // Send REG_REQ to register the producer channel.
            nlohmann::json reg_opts;
            reg_opts["channel_name"]      = channel;
            reg_opts["pattern"]           = "PubSub";
            reg_opts["has_shared_memory"] = false;
            reg_opts["producer_pid"]      = static_cast<uint64_t>(::getpid());
            reg_opts["role_uid"]          = role_uid;
            reg_opts["role_name"]         = "pattern4_heartbeat";
            reg_opts["zmq_pubkey"]        =
                setup.curve.role_keys.at(role_uid).public_z85;

            auto reg_resp = api.register_producer_channel(
                reg_opts, pylabhub::kMidTimeoutMs);
            ASSERT_TRUE(reg_resp.has_value());
            ASSERT_EQ(reg_resp->value("status", std::string{}), "success");

            // Trigger heartbeat install — uses hub-authoritative cadence
            // negotiated via REG_ACK + role's preferred via a hard-coded
            // test cadence (1000 ms, well above the typical 500 ms hub
            // default so the role's "aligned with hub" branch fires the
            // downgrade-to-hub-max path, exercising rung 3's negotiation
            // axis).
            const int role_cfg_ms = 1000;
            const auto hub_max =
                pylabhub::scripting::RoleAPIBase::extract_hub_heartbeat_max(
                    *reg_resp);
            api.install_heartbeat(role_cfg_ms, hub_max);

            // BRC's `set_periodic_task` gates each firing on BOTH a
            // wall-clock interval AND an iteration-count predicate.
            // In production the role-host main loop calls
            // `core.inc_iteration_count()` per cycle; this test has no
            // such loop, so spin up a small background thread that
            // ticks the iteration count fast enough that the BRC
            // poll-loop's interval check is the binding constraint
            // for cadence.  Without this thread the periodic task
            // never fires — the bug surfaced during #223 bring-up.
            std::atomic<bool> iter_driver_running{true};
            std::thread iter_driver([&core, &iter_driver_running] {
                while (iter_driver_running.load(std::memory_order_relaxed))
                {
                    core.inc_iteration_count();
                    std::this_thread::sleep_for(std::chrono::milliseconds{20});
                }
            });

            // Fixed measurement window — role drives its own duration
            // (no parent-side quit-signal needed for this rung).  The
            // window must cover enough heartbeat ticks for the rate
            // band to be tight; 2 s × 2 Hz default = 4 ticks minimum.
            // CI gets a generous window per the `PYLABHUB_CI_BUILD`
            // compile-time flag — slow scheduling under -j 2 can
            // delay the first tick a few hundred ms, eating into the
            // accuracy of a short window.
#ifdef PYLABHUB_CI_BUILD
            constexpr auto kMeasurementWindow = std::chrono::seconds{4};
#else
            constexpr auto kMeasurementWindow = std::chrono::seconds{2};
#endif
            LOGGER_INFO("Pattern4Role[{}]: measuring heartbeat for {}s",
                        role_uid, kMeasurementWindow.count());
            std::this_thread::sleep_for(kMeasurementWindow);
            LOGGER_INFO("Pattern4Role[{}]: measurement window elapsed",
                        role_uid);

            iter_driver_running.store(false, std::memory_order_relaxed);
            iter_driver.join();

            api.stop_handler_threads();
            LOGGER_INFO("Pattern4Role[{}]: exiting cleanly", role_uid);
        },
        "pattern4_heartbeat.producer_role",
        pylabhub::utils::Logger::GetLifecycleModule(),
        pylabhub::utils::FileLock::GetLifecycleModule(),
        pylabhub::utils::JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

// ─── Dispatcher registration ────────────────────────────────────────────────

int dispatch_pattern4_heartbeat(int argc, char **argv)
{
    if (argc < 2) return -1;
    const std::string mode = argv[1];
    const auto        dot  = mode.find('.');
    if (dot == std::string::npos) return -1;
    const std::string module   = mode.substr(0, dot);
    const std::string scenario = mode.substr(dot + 1);
    if (module != "pattern4_heartbeat") return -1;

    if (argc < 3)
    {
        std::fprintf(stderr,
                     "pattern4_heartbeat.%s: missing <temp_dir> arg\n",
                     scenario.c_str());
        return 1;
    }
    const char *temp_dir = argv[2];

    if (scenario == "broker")
        return pattern4_heartbeat_broker(temp_dir);
    if (scenario == "producer_role")
        return pattern4_heartbeat_producer_role(temp_dir);

    std::fprintf(stderr,
                 "pattern4_heartbeat: unknown scenario '%s'\n",
                 scenario.c_str());
    return 1;
}

struct Pattern4HeartbeatRegistrar
{
    Pattern4HeartbeatRegistrar()
    {
        ::register_worker_dispatcher(dispatch_pattern4_heartbeat);
    }
};

static Pattern4HeartbeatRegistrar g_pattern4_heartbeat_registrar;

} // namespace pylabhub::tests::pattern4
