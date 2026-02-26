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
 *   2. ProducerMetricsClear        — clear_metrics() zeroes counters; period_ms survives
 *   3. ProducerFixedRateOverrunDetect — overrun_count > 0 after body sleeps past period_ms
 *   4. SlotIteratorFixedRatePacing — ctx.slots() sleeps maintain start-to-start interval
 *   5. ConsumerMetricsAccumulate   — consumer iteration_count rises with each acquire/release
 *
 * Shared-memory names are generated via make_test_channel_name() (timestamp-unique), so
 * tests can run in parallel (ctest -j2) without collisions.
 *
 * Secret numbers: 80001+ to avoid conflicts with other test suites.
 */
#include "shared_test_helpers.h"
#include "test_datahub_types.h"
#include "plh_datahub.hpp"

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
                pylabhub::hub::GetLifecycleModule()));
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
    ASSERT_EQ(m.iteration_count, uint64_t{5});
    ASSERT_GE(m.max_iteration_us, m.last_iteration_us);
    ASSERT_GE(m.last_slot_wait_us, uint64_t{0});
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
    EXPECT_EQ(producer->metrics().iteration_count, uint64_t{3});

    // Clear and set FixedRate policy
    producer->clear_metrics();
    producer->set_loop_policy(LoopPolicy::FixedRate, 50ms);

    const auto &m = producer->metrics();
    EXPECT_EQ(m.iteration_count, uint64_t{0});
    EXPECT_EQ(m.overrun_count,   uint64_t{0});
    // period_ms is config, not a counter — preserved through clear_metrics()
    EXPECT_EQ(m.period_ms, uint64_t{50});
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

    // period_ms = 1 ms; body sleeps 5 ms → every iteration after the first overruns.
    producer->set_loop_policy(LoopPolicy::FixedRate, 1ms);

    for (int i = 0; i < 5; ++i)
    {
        auto h = producer->acquire_write_slot(1000);
        ASSERT_TRUE(h);
        std::this_thread::sleep_for(5ms); // simulate slow write body
        (void)h->commit(sizeof(TestDataBlock));
    }

    EXPECT_GT(producer->metrics().overrun_count, uint64_t{0});
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

    // Configure 30 ms FixedRate — 5 iterations → 4 inter-iteration sleeps → ≥ 120 ms.
    producer->set_loop_policy(LoopPolicy::FixedRate, 30ms);

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
    ASSERT_EQ(m.iteration_count, uint64_t{3});
    ASSERT_GE(m.last_slot_wait_us, uint64_t{0});
}
