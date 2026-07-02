/**
 * @file pattern4_consumer_lifecycle_workers.cpp
 * @brief Subprocess workers for Pattern 4 rung 4 — Pattern4ConsumerLifecycleTest.
 *
 * ─── Test-faithfulness principle ────────────────────────────────────────────
 *
 * Pattern 4 workers are hand-rolled subprocess scaffolds (no `plh_role`
 * binary spawn).  To prevent the test from drifting away from production
 * and silently masking design bugs, the workers MUST satisfy three
 * invariants — verifiable by reading the diff against
 * `src/consumer/consumer_role_host.cpp` and
 * `src/producer/producer_role_host.cpp`:
 *
 *   1. **Production API only.**  Every call into the framework must be
 *      a public production entry point — `api.build_tx_queue`,
 *      `api.register_producer_channel`, `api.apply_producer_reg_ack`,
 *      `hub::build_producer_reg_payload`, etc.  No test-only factories,
 *      no direct queue/state poking, no mocks.  If a test needs
 *      behavior that production cannot express, that is a design bug to
 *      surface — not a deviation to bolt in.
 *
 *   2. **Production sequence.**  The call ORDER must match
 *      `*_role_host::worker_main_` / `RoleHostFrame::setup_infrastructure_`
 *      as documented in the relevant HEP.  Citing the HEP section per
 *      step lets future readers verify the parity at a glance.
 *
 *   3. **Production parameter SHAPES.**  REG_REQ payloads are built via
 *      `hub::build_*_reg_payload` (NOT hand-rolled JSON).  The
 *      identity pubkey comes from `key_store().pubkey(kRoleIdentityName)`
 *      (NOT the test setup's curve bundle directly).  Schema fields
 *      go through `make_wire_schema_fields` + `apply_*_schema_fields`.
 *      Values (port number, channel name) may be test-chosen, but
 *      the WIRE SHAPE must match the production helper output.
 *
 * Deviations from production are documented inline with a `TEST-ONLY:`
 * tag and a justification.  Today's documented deviations:
 *   - No `engine.invoke_on_init()` — the test workers run no script.
 *   - No `inbox_cfg` — the test does not exercise inbox.  Production's
 *     `api.append_inbox_to_reg(reg_opts, inbox_cfg_)` is still called
 *     for shape parity (no-op when inbox disabled).
 *   - `iter_driver` thread spinning `core_.inc_iteration_count()` —
 *     production's main loop drives this; the test has no main loop.
 *     Same shape used in rung 3 heartbeat workers.
 *
 * If a new rung needs to add a non-trivial deviation, STOP and surface
 * it as a design question — the principle exists to make tests reveal
 * bugs, not to dress test-only workarounds as features.
 *
 * ─── Cross-process orchestration ────────────────────────────────────────────
 *
 * Three subprocesses orchestrated by the parent:
 *
 *   - `pattern4_consumer_lifecycle.broker` — BrokerService; held alive
 *     via quit-pipe so both producer and consumer can register against
 *     a live hub.
 *
 *   - `pattern4_consumer_lifecycle.producer_role` — mirrors
 *     `producer_role_host::worker_main_` (build_tx_queue → REG_REQ →
 *     apply_producer_reg_ack → install_heartbeat).  Held alive via
 *     quit-pipe so its presence stays kLive while the consumer
 *     registers.
 *
 *   - `pattern4_consumer_lifecycle.consumer_role` — mirrors
 *     `consumer_role_host::worker_main_` (build_rx_queue Standby →
 *     CONSUMER_REG_REQ → apply_consumer_reg_ack drives Standby →
 *     Configured → Active per HEP-CORE-0036 §6.7).  Exits cleanly on
 *     its own.
 *
 * All three redirect their Logger sink to `setup.shared_log_path`;
 * the parent then pins the cross-process marker sequence with
 * `expect_log_sequence`.
 *
 * See docs/README/README_testing.md § "Pattern 4 — ... — Test ladder"
 * rung 4 for the contract this trio pins.
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
#include "utils/role_reg_payload.hpp"
#include "utils/role_uid.hpp"
#include "utils/schema_utils.hpp"
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

constexpr int  kBindMaxAttempts    = 4;
constexpr auto kBindInitialBackoff = std::chrono::milliseconds{10};

// Safety timeout for any worker held on the quit-pipe.  Same generous
// budget rung 2/3 use — fires only on parent crash.
constexpr auto kSafetyTimeout = std::chrono::seconds{60};

void maybe_redirect_to_shared_log(const Pattern4Setup &setup)
{
    if (!setup.shared_log_path.empty())
        set_shared_log(setup.shared_log_path);
}

} // anon

// ─── pattern4_consumer_lifecycle.broker ─────────────────────────────────────

int pattern4_consumer_lifecycle_broker(const char *temp_dir_arg)
{
    return pylabhub::tests::helper::run_gtest_worker(
        [&]() {
            const fs::path temp_dir = temp_dir_arg;
            const auto     setup    = read_pattern4_setup(temp_dir / "setup.json");

            pylabhub::tests::CurveKeyStoreFixture ks_fixture(
                "pattern4", "consumer_lifecycle.broker", setup.curve);

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
            ASSERT_TRUE(broker)
                << "Pattern4Broker: failed to bind after "
                << kBindMaxAttempts << " attempts on '"
                << cfg.endpoint << "'";

            std::thread broker_thread([&] { broker->run(); });

            ASSERT_EQ(ready_future.wait_for(std::chrono::seconds{2}),
                      std::future_status::ready)
                << "Pattern4Broker: on_ready callback did not fire within 2s";

            LOGGER_INFO("Pattern4Broker: waiting on quit-signal pipe "
                        "(safety timeout {} s)",
                        kSafetyTimeout.count());
            const auto wait_result =
                pylabhub::tests::helper::wait_for_quit_or_safety_timeout(
                    kSafetyTimeout);
            switch (wait_result)
            {
            case pylabhub::tests::helper::QuitWaitResult::QuitSignal:
                LOGGER_INFO("Pattern4Broker: quit signal received, stopping");
                break;
            case pylabhub::tests::helper::QuitWaitResult::SafetyTimeout:
                LOGGER_WARN("Pattern4Broker: safety timeout fired — parent "
                            "did not call signal_quit() within {} s",
                            kSafetyTimeout.count());
                break;
            case pylabhub::tests::helper::QuitWaitResult::NoQuitPipe:
                LOGGER_WARN("Pattern4Broker: no PLH_TEST_QUIT_FD — falling "
                            "back to 3 s sleep before exit");
                std::this_thread::sleep_for(std::chrono::seconds{3});
                break;
            }

            broker->stop();
            broker_thread.join();
            LOGGER_INFO("Pattern4Broker: exiting cleanly");
        },
        "pattern4_consumer_lifecycle.broker",
        pylabhub::utils::Logger::GetLifecycleModule(),
        pylabhub::utils::FileLock::GetLifecycleModule(),
        pylabhub::utils::JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

// ─── pattern4_consumer_lifecycle.producer_role ──────────────────────────────

int pattern4_consumer_lifecycle_producer_role(const char *temp_dir_arg)
{
    return pylabhub::tests::helper::run_gtest_worker(
        [&]() {
            const fs::path temp_dir = temp_dir_arg;
            const auto     setup    = read_pattern4_setup(temp_dir / "setup.json");
            const std::string role_uid = pylabhub::scripting::make_role_uid(
                pylabhub::scripting::RoleUidTag::Producer,
                "pattern4cons", 1u);
            const std::string channel = "data.test";

            pylabhub::tests::CurveKeyStoreFixture ks_fixture(
                "pattern4", "consumer_lifecycle.producer_role", setup.curve);
            pylabhub::tests::CurveKeyStoreFixture::add_identity(
                pylabhub::utils::security::kRoleIdentityName,
                setup.curve.role_keys.at(role_uid));

            maybe_redirect_to_shared_log(setup);

            pylabhub::scripting::RoleHostCore core;
            core.set_running(true);
            pylabhub::scripting::RoleAPIBase api(core, "prod", role_uid);
            api.set_name("pattern4_consumer_lifecycle_producer");
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

            // Build the tx queue BEFORE register_producer_channel.
            // Production replicates this via RoleHostFrame::setup_
            // infrastructure_ — the tx queue's bound endpoint is what
            // gets advertised to the broker in REG_REQ.zmq_node_endpoint,
            // which the broker propagates to consumers via
            // CONSUMER_REG_ACK.producers[].endpoint per HEP-CORE-0036
            // §6.4.  Without a real endpoint the consumer's
            // apply_master_approval rejects the ACK (missing required
            // field) and Standby->Configured never fires.
            //
            // ── Step 1: build_tx_queue (mirror RoleHostFrame::setup_
            //    infrastructure_ §4 — tx queue built BEFORE registration
            //    so the bound endpoint is what we advertise on REG_REQ).
            //    slot_spec is consumer-local config in production; the
            //    test picks a minimal single-uint32 schema (matches the
            //    consumer's choice below — broker enforces schema-hash
            //    consistency across the channel's producers).
            const int tx_port =
                pylabhub::tests::pattern4::pick_unused_port();
            const std::string tx_endpoint =
                "tcp://127.0.0.1:" + std::to_string(tx_port);
            hub::TxQueueOptions tx_opts;
            tx_opts.data_transport       = "zmq";
            tx_opts.zmq_bind             = true;
            tx_opts.zmq_node_endpoint    = tx_endpoint;
            tx_opts.slot_spec.has_schema = true;
            tx_opts.slot_spec.fields.push_back(
                pylabhub::hub::FieldDef{"value", "uint32", 1u, 0u});
            ASSERT_TRUE(api.build_tx_queue(tx_opts))
                << "Pattern4Role[producer]: build_tx_queue must succeed "
                   "with bound endpoint '" << tx_endpoint << "'";

            // ── Step 2: build REG_REQ via the production builder.
            //    Mirrors producer_role_host.cpp:327-349 — `ProducerRegInputs`
            //    struct fed to `hub::build_producer_reg_payload`.  Identity
            //    pubkey read from KeyStore via the production accessor
            //    (HEP-CORE-0040 §172) so the test exercises the same
            //    key-lookup path as production.  Hand-rolling the JSON
            //    here would silently mask any future change to the
            //    production payload shape.
            namespace sec = pylabhub::utils::security;
            pylabhub::hub::ProducerRegInputs reg_in;
            reg_in.channel           = channel;
            reg_in.role_uid          = role_uid;
            reg_in.role_name         = "pattern4_consumer_lifecycle_producer";
            reg_in.role_type          = "producer";
            reg_in.has_shm           = false;
            reg_in.is_zmq_transport  = true;
            reg_in.zmq_node_endpoint = tx_endpoint;
            reg_in.zmq_pubkey        = std::string(
                sec::key_store().pubkey(sec::kRoleIdentityName));
            auto reg_opts =
                pylabhub::hub::build_producer_reg_payload(reg_in);

            // ── Step 3: schema fields layered on top of the REG_REQ
            //    (HEP-CORE-0034 §10.1).  Production calls this after the
            //    payload builder; without it the broker takes the
            //    legacy/anonymous schema path.  We pass an empty json
            //    for slot_schema_json and let the layer default-pack
            //    from slot_spec — same shape production uses when no
            //    BLDS named schema is configured.
            const pylabhub::hub::SchemaSpec empty_fz_spec{};
            const auto wire_schema = pylabhub::hub::make_wire_schema_fields(
                nlohmann::json{},               // slot_schema_json (anonymous mode)
                tx_opts.slot_spec,
                empty_fz_spec);
            pylabhub::hub::apply_producer_schema_fields(reg_opts, wire_schema);

            // ── Step 4: register_producer_channel → apply_producer_reg_ack
            //    → install_heartbeat (HEP-CORE-0036 §3.5.5 S2/S3/S4).
            //    Production order is enforced — apply_producer_reg_ack
            //    MUST run before install_heartbeat (§3.5.4 INV1: first
            //    heartbeat after data plane is up).
            auto reg_resp = api.register_producer_channel(
                reg_opts, pylabhub::kMidTimeoutMs);
            ASSERT_TRUE(reg_resp.has_value())
                << "Pattern4Role[producer]: REG_REQ must reach broker";
            ASSERT_EQ(reg_resp->value("status", std::string{}), "success")
                << "Pattern4Role[producer]: REG_ACK status must be success";

            ASSERT_TRUE(api.apply_producer_reg_ack(*reg_resp))
                << "Pattern4Role[producer]: apply_producer_reg_ack must "
                   "succeed (drives PUSH Standby->Active per §3.5.5 S3)";

            // Install heartbeat so the broker observes
            // first_heartbeat_seen=true.  Without this the consumer's
            // register_consumer call would loop on
            // CHANNEL_NOT_READY/awaiting_first_heartbeat until timeout
            // (HEP-CORE-0036 §5.2 R6 + §6.6 reason catalog).
            const auto hub_max =
                pylabhub::scripting::RoleAPIBase::extract_hub_heartbeat_max(
                    *reg_resp);
            api.install_heartbeat(/*role_cfg_ms*/ 500, hub_max);

            // BRC's `set_periodic_task` gates each firing on an
            // iteration-count predicate as well as wall-clock; spin
            // the iteration count so the heartbeat fires (same shape
            // as rung 3's iter_driver — needed because the test has
            // no production main loop calling inc_iteration_count()).
            std::atomic<bool> iter_driver_running{true};
            std::thread iter_driver([&core, &iter_driver_running] {
                while (iter_driver_running.load(std::memory_order_relaxed))
                {
                    core.inc_iteration_count();
                    std::this_thread::sleep_for(std::chrono::milliseconds{20});
                }
            });

            LOGGER_INFO("Pattern4Role[{}]: holding for consumer (quit-pipe)",
                        role_uid);
            const auto wait_result =
                pylabhub::tests::helper::wait_for_quit_or_safety_timeout(
                    kSafetyTimeout);
            switch (wait_result)
            {
            case pylabhub::tests::helper::QuitWaitResult::QuitSignal:
                LOGGER_INFO("Pattern4Role[{}]: quit signal received",
                            role_uid);
                break;
            case pylabhub::tests::helper::QuitWaitResult::SafetyTimeout:
                LOGGER_WARN("Pattern4Role[{}]: safety timeout — parent did "
                            "not call signal_quit() within {} s",
                            role_uid, kSafetyTimeout.count());
                break;
            case pylabhub::tests::helper::QuitWaitResult::NoQuitPipe:
                LOGGER_WARN("Pattern4Role[{}]: no PLH_TEST_QUIT_FD — "
                            "falling back to 3 s sleep",
                            role_uid);
                std::this_thread::sleep_for(std::chrono::seconds{3});
                break;
            }

            iter_driver_running.store(false, std::memory_order_relaxed);
            iter_driver.join();

            api.stop_handler_threads();
            LOGGER_INFO("Pattern4Role[{}]: exiting cleanly", role_uid);
        },
        "pattern4_consumer_lifecycle.producer_role",
        pylabhub::utils::Logger::GetLifecycleModule(),
        pylabhub::utils::FileLock::GetLifecycleModule(),
        pylabhub::utils::JsonConfig::GetLifecycleModule(),
        pylabhub::hub::GetZMQContextModule());
}

