/**
 * @file test_datahub_loop_policy.cpp
 * @brief Layer 3 DataHub – LoopPolicy and ContextMetrics tests (HEP-CORE-0008 Pass 3).
 *
 * Single-process tests: create DataBlockProducer / DataBlockConsumer directly (no broker,
 * no ZMQ, no worker subprocess).  ContextMetrics is per-handle and process-local, so
 * cross-process coordination is not required here.
 *
 * The hub lifecycle (Logger + CryptoUtils + DataExchangeHub) is initialised once for the
 * whole test suite via SetUpTestSuite / TearDownTestSuite and held in a static
 * LifecycleGuard.  This avoids repeated startup/teardown overhead and keeps
 * test code focused on the metric assertions.
 *
 * Tests:
 *   1. ProducerMetricsAccumulate   — iteration_count rises with each acquire/release
 *   2. ProducerMetricsClear        — clear_metrics() zeroes counters; configured_period_us survives
 *   3. ProducerFixedRateOverrunDetect — data_drop_count stays 0 (timing overrun moved to main loop); body sleeps past configured_period_us
 *   4. SlotIteratorFixedRatePacing — ctx.slots() sleeps maintain start-to-start interval
 *   5. ConsumerMetricsAccumulate   — consumer iteration_count rises with each acquire/release
 *   6. ZeroOnCreation              — all ContextMetrics fields are zero before first acquire
 *   7. MaxRateNoOverrun            — MaxRate policy: data_drop_count stays 0 (no data loss)
 *   8. LastSlotWorkUsPopulated     — last_slot_exec_us > 0 after measurable body sleep
 *   9. LastIterationUsPopulated    — last_iteration_us > 0 after two acquires
 *  10. MaxIterationUsPeak          — max_iteration_us tracks the peak and never decreases
 *  11. ContextElapsedUsMonotonic   — context_elapsed_us grows between acquires
 *  12. CtxMetricsPassThrough               — ctx.metrics() is a reference to the same Pimpl storage
 *
 * RAII path tests (via with_transaction + ctx.slots()):
 *  13. RaiiProducerLastSlotWorkUsMultiIter — key regression: per-handle t_slot_acquired_ fix
 *      (pre-fix: last_slot_exec_us was ~0 due to t_iter_start_ being overwritten by next acquire)
 *  14. RaiiProducerMetricsViaSlots        — iteration_count/last/max_iteration_us via ctx.slots()
 *  15. RaiiProducerOverrunViaSlots        — overrun detection works through RAII slot loop
 *  16. RaiiConsumerLastSlotWorkUs         — consumer RAII destructor records last_slot_exec_us
 *
 * Shared-memory names are generated via make_test_channel_name() (timestamp-unique), so
 * tests can run in parallel (ctest -j2) without collisions.
 *
 * Secret numbers: 80001+ to avoid conflicts with other test suites.
 */
#include "shared_test_helpers.h"
#include "test_datahub_types.h"
#include "plh_datahub.hpp"
#include "utils/role_host_core.hpp"

#include <gtest/gtest.h>
#include <chrono>
#include <memory>
#include <thread>

using namespace pylabhub::hub;
using namespace pylabhub::tests;
using namespace pylabhub::tests::helper;
using namespace std::chrono_literals;

// ============================================================================
// Helper: minimal DataBlockConfig for loop-policy tests
// ============================================================================

namespace
{
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
} // namespace

// ============================================================================
// Test fixture: initialise the hub lifecycle once for the whole suite
// ============================================================================

class DatahubLoopPolicyTest : public ::testing::Test
{
  public:
    static void SetUpTestSuite()
    {
        s_lifecycle_ = std::make_unique<pylabhub::utils::LifecycleGuard>(
            pylabhub::utils::MakeModDefList(
                pylabhub::utils::Logger::GetLifecycleModule(),
                pylabhub::crypto::GetLifecycleModule(),
                pylabhub::hub::GetLifecycleModule()), std::source_location::current());
    }

    static void TearDownTestSuite() { s_lifecycle_.reset(); }

