/**
 * @file hub_health_example.cpp
 * @brief Example: Hub health monitoring callbacks.
 *
 * Demonstrates the two-category error taxonomy and the corresponding
 * notification callbacks on Producer and Consumer.
 *
 * Error taxonomy:
 *  Cat 1 — Serious invariant violations (broker shuts down the channel):
 *    - Heartbeat timeout (producer silently died)
 *    - Schema mismatch on channel re-registration
 *  Cat 2 — Application-level issues (notify only; configurable policy):
 *    - Consumer process died without clean deregistration
 *    - Slot checksum errors (optional; reported by Cat2 API)
 *
 * Callback wiring (auto-connected by Producer::create and Consumer::connect):
 *
 *   Producer::on_channel_closing  — Cat 1: broker heartbeat timeout expired.
 *   Producer::on_consumer_died    — Cat 2: broker detected dead consumer PID.
 *   Producer::on_channel_error    — Cat 1: schema mismatch; Cat 2: forwarded events.
 *   Consumer::on_channel_closing  — Cat 1: producer timed out / channel closed.
 *   Consumer::on_channel_error    — Cat 1: schema mismatch notification (forwarded).
 *
 * This example configures a fast timeout (1 s) and consumer liveness check (1 s)
 * so that Cat 1 / Cat 2 scenarios trigger quickly for demonstration purposes.
 */
#include "plh_datahub.hpp"
#include "utils/broker_service.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <thread>

using namespace pylabhub::hub;
using namespace pylabhub::utils;
using namespace std::chrono_literals;

// ─── Shared data types ────────────────────────────────────────────────────────

struct StatusFlexZone
{
    uint64_t sequence{0};
    bool     active{true};
    uint32_t _pad{0};
};
static_assert(std::is_trivially_copyable_v<StatusFlexZone>);

PYLABHUB_SCHEMA_BEGIN(StatusFlexZone)
    PYLABHUB_SCHEMA_MEMBER(sequence)
    PYLABHUB_SCHEMA_MEMBER(active)
PYLABHUB_SCHEMA_END(StatusFlexZone)

struct SampleData
{
    uint64_t timestamp_ns{0};
    float    value{0.0f};
    uint32_t seq{0};
};
static_assert(std::is_trivially_copyable_v<SampleData>);

PYLABHUB_SCHEMA_BEGIN(SampleData)
    PYLABHUB_SCHEMA_MEMBER(timestamp_ns)
    PYLABHUB_SCHEMA_MEMBER(value)
    PYLABHUB_SCHEMA_MEMBER(seq)
PYLABHUB_SCHEMA_END(SampleData)

// ─── Helper: start broker in background thread ────────────────────────────────

struct BrokerHandle
{
    pylabhub::broker::BrokerService service;
    std::thread                     thread;

    explicit BrokerHandle(pylabhub::broker::BrokerService::Config cfg)
        : service(std::move(cfg))
    {
        thread = std::thread([this] { service.run(); });
    }

    ~BrokerHandle()
    {
        service.stop();
        if (thread.joinable())
            thread.join();
    }

    BrokerHandle(const BrokerHandle&) = delete;
    BrokerHandle& operator=(const BrokerHandle&) = delete;
};

// ─── Demo 1: Cat 1 — heartbeat timeout → producer on_channel_closing ──────────