// ─── Dispatcher registration ────────────────────────────────────────────────

int dispatch_pattern4_consumer_lifecycle(int argc, char **argv)
{
    if (argc < 2) return -1;
    const std::string mode = argv[1];
    const auto        dot  = mode.find('.');
    if (dot == std::string::npos) return -1;
    const std::string module   = mode.substr(0, dot);
    const std::string scenario = mode.substr(dot + 1);
    if (module != "pattern4_consumer_lifecycle") return -1;

    if (argc < 3)
    {
        std::fprintf(stderr,
                     "pattern4_consumer_lifecycle.%s: missing <temp_dir> arg\n",
                     scenario.c_str());
        return 1;
    }
    const char *temp_dir = argv[2];

    if (scenario == "broker")
        return pattern4_consumer_lifecycle_broker(temp_dir);
    if (scenario == "producer_role")
        return pattern4_consumer_lifecycle_producer_role(temp_dir);
    // NOTE: `consumer_role` scenario retired 2026-07-02 (Phase 3b.2)
    // alongside the L3 test that dispatched it — see test_pattern4_
    // consumer_lifecycle.cpp head-of-file retirement block for the
    // migration record.  L4 (`test_plh_hub_role_zmq_e2e.cpp`) owns
    // the happy-path scenario the retired L3 test approximated.

    std::fprintf(stderr,
                 "pattern4_consumer_lifecycle: unknown scenario '%s'\n",
                 scenario.c_str());
    return 1;
}

struct Pattern4ConsumerLifecycleRegistrar
{
    Pattern4ConsumerLifecycleRegistrar()
    {
        ::register_worker_dispatcher(dispatch_pattern4_consumer_lifecycle);
    }
};

static Pattern4ConsumerLifecycleRegistrar g_pattern4_consumer_lifecycle_registrar;

} // namespace pylabhub::tests::pattern4