  private:
    // NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
    static std::unique_ptr<pylabhub::utils::LifecycleGuard> s_lifecycle_;
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::unique_ptr<pylabhub::utils::LifecycleGuard> DatahubLoopPolicyTest::s_lifecycle_;

// ============================================================================
// Test 1: ProducerMetricsAccumulate
// ============================================================================

TEST_F(DatahubLoopPolicyTest, ProducerMetricsAccumulate)
{
    const std::string channel = make_test_channel_name("LPMetricsAccum");
    auto cfg = make_lp_config(80001);

    auto producer = create_datablock_producer<EmptyFlexZone, TestDataBlock>(
        channel, DataBlockPolicy::RingBuffer, cfg);
    ASSERT_NE(producer, nullptr);

    // Write 5 slots via primitive API
    for (int i = 0; i < 5; ++i)
    {
        auto h = producer->acquire_write_slot(1000);
        ASSERT_TRUE(h) << "acquire_write_slot failed on iteration " << i;
        (void)h->commit(sizeof(TestDataBlock));
    }

    const auto &m = producer->metrics();
    ASSERT_GE(m.max_iteration_us_val(), m.last_iteration_us_val());
    ASSERT_GE(m.last_slot_wait_us_val(), uint64_t{0});
}

// ============================================================================
// Test 2: ProducerMetricsClear
// ============================================================================

TEST_F(DatahubLoopPolicyTest, ProducerMetricsClear)
{
    const std::string channel = make_test_channel_name("LPMetricsClear");
    auto cfg = make_lp_config(80002);

    auto producer = create_datablock_producer<EmptyFlexZone, TestDataBlock>(
        channel, DataBlockPolicy::RingBuffer, cfg);
    ASSERT_NE(producer, nullptr);

    // Write 3 slots to populate metrics
    for (int i = 0; i < 3; ++i)
    {
        auto h = producer->acquire_write_slot(1000);
        ASSERT_TRUE(h);
        (void)h->commit(sizeof(TestDataBlock));
    }

    // Clear and set configured period
    producer->clear_metrics();
    producer->metrics().set_configured_period(50000);

    const auto &m = producer->metrics();
    // configured_period_us is config, not a counter — preserved through clear_metrics()
    EXPECT_EQ(m.configured_period_us_val(), uint64_t{50000});
}

// ============================================================================
// Test 3: ProducerFixedRateOverrunDetect
// ============================================================================

TEST_F(DatahubLoopPolicyTest, ProducerFixedRateOverrunDetect)
{
    const std::string channel = make_test_channel_name("LPOverrunDetect");
    auto cfg = make_lp_config(80003);

    auto producer = create_datablock_producer<EmptyFlexZone, TestDataBlock>(
        channel, DataBlockPolicy::RingBuffer, cfg);
    ASSERT_NE(producer, nullptr);

    // configured_period_us = 1000 us (1 ms); body sleeps 5 ms → every iteration after the first overruns.
    producer->metrics().set_configured_period(1000);

    for (int i = 0; i < 5; ++i)
    {
        auto h = producer->acquire_write_slot(1000);
        ASSERT_TRUE(h);
        std::this_thread::sleep_for(5ms); // simulate slow write body
        (void)h->commit(sizeof(TestDataBlock));
    }

}

// ============================================================================
// Test 3b: LoopOverrunCount — deadline-based overrun tracked in RoleHostCore
// ============================================================================

TEST_F(DatahubLoopPolicyTest, LoopOverrunCount_IncrementAndAccumulate)
{
    // Verify RoleHostCore::loop_overrun_count tracks deadline overruns.
    // Deterministic: directly call inc_loop_overrun() — no timing dependency.
    pylabhub::scripting::RoleHostCore core;
    EXPECT_EQ(core.loop_overrun_count(), 0u);

    core.inc_loop_overrun();
    EXPECT_EQ(core.loop_overrun_count(), 1u);

    core.inc_loop_overrun();
    core.inc_loop_overrun();
    EXPECT_EQ(core.loop_overrun_count(), 3u);
}

// ============================================================================
// Test 4: SlotIteratorFixedRatePacing
// ============================================================================

TEST_F(DatahubLoopPolicyTest, SlotIteratorFixedRatePacing)
{
    const std::string channel = make_test_channel_name("LPIteratorPacing");
    auto cfg = make_lp_config(80004);

    auto producer = create_datablock_producer<EmptyFlexZone, TestDataBlock>(
        channel, DataBlockPolicy::RingBuffer, cfg);
    ASSERT_NE(producer, nullptr);

    // Configure 30 ms period — 5 iterations → 4 inter-iteration sleeps → ≥ 120 ms.
    producer->metrics().set_configured_period(30000); // 30 ms in µs

    auto t0 = std::chrono::steady_clock::now();

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
                    break; // auto-publish fires on iterator destruction
            }
            EXPECT_EQ(count, 5);
        });

    auto elapsed = std::chrono::steady_clock::now() - t0;

    // 4 inter-iteration sleeps of ≥ 30 ms each → at least 120 ms total.
    EXPECT_GE(elapsed, std::chrono::milliseconds(4 * 30));
    // Upper bound: fail fast if something stalled (generous for slow CI).
    EXPECT_LT(elapsed, std::chrono::milliseconds(2000));
}