void demo_cat1_heartbeat_timeout()
{
    std::cout << "\n=== Cat 1: Heartbeat Timeout ===\n";
    std::cout << "(Broker channel_timeout = 1s; producer stops heartbeating)\n";

    std::string ep, pk;
    std::mutex  mu;
    std::condition_variable cv;
    bool ready = false;

    pylabhub::broker::BrokerService::Config cfg;
    cfg.endpoint        = "tcp://127.0.0.1:0";
    cfg.channel_timeout = 1s; // very short for demo
    cfg.on_ready        = [&](const std::string& e, const std::string& p)
    {
        std::lock_guard lock(mu);
        ep = e; pk = p; ready = true;
        cv.notify_all();
    };

    BrokerHandle broker(std::move(cfg));
    {
        std::unique_lock lock(mu);
        cv.wait_for(lock, 3s, [&] { return ready; });
    }

    Messenger msg;
    msg.connect(ep, pk);

    ProducerOptions opts;
    opts.channel_name = "health_demo";
    opts.pattern      = ChannelPattern::PubSub;
    opts.has_shm      = false; // ZMQ-only for this demo

    auto prod_opt = Producer::create(msg, opts);
    if (!prod_opt)
    {
        std::cerr << "Failed to create producer\n";
        return;
    }
    Producer& prod = *prod_opt;

    // ── Register health callback ──────────────────────────────────────────────
    std::atomic<bool> closing_fired{false};
    prod.on_channel_closing([&closing_fired]()
    {
        std::cout << "[producer] >>> on_channel_closing fired! (Cat 1: heartbeat timeout)\n";
        closing_fired = true;
    });

    prod.start();
    std::cout << "[producer] Running — will stop() to halt heartbeats...\n";

    // Stop the producer (halts heartbeats to the broker) but keep it alive
    // so the callback can fire. In production, on_channel_closing would trigger
    // a graceful shutdown or re-registration attempt.
    std::this_thread::sleep_for(200ms);
    prod.stop(); // peer_thread and write_thread stop; Messenger heartbeat continues via
                 // its own timer — to truly stop heartbeats, call prod.close() which
                 // calls unregister_channel(). Here we just close() to trigger timeout demo.
    prod.close();

    // Wait for broker to detect the missing heartbeats (channel_timeout = 1s).
    for (int i = 0; i < 30 && !closing_fired.load(); ++i)
        std::this_thread::sleep_for(100ms);

    if (closing_fired)
        std::cout << "[demo] Cat 1 heartbeat timeout demonstrated OK\n";
    else
        std::cout << "[demo] (Callback not fired in window — increase wait if needed)\n";
}

// ─── Demo 2: Cat 2 — consumer liveness → producer on_consumer_died ────────────

void demo_cat2_dead_consumer()
{
    std::cout << "\n=== Cat 2: Dead Consumer Detection ===\n";
    std::cout << "(consumer_liveness_check_interval = 1s; consumer closes abruptly)\n";

    std::string ep, pk;
    std::mutex  mu;
    std::condition_variable cv;
    bool ready = false;

    pylabhub::broker::BrokerService::Config cfg;
    cfg.endpoint                         = "tcp://127.0.0.1:0";
    cfg.channel_timeout                  = 30s; // long; we're testing consumer liveness
    cfg.consumer_liveness_check_interval = 1s;
    cfg.on_ready = [&](const std::string& e, const std::string& p)
    {
        std::lock_guard lock(mu);
        ep = e; pk = p; ready = true;
        cv.notify_all();
    };

    BrokerHandle broker(std::move(cfg));
    {
        std::unique_lock lock(mu);
        cv.wait_for(lock, 3s, [&] { return ready; });
    }

    // Producer Messenger.
    Messenger prod_msg;
    prod_msg.connect(ep, pk);

    ProducerOptions popts;
    popts.channel_name = "liveness_demo";
    popts.pattern      = ChannelPattern::PubSub;
    popts.has_shm      = false;

    auto prod_opt = Producer::create(prod_msg, popts);
    if (!prod_opt) { std::cerr << "Producer failed\n"; return; }
    Producer& prod = *prod_opt;

    // ── Cat 2 callback ────────────────────────────────────────────────────────
    std::atomic<bool> died_fired{false};
    prod.on_consumer_died(
        [&died_fired](uint64_t pid, const std::string& reason)
        {
            std::cout << "[producer] >>> on_consumer_died fired! "
                      << "pid=" << pid << " reason=" << reason << "\n";
            died_fired = true;
        });
    prod.start();

    // Consumer Messenger.
    Messenger cons_msg;
    cons_msg.connect(ep, pk);

    ConsumerOptions copts;
    copts.channel_name = "liveness_demo";

    auto cons_opt = Consumer::connect(cons_msg, copts);
    if (!cons_opt) { std::cerr << "Consumer failed\n"; prod.close(); return; }
    Consumer& cons = *cons_opt;
    cons.start();

    std::this_thread::sleep_for(200ms);

    // Simulate an abrupt consumer exit: close() sends CONSUMER_DEREG_REQ cleanly.
    // For a true "dead" simulation (no deregistration), a child process would
    // call _exit(0). Here we call close() to demonstrate the CONSUMER_DIED_NOTIFY
    // pathway via liveness check — in practice this requires a real OS PID check.
    // (The multi-process DeadConsumerDetected test in test_datahub_broker_health.cpp
    //  covers the _exit(0) scenario end-to-end.)
    std::cout << "[consumer] Deregistering cleanly (CONSUMER_DEREG_REQ sent to broker)\n";
    cons.close();

    std::cout << "[producer] Consumer closed — Cat 2 liveness check shows pid alive\n";
    std::cout << "           (For true dead-PID demo, see DeadConsumerDetected test)\n";

    prod.close();
}

