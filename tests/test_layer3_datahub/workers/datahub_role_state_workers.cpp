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
    auto svc = std::make_unique<BrokerService>(std::move(cfg));
    auto* raw = svc.get();
    std::thread t([raw]() { raw->run(); });
    auto info = fut.get();
    BrokerHandle h;
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
    opts["zmq_ctrl_endpoint"] = "tcp://127.0.0.1:0";
    opts["zmq_data_endpoint"] = "tcp://127.0.0.1:0";
    opts["zmq_pubkey"]        = "";
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
            cfg.grace_override                   = std::chrono::milliseconds(1);
            cfg.consumer_liveness_check_interval = std::chrono::seconds(0);
            auto broker = start_broker_with_cfg(std::move(cfg));

            const std::string ch  = make_test_channel_name("role_state.metrics_cycle");
            const std::string uid = "prod." + ch;

            BrcHandle bh;
            bh.start(broker.endpoint, broker.pubkey, uid);

            auto reg = bh.brc.register_channel(make_reg_opts(ch, uid), 3000);
            ASSERT_TRUE(reg.has_value());

            // One heartbeat -> PendingReady -> Ready, bumps pending_to_ready.
            bh.brc.send_heartbeat(ch, {});

            // Stop heartbeating; wait for Ready -> Pending -> dereg via metrics.
            auto metrics_reclaimed = [&]() {
                auto m = broker.service->query_role_state_metrics();
                return m.pending_to_deregistered_total >= 1;
            };
            ASSERT_TRUE(poll_until(metrics_reclaimed, std::chrono::seconds(3)))
                << "pending_to_deregistered_total did not increment within 3s";

            auto m = broker.service->query_role_state_metrics();
            EXPECT_GE(m.pending_to_ready_total, 1u)
                << "pending_to_ready_total should be >=1 (first heartbeat)";
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
            bh.brc.send_heartbeat(ch, {});  // -> Ready

            // Wait for demotion to Pending.
            auto demoted = [&]() {
                auto m = broker.service->query_role_state_metrics();
                return m.ready_to_pending_total >= 1;
            };
            ASSERT_TRUE(poll_until(demoted, std::chrono::seconds(2)))
                << "Ready -> Pending demotion did not fire";

            // Heartbeat again -> should transition Pending -> Ready (counter +1).
            auto before = broker.service->query_role_state_metrics().pending_to_ready_total;
            bh.brc.send_heartbeat(ch, {});
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
            // Ready timeout irrelevant — role never transitions past PendingReady.
            cfg.ready_timeout_override           = std::chrono::seconds(10);
            cfg.pending_timeout_override         = std::chrono::milliseconds(200);
            cfg.grace_override                   = std::chrono::milliseconds(1);
            cfg.consumer_liveness_check_interval = std::chrono::seconds(0);
            auto broker = start_broker_with_cfg(std::move(cfg));

            const std::string ch  = make_test_channel_name("role_state.stuck_pending");
            const std::string uid = "prod." + ch;

            BrcHandle bh;
            bh.start(broker.endpoint, broker.pubkey, uid);

            auto reg = bh.brc.register_channel(make_reg_opts(ch, uid), 3000);
            ASSERT_TRUE(reg.has_value());
            // Deliberately NO heartbeat — stays PendingReady.

            auto reclaimed = [&]() {
                auto m = broker.service->query_role_state_metrics();
                return m.pending_to_deregistered_total >= 1;
            };
            ASSERT_TRUE(poll_until(reclaimed, std::chrono::seconds(2)))
                << "Stuck-pending role was not reclaimed after pending_timeout";

            auto m = broker.service->query_role_state_metrics();
            EXPECT_EQ(m.pending_to_ready_total, 0u)
                << "Should never have transitioned to Ready (no heartbeat sent)";
            EXPECT_EQ(m.ready_to_pending_total, 0u)
                << "Should not have been demoted (was never Ready)";

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
            const std::string band    = make_test_channel_name("test.band");

            BrcHandle a, b;
            a.start(broker.endpoint, broker.pubkey, uid_a);
            b.start(broker.endpoint, broker.pubkey, uid_b);

            ASSERT_TRUE(a.brc.register_channel(make_reg_opts(ch_a, uid_a), 3000).has_value());
            ASSERT_TRUE(b.brc.register_channel(make_reg_opts(ch_b, uid_b), 3000).has_value());
            a.brc.send_heartbeat(ch_a, {});
            b.brc.send_heartbeat(ch_b, {});

            ASSERT_TRUE(a.brc.band_join(band, 3000).has_value());
            ASSERT_TRUE(b.brc.band_join(band, 3000).has_value());

            auto members1 = b.brc.band_members(band, 3000);
            ASSERT_TRUE(members1.has_value());
            ASSERT_TRUE(members1->contains("members"));
            EXPECT_EQ((*members1)["members"].size(), 2u) << "Expected 2 members after join";

            // Voluntarily deregister A's channel — on_channel_closed fires,
            // cleanup hook removes A from the band.
            EXPECT_TRUE(a.brc.deregister_channel(ch_a));

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
                return 1;
            });
    }
};

static BrokerRoleStateWorkerRegistrar g_broker_role_state_registrar; // NOLINT(cert-err58-cpp)

} // anon