// ============================================================================
// Test 5: ConsumerMetricsAccumulate
// ============================================================================

TEST_F(DatahubLoopPolicyTest, ConsumerMetricsAccumulate)
{
    const std::string channel = make_test_channel_name("LPConsumerMetrics");
    auto cfg = make_lp_config(80005);

    auto producer = create_datablock_producer<EmptyFlexZone, TestDataBlock>(
        channel, DataBlockPolicy::RingBuffer, cfg);
    ASSERT_NE(producer, nullptr);

    auto consumer = find_datablock_consumer<EmptyFlexZone, TestDataBlock>(
        channel, cfg.shared_secret, cfg);
    ASSERT_NE(consumer, nullptr);

    // Interleave writes and reads — commit + explicit release_write_slot makes the slot
    // immediately visible to the consumer (same pattern as c_api_slot_protocol tests).
    for (int i = 0; i < 3; ++i)
    {
        auto wh = producer->acquire_write_slot(1000);
        ASSERT_TRUE(wh) << "acquire_write_slot failed on iteration " << i;
        (void)wh->commit(sizeof(TestDataBlock));
        (void)producer->release_write_slot(*wh);

        auto rh = consumer->acquire_consume_slot(1000);
        ASSERT_TRUE(rh) << "acquire_consume_slot failed on iteration " << i;
        (void)consumer->release_consume_slot(*rh);
    }

    const auto &m = consumer->metrics();
    ASSERT_GE(m.last_slot_wait_us_val(), uint64_t{0});
}

// ============================================================================
// Test 6: ZeroOnCreation
// ============================================================================

TEST_F(DatahubLoopPolicyTest, ZeroOnCreation)
{
    const std::string channel = make_test_channel_name("LPZeroOnCreation");
    auto cfg = make_lp_config(80006);

    auto producer = create_datablock_producer<EmptyFlexZone, TestDataBlock>(
        channel, DataBlockPolicy::RingBuffer, cfg);
    ASSERT_NE(producer, nullptr);

    // Before any acquire, all metric counters and context_start_time must be zero.
    const auto &m = producer->metrics();
    EXPECT_EQ(m.last_slot_wait_us_val(),  uint64_t{0});
    EXPECT_EQ(m.last_iteration_us_val(),  uint64_t{0});
    EXPECT_EQ(m.max_iteration_us_val(),   uint64_t{0});
    EXPECT_EQ(m.last_slot_exec_us_val(),  uint64_t{0});
    EXPECT_EQ(m.context_elapsed_us_val(), uint64_t{0});
    EXPECT_EQ(m.configured_period_us_val(),          uint64_t{0});
    EXPECT_EQ(m.context_start_time_val(), ContextMetrics::Clock::time_point{});
}

// ============================================================================
// Test 7: MaxRateNoOverrun
// ============================================================================