// ─── Demo 3: Cat 1 — schema mismatch → producer on_channel_error ─────────────

void demo_cat1_schema_mismatch()
{
    std::cout << "\n=== Cat 1: Schema Mismatch Notification ===\n";
    std::cout << "(Producer B tries same channel with different schema)\n";

    std::string ep, pk;
    std::mutex  mu;
    std::condition_variable cv;
    bool ready = false;

    pylabhub::broker::BrokerService::Config cfg;
    cfg.endpoint = "tcp://127.0.0.1:0";
    cfg.on_ready = [&](const std::string& e, const std::string& p)
    {
        std::lock_guard lock(mu);
        ep = e; pk = p; ready = true;
        cv.notify_all();
    };

    BrokerHandle broker(std::move(cfg));
    {
        std::unique_lock lock(mu);
        cv.wait_for(lock, 3s, [&] { return ready; });
    }

    // Producer A: creates the channel with typed schema (StatusFlexZone, SampleData).
    Messenger msg_a;
    msg_a.connect(ep, pk);

    ProducerOptions opts_a;
    opts_a.channel_name = "schema_demo";
    opts_a.pattern      = ChannelPattern::PubSub;
    opts_a.has_shm      = false;

    auto prod_a_opt = Producer::create<StatusFlexZone, SampleData>(msg_a, opts_a);
    if (!prod_a_opt) { std::cerr << "Producer A failed\n"; return; }
    Producer& prod_a = *prod_a_opt;

    // ── Register Cat 1 schema error callback on Producer A ────────────────────
    std::atomic<bool> error_fired{false};
    prod_a.on_channel_error(
        [&error_fired](const std::string& event, const nlohmann::json& details)
        {
            std::cout << "[producer A] >>> on_channel_error fired!\n"
                      << "              event  = " << event << "\n"
                      << "              detail = " << details.dump() << "\n";
            error_fired = true;
        });

    prod_a.start();
    std::cout << "[producer A] Channel 'schema_demo' created with StatusFlexZone/SampleData\n";

    // Producer B: tries same channel with a DIFFERENT schema (SampleData as FlexZone).
    // Broker rejects it and sends CHANNEL_ERROR_NOTIFY to Producer A.
    Messenger msg_b;
    msg_b.connect(ep, pk);

    ProducerOptions opts_b;
    opts_b.channel_name = "schema_demo";
    opts_b.pattern      = ChannelPattern::PubSub;
    opts_b.has_shm      = false;

    // Use different types — this gives a different schema hash.
    auto prod_b_opt = Producer::create<SampleData, StatusFlexZone>(msg_b, opts_b);
    // prod_b_opt will be nullopt — broker rejects the conflicting registration.
    if (prod_b_opt)
    {
        std::cout << "[producer B] (Unexpectedly succeeded — schema hashes collided?)\n";
        prod_b_opt->close();
    }
    else
    {
        std::cout << "[producer B] Rejected by broker (schema mismatch) — expected\n";
    }

    // Give Messenger A's worker thread time to receive CHANNEL_ERROR_NOTIFY.
    for (int i = 0; i < 20 && !error_fired.load(); ++i)
        std::this_thread::sleep_for(100ms);

    if (error_fired)
        std::cout << "[demo] Cat 1 schema mismatch notification demonstrated OK\n";
    else
        std::cout << "[demo] (CHANNEL_ERROR_NOTIFY not received in window)\n";

    prod_a.close();
}

// ─── Main ─────────────────────────────────────────────────────────────────────

int main()
{
    LifecycleGuard lifecycle(MakeModDefList(
        Logger::GetLifecycleModule(),
        pylabhub::crypto::GetLifecycleModule()));

    demo_cat1_heartbeat_timeout();
    demo_cat2_dead_consumer();
    demo_cat1_schema_mismatch();

    std::cout << "\nAll health demos complete\n";
    return 0;
}
