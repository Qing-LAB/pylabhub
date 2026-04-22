/**
 * @file role_api_raii_workers.cpp
 * @brief Worker bodies for L3 role-API legacy template-RAII integration
 *        tests (Pattern 3).
 *
 * Scope is `DataBlockProducer::with_transaction<F,D>` + `ctx.slots()` +
 * `SlotIterator` — the template RAII layer with no current production
 * callers.  Pending replacement per
 * `docs/tech_draft/raii_layer_redesign.md` Phases 2-5.  Split out from
 * the loop-policy workers so the Group A metrics tests are unaffected
 * by the RAII-layer churn.
 *
 * Why worker subprocess: every body creates `DataBlockProducer`
 * (transitively touching Logger + crypto + DataBlock lifecycle modules).
 * `run_gtest_worker` owns the `LifecycleGuard` for the subprocess lifetime.
 */
#include "role_api_raii_workers.h"

#include "test_datahub_types.h"
#include "plh_datahub.hpp"

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

namespace pylabhub::tests::worker::role_api_raii
{
namespace
{

static auto logger_module() { return utils::Logger::GetLifecycleModule(); }
static auto crypto_module() { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto hub_module()    { return ::pylabhub::hub::GetDataBlockModule(); }

DataBlockConfig make_raii_config(uint64_t secret)
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

} // namespace

// ----------------------------------------------------------------------------
// slot_iterator_fixed_rate_pacing — ctx.slots() respects configured_period_us
// ----------------------------------------------------------------------------
int slot_iterator_fixed_rate_pacing()
{
    return run_gtest_worker(
        [&]()
        {
            const std::string channel = make_test_channel_name("LPIterPacing");
            auto cfg = make_raii_config(80004);

            auto producer = create_datablock_producer<EmptyFlexZone, TestDataBlock>(
                channel, DataBlockPolicy::RingBuffer, cfg);
            ASSERT_NE(producer, nullptr);

            // 30 ms period → 5 iter → 4 inter-iter sleeps → ≥ 120 ms.
            producer->mutable_metrics().set_configured_period(30000);

            const auto t0 = std::chrono::steady_clock::now();

            producer->with_transaction<EmptyFlexZone, TestDataBlock>(
                5000ms,
                [](WriteTransactionContext<EmptyFlexZone, TestDataBlock> &ctx)
                {
                    int count = 0;
                    for (auto &result : ctx.slots(100ms))
                    {
                        if (!result.is_ok())
                            continue;
                        ++count;
                        result.content().get().sequence = static_cast<uint64_t>(count);
                        if (count >= 5)
                            break;  // ~SlotIterator fires auto-publish
                    }
                    EXPECT_EQ(count, 5);
                });

            const auto elapsed = std::chrono::steady_clock::now() - t0;
            EXPECT_GE(elapsed, std::chrono::milliseconds(4 * 30))
                << "pacing lost: 4 inter-iter sleeps at 30ms each should hold";
            EXPECT_LT(elapsed, std::chrono::milliseconds(2000))
                << "something stalled far beyond the 4x30ms minimum";
        },
        "role_api_raii::slot_iterator_fixed_rate_pacing",
        logger_module(), crypto_module(), hub_module());
}

// ----------------------------------------------------------------------------
// ctx_metrics_pass_through — ctx.metrics() aliases producer->metrics()
// ----------------------------------------------------------------------------
int ctx_metrics_pass_through()
{
    return run_gtest_worker(
        [&]()
        {
            const std::string channel = make_test_channel_name("LPCtxPassThrough");
            auto cfg = make_raii_config(80012);

            auto producer = create_datablock_producer<EmptyFlexZone, TestDataBlock>(
                channel, DataBlockPolicy::RingBuffer, cfg);
            ASSERT_NE(producer, nullptr);

            const ContextMetrics *outer_ptr = &producer->metrics();
            const ContextMetrics *inner_ptr = nullptr;

            producer->with_transaction<EmptyFlexZone, TestDataBlock>(
                1000ms,
                [&inner_ptr]
                (WriteTransactionContext<EmptyFlexZone, TestDataBlock> &ctx)
                {
                    inner_ptr = &ctx.metrics();
                });

            ASSERT_NE(inner_ptr, nullptr);
            EXPECT_EQ(inner_ptr, outer_ptr)
                << "ctx.metrics() must alias producer->metrics() Pimpl storage";
        },
        "role_api_raii::ctx_metrics_pass_through",
        logger_module(), crypto_module(), hub_module());
}

// ----------------------------------------------------------------------------
// raii_producer_last_slot_work_us_multi_iter — per-handle regression pin
// ----------------------------------------------------------------------------
/**
 * Key regression pin for the per-handle t_slot_acquired_ fix (HEP-0008 §4.2).
 *
 * Pre-fix bug: `SlotIterator::acquire_next_slot()` acquired the NEW slot
 * (updating `owner->t_iter_start_`) BEFORE the OLD handle's unique_ptr
 * was replaced — firing ~SlotWriteHandle().  `release_write_handle()` used
 * `owner->t_iter_start_`, so it saw the NEW slot's acquire time →
 * last_slot_exec_us ≈ 0 (wrong).
 *
 * Post-fix: each SlotWriteHandle stores its own `t_slot_acquired_` at
 * creation time; `~SlotWriteHandle()` uses the per-handle anchor →
 * correctly records body time.
 */
int raii_producer_last_slot_work_us_multi_iter()
{
    return run_gtest_worker(
        [&]()
        {
            const std::string channel = make_test_channel_name("LPRaiiProdWork");
            auto cfg = make_raii_config(80013);

            auto producer = create_datablock_producer<EmptyFlexZone, TestDataBlock>(
                channel, DataBlockPolicy::RingBuffer, cfg);
            ASSERT_NE(producer, nullptr);

            producer->with_transaction<EmptyFlexZone, TestDataBlock>(
                2000ms,
                [](WriteTransactionContext<EmptyFlexZone, TestDataBlock> &ctx)
                {
                    int count = 0;
                    for (auto &result : ctx.slots(100ms))
                    {
                        if (!result.is_ok())
                            continue;
                        std::this_thread::sleep_for(5ms);
                        result.content().get().sequence =
                            static_cast<uint64_t>(++count);
                        if (count >= 2)
                            break;
                    }
                    EXPECT_EQ(count, 2);
                });

            // Pre-fix: this was ~0 because t_iter_start_ was overwritten
            // by the next acquire before ~SlotWriteHandle fired.
            EXPECT_GE(producer->metrics().last_slot_exec_us_val(), uint64_t{3000})
                << "RAII multi-iter: last_slot_exec_us should reflect ~5ms body";
        },
        "role_api_raii::raii_producer_last_slot_work_us_multi_iter",
        logger_module(), crypto_module(), hub_module());
}

// ----------------------------------------------------------------------------
// raii_producer_metrics_via_slots — RAII slot loop populates metrics
// ----------------------------------------------------------------------------
int raii_producer_metrics_via_slots()
{
    return run_gtest_worker(
        [&]()
        {
            const std::string channel = make_test_channel_name("LPRaiiProdMetrics");
            auto cfg = make_raii_config(80014);

            auto producer = create_datablock_producer<EmptyFlexZone, TestDataBlock>(
                channel, DataBlockPolicy::RingBuffer, cfg);
            ASSERT_NE(producer, nullptr);

            producer->with_transaction<EmptyFlexZone, TestDataBlock>(
                5000ms,
                [](WriteTransactionContext<EmptyFlexZone, TestDataBlock> &ctx)
                {
                    int count = 0;
                    for (auto &result : ctx.slots(100ms))
                    {
                        if (!result.is_ok())
                            continue;
                        result.content().get().sequence =
                            static_cast<uint64_t>(++count);
                        if (count >= 5)
                            break;
                    }
                    EXPECT_EQ(count, 5);
                });

            const auto &m = producer->metrics();
            // After 5 iter via ctx.slots(): timing captures populate.
            // (iteration_count itself lives on RoleHostCore, not on the
            // DataBlock ContextMetrics — this test exercises the queue
            // layer only.)
            EXPECT_GT(m.last_iteration_us_val(), uint64_t{0})
                << "last_iteration_us must populate after 2+ RAII iterations";
            EXPECT_GE(m.max_iteration_us_val(), m.last_iteration_us_val());
            EXPECT_GT(m.context_elapsed_us_val(), uint64_t{0});
        },
        "role_api_raii::raii_producer_metrics_via_slots",
        logger_module(), crypto_module(), hub_module());
}

// ----------------------------------------------------------------------------
// raii_consumer_last_slot_work_us — RAII destructor records exec time
// ----------------------------------------------------------------------------
int raii_consumer_last_slot_work_us()
{
    return run_gtest_worker(
        [&]()
        {
            const std::string channel = make_test_channel_name("LPRaiiConsWork");
            auto cfg = make_raii_config(80016);

            auto producer = create_datablock_producer<EmptyFlexZone, TestDataBlock>(
                channel, DataBlockPolicy::RingBuffer, cfg);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer<EmptyFlexZone, TestDataBlock>(
                channel, cfg.shared_secret, cfg);
            ASSERT_NE(consumer, nullptr);

            {
                auto h = producer->acquire_write_slot(1000);
                ASSERT_TRUE(h);
                (void)h->commit(sizeof(TestDataBlock));
            }

            // Consumer reads via RAII ctx.slots().  ~SlotConsumeHandle()
            // (via SlotIterator destructor on loop exit) records
            // last_slot_exec_us from per-handle t_slot_acquired_.
            consumer->with_transaction<EmptyFlexZone, TestDataBlock>(
                2000ms,
                [](ReadTransactionContext<EmptyFlexZone, TestDataBlock> &ctx)
                {
                    for (auto &result : ctx.slots(100ms))
                    {
                        if (!result.is_ok())
                            continue;
                        std::this_thread::sleep_for(2ms);
                        break;  // RAII: iterator destructor releases handle
                    }
                });

            // Must reflect the ~2ms body sleep, not just "> 0" (a clock
            // tick would satisfy the weak version and hide a regression
            // where the capture went no-op).  Lower bound 1ms leaves
            // headroom for clock granularity on CI.
            EXPECT_GE(consumer->metrics().last_slot_exec_us_val(), uint64_t{1000})
                << "RAII consumer: last_slot_exec_us should reflect ~2ms body";
        },
        "role_api_raii::raii_consumer_last_slot_work_us",
        logger_module(), crypto_module(), hub_module());
}

} // namespace pylabhub::tests::worker::role_api_raii

// ============================================================================
// Worker dispatch registrar
// ============================================================================

namespace
{

struct RoleApiRaiiWorkerRegistrar
{
    RoleApiRaiiWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                const auto dot = mode.find('.');
                if (dot == std::string_view::npos ||
                    mode.substr(0, dot) != "role_api_raii")
                    return -1;
                std::string sc(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::role_api_raii;

                if (sc == "slot_iterator_fixed_rate_pacing")
                    return slot_iterator_fixed_rate_pacing();
                if (sc == "ctx_metrics_pass_through")
                    return ctx_metrics_pass_through();
                if (sc == "raii_producer_last_slot_work_us_multi_iter")
                    return raii_producer_last_slot_work_us_multi_iter();
                if (sc == "raii_producer_metrics_via_slots")
                    return raii_producer_metrics_via_slots();
                if (sc == "raii_consumer_last_slot_work_us")
                    return raii_consumer_last_slot_work_us();

                fmt::print(stderr,
                           "[role_api_raii] ERROR: unknown scenario '{}'\n",
                           sc);
                return 1;
            });
    }
};

static RoleApiRaiiWorkerRegistrar g_role_api_raii_registrar; // NOLINT(cert-err58-cpp)

} // anonymous namespace