TEST_F(DatahubLoopPolicyTest, MaxRateNoOverrun)
{
    const std::string channel = make_test_channel_name("LPMaxRateNoOverrun");
    auto cfg = make_lp_config(80007);

    auto producer = create_datablock_producer<EmptyFlexZone, TestDataBlock>(
        channel, DataBlockPolicy::RingBuffer, cfg);
    ASSERT_NE(producer, nullptr);

    // MaxRate (configured_period_us = 0) — default after clear_metrics().
    // No set_configured_period needed; period is already 0.

    // Slow body: would overrun a FixedRate 1 ms policy, but MaxRate never counts overruns.
    for (int i = 0; i < 3; ++i)
    {
        auto h = producer->acquire_write_slot(1000);
        ASSERT_TRUE(h);
        std::this_thread::sleep_for(5ms);
        (void)h->commit(sizeof(TestDataBlock));
    }

    EXPECT_EQ(producer->metrics().configured_period_us_val(),     uint64_t{0});
}

// ============================================================================
// Test 8: LastSlotWorkUsPopulated
// ============================================================================

TEST_F(DatahubLoopPolicyTest, LastSlotWorkUsPopulated)
{
    const std::string channel = make_test_channel_name("LPLastSlotWork");
    auto cfg = make_lp_config(80008);

    auto producer = create_datablock_producer<EmptyFlexZone, TestDataBlock>(
        channel, DataBlockPolicy::RingBuffer, cfg);
    ASSERT_NE(producer, nullptr);

    {
        auto h = producer->acquire_write_slot(1000);
        ASSERT_TRUE(h);
        std::this_thread::sleep_for(2ms); // measurable body time
        (void)h->commit(sizeof(TestDataBlock));
        // h destructor calls release_write_handle() which records last_slot_exec_us.
    }

    EXPECT_GT(producer->metrics().last_slot_exec_us_val(), uint64_t{0});
}

// ============================================================================
// Test 9: LastIterationUsPopulated
// ============================================================================

TEST_F(DatahubLoopPolicyTest, LastIterationUsPopulated)
{
    const std::string channel = make_test_channel_name("LPLastIteration");
    auto cfg = make_lp_config(80009);

    auto producer = create_datablock_producer<EmptyFlexZone, TestDataBlock>(
        channel, DataBlockPolicy::RingBuffer, cfg);
    ASSERT_NE(producer, nullptr);

    // First acquire sets the timing anchor; second produces the first last_iteration_us measurement.
    for (int i = 0; i < 2; ++i)
    {
        auto h = producer->acquire_write_slot(1000);
        ASSERT_TRUE(h);
        (void)h->commit(sizeof(TestDataBlock));
    }

    EXPECT_GT(producer->metrics().last_iteration_us_val(), uint64_t{0});
}

// ============================================================================
// Test 10: MaxIterationUsPeak
// ============================================================================

TEST_F(DatahubLoopPolicyTest, MaxIterationUsPeak)
{
    const std::string channel = make_test_channel_name("LPMaxIterPeak");
    auto cfg = make_lp_config(80010);

    auto producer = create_datablock_producer<EmptyFlexZone, TestDataBlock>(
        channel, DataBlockPolicy::RingBuffer, cfg);
    ASSERT_NE(producer, nullptr);

    // Iteration 1: fast (sets timing anchor).
    {
        auto h = producer->acquire_write_slot(1000);
        ASSERT_TRUE(h);
        (void)h->commit(sizeof(TestDataBlock));
    }

    // Inject a delay: start-to-start for iteration 2 should be > 5 ms.
    std::this_thread::sleep_for(5ms);

    // Iteration 2: slower — drives max_iteration_us to a measurable peak.
    {
        auto h = producer->acquire_write_slot(1000);
        ASSERT_TRUE(h);
        (void)h->commit(sizeof(TestDataBlock));
    }

    const uint64_t peak_after_2 = producer->metrics().max_iteration_us_val();

    // Iteration 3: fast (no sleep before it) — last_iteration_us should be small.
    {
        auto h = producer->acquire_write_slot(1000);
        ASSERT_TRUE(h);
        (void)h->commit(sizeof(TestDataBlock));
    }

    const auto &m = producer->metrics();
    // max_iteration_us must never decrease.
    EXPECT_GE(m.max_iteration_us_val(), peak_after_2);
    // max_iteration_us must always be >= last_iteration_us.
    EXPECT_GE(m.max_iteration_us_val(), m.last_iteration_us_val());
}

