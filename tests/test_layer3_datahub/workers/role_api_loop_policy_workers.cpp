/**
 * @file role_api_loop_policy_workers.cpp
 * @brief Worker bodies for L3 role-API ContextMetrics + loop-overrun
 *        integration tests (Pattern 3).
 *
 * Scope is the DataBlock `ContextMetrics` surface and
 * `RoleHostCore::loop_overrun_count`.  The legacy template-RAII loop
 * lives in `role_api_raii_workers.cpp`.
 *
 * Why worker subprocess: every body creates `DataBlockProducer`
 * (transitively touching Logger + crypto + DataBlock lifecycle modules).
 * `run_gtest_worker` owns the `LifecycleGuard` for the subprocess lifetime.
 */
#include "role_api_loop_policy_workers.h"

#include "test_datahub_types.h"
#include "plh_datahub.hpp"
#include "utils/role_host_core.hpp"

#include "shared_test_helpers.h"
#include "test_entrypoint.h"

#include <fmt/core.h>
#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <thread>

using namespace pylabhub::hub;
using namespace pylabhub::tests;
using namespace pylabhub::tests::helper;
using namespace std::chrono_literals;

namespace pylabhub::tests::worker::role_api_loop_policy
{
namespace
{

/// Lifecycle modules the metric tests need.  Logger for worker-begin/end
/// milestones + the subprocess's own log stream; CryptoUtils +
/// DataBlock for the producer/consumer construction.
static auto logger_module() { return utils::Logger::GetLifecycleModule(); }
static auto crypto_module() { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto hub_module()    { return ::pylabhub::hub::GetDataBlockModule(); }

DataBlockConfig make_lp_config(uint64_t secret)
{
    DataBlockConfig cfg{};
    cfg.policy                = DataBlockPolicy::RingBuffer;
    cfg.consumer_sync_policy  = ConsumerSyncPolicy::Latest_only;
    cfg.shared_secret         = secret;
    cfg.ring_buffer_capacity  = 4;
    cfg.physical_page_size    = DataBlockPageSize::Size4K;
    cfg.flex_zone_size        = sizeof(EmptyFlexZone);
    cfg.checksum_policy       = ChecksumPolicy::None;
    return cfg;
}

/// Construct a DataBlockProducer suitable for Group A metrics tests.
/// Centralises the 4-line boilerplate that otherwise repeats across seven
/// tests: channel name from tag, default Group-A config, factory call,
/// non-null assertion.
///
/// NOTE: ASSERT_NE here is safe only under `throw_on_failure=true`
/// (set by `run_gtest_worker` before the lambda body runs).  Calling this
/// from a plain TEST_F body would silently return nullptr on failure.
std::unique_ptr<DataBlockProducer>
make_metrics_producer(const char *tag, uint64_t secret)
{
    const std::string channel = make_test_channel_name(tag);
    auto cfg = make_lp_config(secret);
    auto producer = create_datablock_producer<EmptyFlexZone, TestDataBlock>(
        channel, DataBlockPolicy::RingBuffer, cfg);
    [&]() { ASSERT_NE(producer, nullptr); }();
    return producer;
}

} // namespace

// ----------------------------------------------------------------------------
// producer_metrics_accumulate — DEEPENED
// ----------------------------------------------------------------------------
int producer_metrics_accumulate()
{
    return run_gtest_worker(
        [&]()
        {
            auto producer = make_metrics_producer("LPMetricsAccum", 80001);

            constexpr int kWrites = 5;
            for (int i = 0; i < kWrites; ++i)
            {
                auto h = producer->acquire_write_slot(1000);
                ASSERT_TRUE(h) << "acquire_write_slot failed at i=" << i;
                (void)h->commit(sizeof(TestDataBlock));
            }

            const auto &m = producer->metrics();
            // ContextMetrics does not track iteration count directly —
            // that counter lives on RoleHostCore.  Here we verify the
            // per-iteration timing captures populate after multiple
            // acquires: last_iteration_us gets a measurement from iter 2
            // onward; max ≥ last is the monotonic invariant; elapsed
            // must advance since context_start_time anchored on iter 1.
            EXPECT_GT(m.last_iteration_us_val(), uint64_t{0})
                << "last_iteration_us must populate after ≥ 2 acquires";
            EXPECT_GE(m.max_iteration_us_val(), m.last_iteration_us_val());
            EXPECT_GT(m.context_elapsed_us_val(), uint64_t{0})
                << "context_elapsed_us must advance after ≥ 1 acquire";
        },
        "role_api_loop_policy::producer_metrics_accumulate",
        logger_module(), crypto_module(), hub_module());
}

// ----------------------------------------------------------------------------
// producer_metrics_clear — DEEPENED
// ----------------------------------------------------------------------------
int producer_metrics_clear()
{
    return run_gtest_worker(
        [&]()
        {
            auto producer = make_metrics_producer("LPMetricsClear", 80002);

            // Set configured_period FIRST so the post-clear assertion
            // actually verifies that clear_metrics preserves the
            // config-vs-counter distinction (configured_period_us is
            // config: must survive clear; all other ContextMetrics
            // fields are counters: must zero).
            producer->mutable_metrics().set_configured_period(50000);

            for (int i = 0; i < 3; ++i)
            {
                auto h = producer->acquire_write_slot(1000);
                ASSERT_TRUE(h);
                (void)h->commit(sizeof(TestDataBlock));
            }

            // Pre-clear: the per-iteration timing captures must have
            // populated (3 writes → at least last_iteration_us and
            // context_elapsed_us are non-zero).
            {
                const auto &m = producer->metrics();
                EXPECT_GT(m.last_iteration_us_val(), uint64_t{0});
                EXPECT_GT(m.context_elapsed_us_val(), uint64_t{0});
                EXPECT_EQ(m.configured_period_us_val(), uint64_t{50000});
            }

            producer->clear_metrics();

            const auto &m = producer->metrics();
            // Counters zeroed.
            EXPECT_EQ(m.last_iteration_us_val(),       uint64_t{0});
            EXPECT_EQ(m.max_iteration_us_val(),        uint64_t{0});
            EXPECT_EQ(m.last_slot_exec_us_val(),       uint64_t{0});
            EXPECT_EQ(m.last_slot_wait_us_val(),       uint64_t{0});
            EXPECT_EQ(m.context_elapsed_us_val(),      uint64_t{0});
            // Config preserved across clear — this is the key distinction.
            EXPECT_EQ(m.configured_period_us_val(),    uint64_t{50000})
                << "clear_metrics must preserve configured_period (config, not counter)";
        },
        "role_api_loop_policy::producer_metrics_clear",
        logger_module(), crypto_module(), hub_module());
}

// ----------------------------------------------------------------------------
// loop_overrun_count_increment_and_accumulate — deterministic unit test
// ----------------------------------------------------------------------------
int loop_overrun_count_increment_and_accumulate()
{
    return run_gtest_worker(
        [&]()
        {
            // RoleHostCore::loop_overrun_count is the post-HEP-0008-Pass-3
            // location for deadline-overrun tracking (moved off the
            // DataBlock metrics).  Deterministic: no timing dependency —
            // we just drive the counter directly.
            pylabhub::scripting::RoleHostCore core;
            EXPECT_EQ(core.loop_overrun_count(), 0u);

            core.inc_loop_overrun();
            EXPECT_EQ(core.loop_overrun_count(), 1u);

            core.inc_loop_overrun();
            core.inc_loop_overrun();
            EXPECT_EQ(core.loop_overrun_count(), 3u);
        },
        "role_api_loop_policy::loop_overrun_count_increment_and_accumulate",
        logger_module());
}

// ----------------------------------------------------------------------------
// consumer_metrics_accumulate — DEEPENED
// ----------------------------------------------------------------------------
int consumer_metrics_accumulate()
{
    return run_gtest_worker(
        [&]()
        {
            const std::string channel = make_test_channel_name("LPConsumerMetrics");
            auto cfg = make_lp_config(80005);

            auto producer = create_datablock_producer<EmptyFlexZone, TestDataBlock>(
                channel, DataBlockPolicy::RingBuffer, cfg);
            ASSERT_NE(producer, nullptr);

            auto consumer = find_datablock_consumer<EmptyFlexZone, TestDataBlock>(
                channel, cfg.shared_secret, cfg);
            ASSERT_NE(consumer, nullptr);

            constexpr int kCycles = 3;
            for (int i = 0; i < kCycles; ++i)
            {
                auto wh = producer->acquire_write_slot(1000);
                ASSERT_TRUE(wh) << "producer acquire_write_slot failed at i=" << i;
                (void)wh->commit(sizeof(TestDataBlock));
                (void)producer->release_write_slot(*wh);

                auto rh = consumer->acquire_consume_slot(1000);
                ASSERT_TRUE(rh) << "consumer acquire_consume_slot failed at i=" << i;
                (void)consumer->release_consume_slot(*rh);
            }

            const auto &pm = producer->metrics();
            const auto &cm = consumer->metrics();

            // After ≥ 2 cycles on each side, per-iteration timing captures
            // must populate on both.  (iteration_count itself lives on
            // RoleHostCore; this test exercises ContextMetrics only.)
            EXPECT_GT(pm.last_iteration_us_val(), uint64_t{0})
                << "producer last_iteration_us must populate after 2+ writes";
            EXPECT_GT(cm.last_iteration_us_val(), uint64_t{0})
                << "consumer last_iteration_us must populate after 2+ reads";
            // Both contexts must have elapsed time.
            EXPECT_GT(pm.context_elapsed_us_val(), uint64_t{0});
            EXPECT_GT(cm.context_elapsed_us_val(), uint64_t{0});
        },
        "role_api_loop_policy::consumer_metrics_accumulate",
        logger_module(), crypto_module(), hub_module());
}

// ----------------------------------------------------------------------------
// zero_on_creation — baseline
// ----------------------------------------------------------------------------
int zero_on_creation()
{
    return run_gtest_worker(
        [&]()
        {
            auto producer = make_metrics_producer("LPZeroOnCreation", 80006);

            const auto &m = producer->metrics();
            // All ContextMetrics fields start at zero.  Note: iteration
            // count lives on RoleHostCore, not here — this test covers
            // the ContextMetrics surface only.
            EXPECT_EQ(m.last_slot_wait_us_val(),  uint64_t{0});
            EXPECT_EQ(m.last_iteration_us_val(),   uint64_t{0});
            EXPECT_EQ(m.max_iteration_us_val(),    uint64_t{0});
            EXPECT_EQ(m.last_slot_exec_us_val(),   uint64_t{0});
            EXPECT_EQ(m.context_elapsed_us_val(),  uint64_t{0});
            EXPECT_EQ(m.configured_period_us_val(), uint64_t{0});
            EXPECT_EQ(m.context_start_time_val(), ContextMetrics::Clock::time_point{});
        },
        "role_api_loop_policy::zero_on_creation",
        logger_module(), crypto_module(), hub_module());
}

// ----------------------------------------------------------------------------
// max_rate_metrics_period_zero — renamed from MaxRateNoOverrun (deepened)
// ----------------------------------------------------------------------------
int max_rate_metrics_period_zero()
{
    return run_gtest_worker(
        [&]()
        {
            auto producer = make_metrics_producer("LPMaxRate", 80007);

            // MaxRate ≡ configured_period_us == 0 (default).  Body sleeps
            // that would overrun FixedRate still produce valid metrics —
            // the iteration_count + per-slot timing work regardless.
            for (int i = 0; i < 3; ++i)
            {
                auto h = producer->acquire_write_slot(1000);
                ASSERT_TRUE(h);
                std::this_thread::sleep_for(5ms);
                (void)h->commit(sizeof(TestDataBlock));
            }

            const auto &m = producer->metrics();
            // MaxRate pins configured_period_us == 0 (the defining
            // property of the policy at the metrics layer).
            EXPECT_EQ(m.configured_period_us_val(), uint64_t{0});
            // last_slot_exec_us reflects our 5ms body sleep → ≥ 3000 µs.
            // With 3 writes, this captures the last one's body time.
            EXPECT_GE(m.last_slot_exec_us_val(),     uint64_t{3000})
                << "5ms body sleep should land ≥ 3ms in last_slot_exec_us";
            // Iteration timing populated after ≥ 2 acquires.
            EXPECT_GT(m.last_iteration_us_val(),     uint64_t{0});
            EXPECT_GT(m.context_elapsed_us_val(),    uint64_t{0});
        },
        "role_api_loop_policy::max_rate_metrics_period_zero",
        logger_module(), crypto_module(), hub_module());
}

// ----------------------------------------------------------------------------
// last_slot_work_us_populated — single-slot timing capture
// ----------------------------------------------------------------------------
int last_slot_work_us_populated()
{
    return run_gtest_worker(
        [&]()
        {
            auto producer = make_metrics_producer("LPLastSlotWork", 80008);

            {
                auto h = producer->acquire_write_slot(1000);
                ASSERT_TRUE(h);
                std::this_thread::sleep_for(2ms);  // measurable body time
                (void)h->commit(sizeof(TestDataBlock));
                // ~SlotWriteHandle() records last_slot_exec_us on scope exit.
            }

            EXPECT_GT(producer->metrics().last_slot_exec_us_val(), uint64_t{0});
        },
        "role_api_loop_policy::last_slot_work_us_populated",
        logger_module(), crypto_module(), hub_module());
}

// ----------------------------------------------------------------------------
// last_iteration_us_populated — two-acquire delta capture
// ----------------------------------------------------------------------------
int last_iteration_us_populated()
{
    return run_gtest_worker(
        [&]()
        {
            auto producer = make_metrics_producer("LPLastIter", 80009);

            // First acquire sets the anchor; second produces the first
            // last_iteration_us measurement.
            for (int i = 0; i < 2; ++i)
            {
                auto h = producer->acquire_write_slot(1000);
                ASSERT_TRUE(h);
                (void)h->commit(sizeof(TestDataBlock));
            }

            EXPECT_GT(producer->metrics().last_iteration_us_val(), uint64_t{0});
        },
        "role_api_loop_policy::last_iteration_us_populated",
        logger_module(), crypto_module(), hub_module());
}

// ----------------------------------------------------------------------------
// max_iteration_us_peak — max is monotonic non-decreasing
// ----------------------------------------------------------------------------
int max_iteration_us_peak()
{
    return run_gtest_worker(
        [&]()
        {
            auto producer = make_metrics_producer("LPMaxIterPeak", 80010);

            // Iteration 1: fast (anchor).
            {
                auto h = producer->acquire_write_slot(1000);
                ASSERT_TRUE(h);
                (void)h->commit(sizeof(TestDataBlock));
            }

            // Inject a delay → iteration 2 gets a large inter-arrival.
            std::this_thread::sleep_for(5ms);

            // Iteration 2: slow (drives peak).
            {
                auto h = producer->acquire_write_slot(1000);
                ASSERT_TRUE(h);
                (void)h->commit(sizeof(TestDataBlock));
            }
            const uint64_t peak_after_2 = producer->metrics().max_iteration_us_val();
            // The 5 ms sleep between iter 1 and iter 2 drives
            // last_iteration_us for iter 2 ≥ 5 ms; the peak captures that.
            // Using a generous lower bound (3 ms) to tolerate clock-tick
            // granularity on CI while still catching any bug that
            // recorded the peak at near-zero (e.g. clock source misread
            // or field overwrite by a later short iteration).
            EXPECT_GE(peak_after_2, uint64_t{3000})
                << "peak should reflect 5ms inter-iter sleep";

            // Iteration 3: fast — last_iteration_us smaller, but max must
            // NEVER decrease.
            {
                auto h = producer->acquire_write_slot(1000);
                ASSERT_TRUE(h);
                (void)h->commit(sizeof(TestDataBlock));
            }

            const auto &m = producer->metrics();
            EXPECT_GE(m.max_iteration_us_val(), peak_after_2)
                << "max_iteration_us must be monotonic non-decreasing";
            EXPECT_GE(m.max_iteration_us_val(), m.last_iteration_us_val());
        },
        "role_api_loop_policy::max_iteration_us_peak",
        logger_module(), crypto_module(), hub_module());
}

// ----------------------------------------------------------------------------
// context_elapsed_us_monotonic — wall-time accumulator
// ----------------------------------------------------------------------------
int context_elapsed_us_monotonic()
{
    return run_gtest_worker(
        [&]()
        {
            auto producer = make_metrics_producer("LPContextElapsed", 80011);

            {
                auto h = producer->acquire_write_slot(1000);
                ASSERT_TRUE(h);
                (void)h->commit(sizeof(TestDataBlock));
            }
            const uint64_t elapsed_1 = producer->metrics().context_elapsed_us_val();

            std::this_thread::sleep_for(2ms);

            {
                auto h = producer->acquire_write_slot(1000);
                ASSERT_TRUE(h);
                (void)h->commit(sizeof(TestDataBlock));
            }
            const uint64_t elapsed_2 = producer->metrics().context_elapsed_us_val();

            EXPECT_GE(elapsed_2, elapsed_1)
                << "context_elapsed_us must be non-decreasing";
            // 2 ms sleep + second acquire should advance elapsed by
            // ≥ 1 ms — lower bound leaves headroom for the clock
            // source granularity but catches "elapsed advances by
            // nanoseconds only" (clock misconfigured or misconverted).
            EXPECT_GE(elapsed_2 - elapsed_1, uint64_t{1000})
                << "elapsed should advance ≥ 1ms after 2ms sleep";
            EXPECT_GT(elapsed_2, elapsed_1)
                << "after a 2ms sleep + second acquire, elapsed should advance";
        },
        "role_api_loop_policy::context_elapsed_us_monotonic",
        logger_module(), crypto_module(), hub_module());
}

} // namespace pylabhub::tests::worker::role_api_loop_policy

// ============================================================================
// Worker dispatch registrar
// ============================================================================

namespace
{

struct RoleApiLoopPolicyWorkerRegistrar
{
    RoleApiLoopPolicyWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                const auto dot = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "role_api_loop_policy")
                    return -1;
                std::string sc(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::role_api_loop_policy;

                if (sc == "producer_metrics_accumulate")
                    return producer_metrics_accumulate();
                if (sc == "producer_metrics_clear")
                    return producer_metrics_clear();
                if (sc == "loop_overrun_count_increment_and_accumulate")
                    return loop_overrun_count_increment_and_accumulate();
                if (sc == "consumer_metrics_accumulate")
                    return consumer_metrics_accumulate();
                if (sc == "zero_on_creation")
                    return zero_on_creation();
                if (sc == "max_rate_metrics_period_zero")
                    return max_rate_metrics_period_zero();
                if (sc == "last_slot_work_us_populated")
                    return last_slot_work_us_populated();
                if (sc == "last_iteration_us_populated")
                    return last_iteration_us_populated();
                if (sc == "max_iteration_us_peak")
                    return max_iteration_us_peak();
                if (sc == "context_elapsed_us_monotonic")
                    return context_elapsed_us_monotonic();

                fmt::print(stderr,
                           "[role_api_loop_policy] ERROR: unknown scenario '{}'\n",
                           sc);
                return 1;
            });
    }
};

static RoleApiLoopPolicyWorkerRegistrar g_role_api_loop_policy_registrar; // NOLINT(cert-err58-cpp)

} // anonymous namespace