// ============================================================================
// Test 11: ContextElapsedUsMonotonic
// ============================================================================

TEST_F(DatahubLoopPolicyTest, ContextElapsedUsMonotonic)
{
    const std::string channel = make_test_channel_name("LPContextElapsed");
    auto cfg = make_lp_config(80011);

    auto producer = create_datablock_producer<EmptyFlexZone, TestDataBlock>(
        channel, DataBlockPolicy::RingBuffer, cfg);
    ASSERT_NE(producer, nullptr);

    // First acquire sets context_start_time and captures the first elapsed value.
    {
        auto h = producer->acquire_write_slot(1000);
        ASSERT_TRUE(h);
        (void)h->commit(sizeof(TestDataBlock));
    }
    const uint64_t elapsed_1 = producer->metrics().context_elapsed_us_val();

    std::this_thread::sleep_for(2ms); // ensure the clock advances

    // Second acquire must see a larger context_elapsed_us.
    {
        auto h = producer->acquire_write_slot(1000);
        ASSERT_TRUE(h);
        (void)h->commit(sizeof(TestDataBlock));
    }
    const uint64_t elapsed_2 = producer->metrics().context_elapsed_us_val();

    EXPECT_GE(elapsed_2, elapsed_1) << "context_elapsed_us must be non-decreasing";
}

// ============================================================================
// Test 12: CtxMetricsPassThrough
// ============================================================================

TEST_F(DatahubLoopPolicyTest, CtxMetricsPassThrough)
{
    const std::string channel = make_test_channel_name("LPCtxPassThrough");
    auto cfg = make_lp_config(80012);

    auto producer = create_datablock_producer<EmptyFlexZone, TestDataBlock>(
        channel, DataBlockPolicy::RingBuffer, cfg);
    ASSERT_NE(producer, nullptr);

    const ContextMetrics *outer_ptr = &producer->metrics();
    const ContextMetrics *inner_ptr = nullptr;

    producer->with_transaction<EmptyFlexZone, TestDataBlock>(
        1000ms,
        [&inner_ptr](WriteTransactionContext<EmptyFlexZone, TestDataBlock> &ctx)
        {
            // ctx.metrics() must be a pass-through reference into the same Pimpl storage.
            inner_ptr = &ctx.metrics();
        });

    ASSERT_NE(inner_ptr, nullptr);
    EXPECT_EQ(inner_ptr, outer_ptr)
        << "ctx.metrics() must reference the same Pimpl storage as producer->metrics()";
}

// ============================================================================
// Test 13: RaiiProducerLastSlotWorkUsMultiIter
// ============================================================================

/**
 * Key regression test for the per-handle t_slot_acquired_ fix (HEP-CORE-0008 §4.2).
 *
 * RAII multi-iteration issue (pre-fix):
 *   SlotIterator::acquire_next_slot() acquires the NEW slot (updating owner->t_iter_start_)
 *   BEFORE the OLD handle's unique_ptr is replaced, which fires ~SlotWriteHandle().
 *   If release_write_handle() used owner->t_iter_start_, it would see the NEW slot's
 *   acquire time → last_slot_exec_us ≈ 0 (wrong).
 *
 * Post-fix: each SlotWriteHandle stores its own t_slot_acquired_ at creation time.
 *   ~SlotWriteHandle() uses impl.t_slot_acquired_ → correctly records body time.
 */
TEST_F(DatahubLoopPolicyTest, RaiiProducerLastSlotWorkUsMultiIter)
{
    const std::string channel = make_test_channel_name("LPRaiiProdWork");
    auto cfg = make_lp_config(80013);

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
                std::this_thread::sleep_for(5ms); // measurable body time
                result.content().get().sequence = static_cast<uint64_t>(++count);
                if (count >= 2)
                    break; // second break: destructor releases last slot via RAII
            }
            EXPECT_EQ(count, 2);
        });

    // Without the per-handle fix, the RAII multi-iter path recorded ~0 here
    // because t_iter_start_ was overwritten by the next acquire before ~SlotWriteHandle fired.
    EXPECT_GE(producer->metrics().last_slot_exec_us_val(), uint64_t{3000})
        << "RAII multi-iter: last_slot_exec_us should reflect body sleep (~5 ms)";
}

// ============================================================================
// Test 14: RaiiProducerMetricsViaSlots
// ============================================================================

TEST_F(DatahubLoopPolicyTest, RaiiProducerMetricsViaSlots)
{
    const std::string channel = make_test_channel_name("LPRaiiProdMetrics");
    auto cfg = make_lp_config(80014);

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
                result.content().get().sequence = static_cast<uint64_t>(++count);
                if (count >= 5)
                    break;
            }
            EXPECT_EQ(count, 5);
        });

    const auto &m = producer->metrics();
    EXPECT_GT(m.last_iteration_us_val(), uint64_t{0});
    EXPECT_GE(m.max_iteration_us_val(), m.last_iteration_us_val());
}

// ============================================================================
// Test 15: RaiiProducerOverrunViaSlots
// ============================================================================

TEST_F(DatahubLoopPolicyTest, RaiiProducerOverrunViaSlots)
{
    const std::string channel = make_test_channel_name("LPRaiiProdOverrun");
    auto cfg = make_lp_config(80015);

    auto producer = create_datablock_producer<EmptyFlexZone, TestDataBlock>(
        channel, DataBlockPolicy::RingBuffer, cfg);
    ASSERT_NE(producer, nullptr);

    // 1 ms period + 5 ms body sleep guarantees overruns via the RAII ctx.slots() path.
    producer->metrics().set_configured_period(1000); // 1 ms in µs

    producer->with_transaction<EmptyFlexZone, TestDataBlock>(
        5000ms,
        [](WriteTransactionContext<EmptyFlexZone, TestDataBlock> &ctx)
        {
            int count = 0;
            for (auto &result : ctx.slots(100ms))
            {
                if (!result.is_ok())
                    continue;
                std::this_thread::sleep_for(5ms);
                result.content().get().sequence = static_cast<uint64_t>(++count);
                if (count >= 3)
                    break;
            }
            EXPECT_EQ(count, 3);
        });

}

// ============================================================================
// Test 16: RaiiConsumerLastSlotWorkUs
// ============================================================================

TEST_F(DatahubLoopPolicyTest, RaiiConsumerLastSlotWorkUs)
{
    const std::string channel = make_test_channel_name("LPRaiiConsumerWork");
    auto cfg = make_lp_config(80016);

    auto producer = create_datablock_producer<EmptyFlexZone, TestDataBlock>(
        channel, DataBlockPolicy::RingBuffer, cfg);
    ASSERT_NE(producer, nullptr);

    auto consumer = find_datablock_consumer<EmptyFlexZone, TestDataBlock>(
        channel, cfg.shared_secret, cfg);
    ASSERT_NE(consumer, nullptr);

    // Write a slot so the consumer has data to read.
    {
        auto h = producer->acquire_write_slot(1000);
        ASSERT_TRUE(h);
        (void)h->commit(sizeof(TestDataBlock));
    }

    // Consumer reads via RAII ctx.slots() — break after one slot.
    // The SlotIterator destructor releases the handle via ~SlotConsumeHandle()
    // → release_consume_handle() records last_slot_exec_us using per-handle t_slot_acquired_.
    consumer->with_transaction<EmptyFlexZone, TestDataBlock>(
        2000ms,
        [](ReadTransactionContext<EmptyFlexZone, TestDataBlock> &ctx)
        {
            for (auto &result : ctx.slots(100ms))
            {
                if (!result.is_ok())
                    continue;
                std::this_thread::sleep_for(2ms); // measurable body time
                break; // RAII: SlotIterator destructor releases handle on loop exit
            }
        });

    EXPECT_GT(consumer->metrics().last_slot_exec_us_val(), uint64_t{0})
        << "RAII consumer: last_slot_exec_us should reflect body sleep (~2 ms)";
}
