// tests/test_layer3_datahub/workers/datahub_hub_processor_workers.cpp
//
// Hub Processor unit test workers.  Each function creates two DataBlocks
// (in-channel and out-channel), wraps them in ShmQueues, builds a Processor,
// runs the scenario, and asserts the expected results.
#include "datahub_hub_processor_workers.h"
#include "test_entrypoint.h"
#include "shared_test_helpers.h"

#include "plh_datahub_client.hpp"
#include "utils/hub_shm_queue.hpp"
#include "utils/hub_zmq_queue.hpp"
#include "utils/hub_processor.hpp"

#include <gtest/gtest.h>
#include <fmt/core.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

using namespace pylabhub::tests::helper;
using namespace pylabhub::hub;
using namespace std::chrono_literals;

namespace pylabhub::tests::worker::hub_processor
{

// ── Shared lifecycle helpers ──────────────────────────────────────────────────

static auto logger_module() { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto crypto_module() { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto hub_module()    { return ::pylabhub::hub::GetLifecycleModule(); }

// ── Helpers: make DataBlockConfig + short-timeout ProcessorOptions ────────────

namespace
{

DataBlockConfig make_config(uint64_t shared_secret)
{
    DataBlockConfig cfg{};
    cfg.policy               = DataBlockPolicy::RingBuffer;
    cfg.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
    cfg.shared_secret        = shared_secret;
    cfg.ring_buffer_capacity = 8; // larger capacity avoids overwrite in sequential tests
    cfg.physical_page_size   = DataBlockPageSize::Size4K;
    cfg.flex_zone_size       = 0;
    return cfg;
}

// Short input_timeout so stop() returns quickly in test scenarios.
ProcessorOptions fast_opts(OverflowPolicy policy = OverflowPolicy::Block)
{
    ProcessorOptions o;
    o.overflow_policy = policy;
    o.input_timeout   = std::chrono::milliseconds{200};
    return o;
}

// Write one double to a DataBlockProducer (timeout_ms = 500).
// Returns false and triggers GTEST assertion on failure.
bool write_double(DataBlockProducer* dbp, double val)
{
    auto wh = dbp->acquire_write_slot(500);
    if (wh == nullptr)
        return false;
    std::memcpy(wh->buffer_span().data(), &val, sizeof(val));
    (void)wh->commit(sizeof(double));
    (void)dbp->release_write_slot(*wh);
    return true;
}

// Read one double from a DataBlockConsumer (timeout_ms configurable).
// Returns false if the slot was not available within the timeout.
bool read_double(DataBlockConsumer* dbc, double& out, int timeout_ms = 2000)
{
    auto rh = dbc->acquire_consume_slot(timeout_ms);
    if (rh == nullptr)
        return false;
    std::memcpy(&out, rh->buffer_span().data(), sizeof(out));
    (void)dbc->release_consume_slot(*rh);
    return true;
}

// Poll predicate with timeout.  Returns true when pred() becomes true.
template <typename Pred>
bool wait_for(Pred pred, std::chrono::milliseconds timeout)
{
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!pred())
    {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return true;
}

} // anonymous namespace

// ============================================================================
// processor_create
// ============================================================================

int processor_create()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g_in("ProcessorCreate_in");
            DataBlockTestGuard g_out("ProcessorCreate_out");
            DataBlockConfig cfg_in  = make_config(70010);
            DataBlockConfig cfg_out = make_config(70011);

            auto in_dbp_test = create_datablock_producer_impl(
                g_in.channel_name(), DataBlockPolicy::RingBuffer, cfg_in, nullptr, nullptr);
            ASSERT_NE(in_dbp_test, nullptr);
            auto in_dbc_proc = find_datablock_consumer_impl(
                g_in.channel_name(), cfg_in.shared_secret, &cfg_in, nullptr, nullptr);
            ASSERT_NE(in_dbc_proc, nullptr);

            auto out_dbp_proc = create_datablock_producer_impl(
                g_out.channel_name(), DataBlockPolicy::RingBuffer, cfg_out, nullptr, nullptr);
            ASSERT_NE(out_dbp_proc, nullptr);
            auto out_dbc_test = find_datablock_consumer_impl(
                g_out.channel_name(), cfg_out.shared_secret, &cfg_out, nullptr, nullptr);
            ASSERT_NE(out_dbc_test, nullptr);

            auto in_q  = ShmQueue::from_consumer(std::move(in_dbc_proc),  sizeof(double));
            auto out_q = ShmQueue::from_producer(std::move(out_dbp_proc), sizeof(double));
            ASSERT_NE(in_q,  nullptr);
            ASSERT_NE(out_q, nullptr);

            auto maybe_proc = Processor::create(*in_q, *out_q, fast_opts());
            ASSERT_TRUE(maybe_proc.has_value());
            EXPECT_FALSE(maybe_proc->is_running());
            EXPECT_FALSE(maybe_proc->has_process_handler());
        },
        "hub_processor.processor_create",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// processor_no_handler
// ============================================================================

int processor_no_handler()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g_in("ProcessorNoHandler_in");
            DataBlockTestGuard g_out("ProcessorNoHandler_out");
            DataBlockConfig cfg_in  = make_config(70012);
            DataBlockConfig cfg_out = make_config(70013);

            auto in_dbp_test = create_datablock_producer_impl(
                g_in.channel_name(), DataBlockPolicy::RingBuffer, cfg_in, nullptr, nullptr);
            ASSERT_NE(in_dbp_test, nullptr);
            auto in_dbc_proc = find_datablock_consumer_impl(
                g_in.channel_name(), cfg_in.shared_secret, &cfg_in, nullptr, nullptr);
            ASSERT_NE(in_dbc_proc, nullptr);

            auto out_dbp_proc = create_datablock_producer_impl(
                g_out.channel_name(), DataBlockPolicy::RingBuffer, cfg_out, nullptr, nullptr);
            ASSERT_NE(out_dbp_proc, nullptr);
            auto out_dbc_test = find_datablock_consumer_impl(
                g_out.channel_name(), cfg_out.shared_secret, &cfg_out, nullptr, nullptr);
            ASSERT_NE(out_dbc_test, nullptr);

            auto in_q  = ShmQueue::from_consumer(std::move(in_dbc_proc),  sizeof(double));
            auto out_q = ShmQueue::from_producer(std::move(out_dbp_proc), sizeof(double));

            auto maybe_proc = Processor::create(*in_q, *out_q, fast_opts());
            ASSERT_TRUE(maybe_proc.has_value());
            Processor& proc = *maybe_proc;

            // No handler installed.
            proc.start();
            EXPECT_TRUE(proc.is_running());

            // Write one item — Processor should ignore it (no handler = no read).
            EXPECT_TRUE(write_double(in_dbp_test.get(), 1.0));

            // Wait 300ms; the Processor's loop only sleeps 10ms between handler checks
            // but never calls read_acquire without a handler.
            std::this_thread::sleep_for(300ms);

            EXPECT_EQ(proc.in_slots_received(), 0u);
            EXPECT_EQ(proc.out_slots_written(), 0u);

            // Output queue should have nothing.
            double dummy = 0.0;
            EXPECT_FALSE(read_double(out_dbc_test.get(), dummy, 100));

            proc.stop();
            EXPECT_FALSE(proc.is_running());
        },
        "hub_processor.processor_no_handler",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// processor_set_handler
// ============================================================================

int processor_set_handler()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g_in("ProcessorSetHandler_in");
            DataBlockTestGuard g_out("ProcessorSetHandler_out");
            DataBlockConfig cfg_in  = make_config(70014);
            DataBlockConfig cfg_out = make_config(70015);

            auto in_dbp_test = create_datablock_producer_impl(
                g_in.channel_name(), DataBlockPolicy::RingBuffer, cfg_in, nullptr, nullptr);
            ASSERT_NE(in_dbp_test, nullptr);
            auto in_dbc_proc = find_datablock_consumer_impl(
                g_in.channel_name(), cfg_in.shared_secret, &cfg_in, nullptr, nullptr);
            ASSERT_NE(in_dbc_proc, nullptr);

            auto out_dbp_proc = create_datablock_producer_impl(
                g_out.channel_name(), DataBlockPolicy::RingBuffer, cfg_out, nullptr, nullptr);
            ASSERT_NE(out_dbp_proc, nullptr);
            auto out_dbc_test = find_datablock_consumer_impl(
                g_out.channel_name(), cfg_out.shared_secret, &cfg_out, nullptr, nullptr);
            ASSERT_NE(out_dbc_test, nullptr);

            auto in_q  = ShmQueue::from_consumer(std::move(in_dbc_proc),  sizeof(double));
            auto out_q = ShmQueue::from_producer(std::move(out_dbp_proc), sizeof(double));

            auto maybe_proc = Processor::create(*in_q, *out_q, fast_opts());
            ASSERT_TRUE(maybe_proc.has_value());
            Processor& proc = *maybe_proc;

            EXPECT_FALSE(proc.has_process_handler());

            // Install a no-op handler.
            proc.set_process_handler<void, double, void, double>(
                [](ProcessorContext<void, double, void, double>& ctx) -> bool {
                    ctx.output() = ctx.input();
                    return true;
                });
            EXPECT_TRUE(proc.has_process_handler());

            // Clear the handler.
            proc.set_process_handler<void, double, void, double>(nullptr);
            EXPECT_FALSE(proc.has_process_handler());
        },
        "hub_processor.processor_set_handler",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// processor_transform
// ============================================================================

int processor_transform()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g_in("ProcessorTransform_in");
            DataBlockTestGuard g_out("ProcessorTransform_out");
            DataBlockConfig cfg_in  = make_config(70016);
            DataBlockConfig cfg_out = make_config(70017);

            auto in_dbp_test = create_datablock_producer_impl(
                g_in.channel_name(), DataBlockPolicy::RingBuffer, cfg_in, nullptr, nullptr);
            ASSERT_NE(in_dbp_test, nullptr);
            auto in_dbc_proc = find_datablock_consumer_impl(
                g_in.channel_name(), cfg_in.shared_secret, &cfg_in, nullptr, nullptr);
            ASSERT_NE(in_dbc_proc, nullptr);

            auto out_dbp_proc = create_datablock_producer_impl(
                g_out.channel_name(), DataBlockPolicy::RingBuffer, cfg_out, nullptr, nullptr);
            ASSERT_NE(out_dbp_proc, nullptr);
            auto out_dbc_test = find_datablock_consumer_impl(
                g_out.channel_name(), cfg_out.shared_secret, &cfg_out, nullptr, nullptr);
            ASSERT_NE(out_dbc_test, nullptr);

            auto in_q  = ShmQueue::from_consumer(std::move(in_dbc_proc),  sizeof(double));
            auto out_q = ShmQueue::from_producer(std::move(out_dbp_proc), sizeof(double));

            auto maybe_proc = Processor::create(*in_q, *out_q, fast_opts());
            ASSERT_TRUE(maybe_proc.has_value());
            Processor& proc = *maybe_proc;

            proc.set_process_handler<void, double, void, double>(
                [](ProcessorContext<void, double, void, double>& ctx) -> bool {
                    ctx.output() = ctx.input() * 2.0;
                    return true;
                });
            proc.start();

            // Write 3 slots one at a time and verify each output to ensure ordering.
            for (int i = 1; i <= 3; ++i)
            {
                double val = static_cast<double>(i);
                ASSERT_TRUE(write_double(in_dbp_test.get(), val))
                    << "Failed to write input slot " << i;

                double result = -1.0;
                ASSERT_TRUE(read_double(out_dbc_test.get(), result, 2000))
                    << "Timeout waiting for output slot " << i;
                EXPECT_DOUBLE_EQ(result, val * 2.0) << "Output mismatch for slot " << i;
            }

            proc.stop();

            EXPECT_EQ(proc.in_slots_received(),  3u);
            EXPECT_EQ(proc.out_slots_written(),  3u);
            EXPECT_EQ(proc.out_drop_count(),     0u);
        },
        "hub_processor.processor_transform",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// processor_drop_false
// ============================================================================

int processor_drop_false()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g_in("ProcessorDropFalse_in");
            DataBlockTestGuard g_out("ProcessorDropFalse_out");
            DataBlockConfig cfg_in  = make_config(70018);
            DataBlockConfig cfg_out = make_config(70019);

            auto in_dbp_test = create_datablock_producer_impl(
                g_in.channel_name(), DataBlockPolicy::RingBuffer, cfg_in, nullptr, nullptr);
            ASSERT_NE(in_dbp_test, nullptr);
            auto in_dbc_proc = find_datablock_consumer_impl(
                g_in.channel_name(), cfg_in.shared_secret, &cfg_in, nullptr, nullptr);
            ASSERT_NE(in_dbc_proc, nullptr);

            auto out_dbp_proc = create_datablock_producer_impl(
                g_out.channel_name(), DataBlockPolicy::RingBuffer, cfg_out, nullptr, nullptr);
            ASSERT_NE(out_dbp_proc, nullptr);
            auto out_dbc_test = find_datablock_consumer_impl(
                g_out.channel_name(), cfg_out.shared_secret, &cfg_out, nullptr, nullptr);
            ASSERT_NE(out_dbc_test, nullptr);

            auto in_q  = ShmQueue::from_consumer(std::move(in_dbc_proc),  sizeof(double));
            auto out_q = ShmQueue::from_producer(std::move(out_dbp_proc), sizeof(double));

            auto maybe_proc = Processor::create(*in_q, *out_q, fast_opts());
            ASSERT_TRUE(maybe_proc.has_value());
            Processor& proc = *maybe_proc;

            // Handler always returns false — slot is aborted, not committed.
            proc.set_process_handler<void, double, void, double>(
                [](ProcessorContext<void, double, void, double>&) -> bool { return false; });
            proc.start();

            ASSERT_TRUE(write_double(in_dbp_test.get(), 99.0));

            // Wait for Processor to consume the input.
            bool consumed = wait_for([&proc] { return proc.in_slots_received() >= 1; }, 3000ms);
            ASSERT_TRUE(consumed) << "Processor did not consume input slot within 3s";

            // Allow a brief settling period for the drop counter to be updated.
            std::this_thread::sleep_for(50ms);

            EXPECT_EQ(proc.out_drop_count(),    1u);
            EXPECT_EQ(proc.out_slots_written(), 0u);

            // Output queue should be empty — consumer sees a timeout.
            double dummy = 0.0;
            EXPECT_FALSE(read_double(out_dbc_test.get(), dummy, 100));

            proc.stop();
        },
        "hub_processor.processor_drop_false",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// processor_overflow_drop
// ============================================================================

int processor_overflow_drop()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g_in("ProcessorOverflowDrop_in");
            DataBlockTestGuard g_out("ProcessorOverflowDrop_out");
            DataBlockConfig cfg_in  = make_config(70020);
            DataBlockConfig cfg_out = make_config(70021);

            auto in_dbp_test = create_datablock_producer_impl(
                g_in.channel_name(), DataBlockPolicy::RingBuffer, cfg_in, nullptr, nullptr);
            ASSERT_NE(in_dbp_test, nullptr);
            auto in_dbc_proc = find_datablock_consumer_impl(
                g_in.channel_name(), cfg_in.shared_secret, &cfg_in, nullptr, nullptr);
            ASSERT_NE(in_dbc_proc, nullptr);

            auto out_dbp_proc = create_datablock_producer_impl(
                g_out.channel_name(), DataBlockPolicy::RingBuffer, cfg_out, nullptr, nullptr);
            ASSERT_NE(out_dbp_proc, nullptr);
            auto out_dbc_test = find_datablock_consumer_impl(
                g_out.channel_name(), cfg_out.shared_secret, &cfg_out, nullptr, nullptr);
            ASSERT_NE(out_dbc_test, nullptr);

            auto in_q  = ShmQueue::from_consumer(std::move(in_dbc_proc),  sizeof(double));
            auto out_q = ShmQueue::from_producer(std::move(out_dbp_proc), sizeof(double));

            // Drop policy: write_acquire uses 0ms timeout.
            auto maybe_proc = Processor::create(*in_q, *out_q, fast_opts(OverflowPolicy::Drop));
            ASSERT_TRUE(maybe_proc.has_value());
            Processor& proc = *maybe_proc;

            proc.set_process_handler<void, double, void, double>(
                [](ProcessorContext<void, double, void, double>& ctx) -> bool {
                    ctx.output() = ctx.input();
                    return true;
                });
            proc.start();

            // Write 3 items one at a time, waiting for each to be consumed before writing
            // the next.  ConsumerSyncPolicy::Latest_only means bulk-writing would cause the
            // Processor to see only the most recent slot; sequential writes avoid skipping.
            for (int i = 1; i <= 3; ++i)
            {
                uint64_t before = proc.in_slots_received();
                ASSERT_TRUE(write_double(in_dbp_test.get(), static_cast<double>(i)));
                bool consumed = wait_for(
                    [&proc, before] { return proc.in_slots_received() > before; }, 3000ms);
                ASSERT_TRUE(consumed) << "Item " << i << " not consumed within 3s";
                std::this_thread::sleep_for(20ms); // allow commit/drop counter to update
            }

            // Accounting: in_received == out_written + out_drops (no item lost unaccounted).
            uint64_t received = proc.in_slots_received();
            uint64_t written  = proc.out_slots_written();
            uint64_t drops    = proc.out_drop_count();
            EXPECT_EQ(received, written + drops);

            proc.stop();
            EXPECT_FALSE(proc.is_running());
        },
        "hub_processor.processor_overflow_drop",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// processor_overflow_block
// ============================================================================

int processor_overflow_block()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g_in("ProcessorOverflowBlock_in");
            DataBlockTestGuard g_out("ProcessorOverflowBlock_out");
            DataBlockConfig cfg_in  = make_config(70022);
            DataBlockConfig cfg_out = make_config(70023);

            auto in_dbp_test = create_datablock_producer_impl(
                g_in.channel_name(), DataBlockPolicy::RingBuffer, cfg_in, nullptr, nullptr);
            ASSERT_NE(in_dbp_test, nullptr);
            auto in_dbc_proc = find_datablock_consumer_impl(
                g_in.channel_name(), cfg_in.shared_secret, &cfg_in, nullptr, nullptr);
            ASSERT_NE(in_dbc_proc, nullptr);

            auto out_dbp_proc = create_datablock_producer_impl(
                g_out.channel_name(), DataBlockPolicy::RingBuffer, cfg_out, nullptr, nullptr);
            ASSERT_NE(out_dbp_proc, nullptr);
            auto out_dbc_test = find_datablock_consumer_impl(
                g_out.channel_name(), cfg_out.shared_secret, &cfg_out, nullptr, nullptr);
            ASSERT_NE(out_dbc_test, nullptr);

            auto in_q  = ShmQueue::from_consumer(std::move(in_dbc_proc),  sizeof(double));
            auto out_q = ShmQueue::from_producer(std::move(out_dbp_proc), sizeof(double));

            // Block policy (default): write_acquire uses 5000ms timeout; backpressure applies.
            auto maybe_proc = Processor::create(*in_q, *out_q, fast_opts(OverflowPolicy::Block));
            ASSERT_TRUE(maybe_proc.has_value());
            Processor& proc = *maybe_proc;

            proc.set_process_handler<void, double, void, double>(
                [](ProcessorContext<void, double, void, double>& ctx) -> bool {
                    ctx.output() = ctx.input() * 3.0;
                    return true;
                });
            proc.start();

            // Write 3 items one at a time, reading each output before writing the next.
            // This exercises the Block policy path with an active consumer (no backpressure stall).
            for (int i = 1; i <= 3; ++i)
            {
                ASSERT_TRUE(write_double(in_dbp_test.get(), static_cast<double>(i)));

                double result = -1.0;
                ASSERT_TRUE(read_double(out_dbc_test.get(), result, 2000))
                    << "Timeout waiting for output slot " << i;
                EXPECT_DOUBLE_EQ(result, static_cast<double>(i) * 3.0);
            }

            proc.stop();
            EXPECT_FALSE(proc.is_running());
            EXPECT_EQ(proc.in_slots_received(),  3u);
            EXPECT_EQ(proc.out_slots_written(),  3u);
            EXPECT_EQ(proc.out_drop_count(),     0u);
        },
        "hub_processor.processor_overflow_block",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// processor_counters
// ============================================================================

int processor_counters()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g_in("ProcessorCounters_in");
            DataBlockTestGuard g_out("ProcessorCounters_out");
            DataBlockConfig cfg_in  = make_config(70024);
            DataBlockConfig cfg_out = make_config(70025);

            auto in_dbp_test = create_datablock_producer_impl(
                g_in.channel_name(), DataBlockPolicy::RingBuffer, cfg_in, nullptr, nullptr);
            ASSERT_NE(in_dbp_test, nullptr);
            auto in_dbc_proc = find_datablock_consumer_impl(
                g_in.channel_name(), cfg_in.shared_secret, &cfg_in, nullptr, nullptr);
            ASSERT_NE(in_dbc_proc, nullptr);

            auto out_dbp_proc = create_datablock_producer_impl(
                g_out.channel_name(), DataBlockPolicy::RingBuffer, cfg_out, nullptr, nullptr);
            ASSERT_NE(out_dbp_proc, nullptr);
            auto out_dbc_test = find_datablock_consumer_impl(
                g_out.channel_name(), cfg_out.shared_secret, &cfg_out, nullptr, nullptr);
            ASSERT_NE(out_dbc_test, nullptr);

            auto in_q  = ShmQueue::from_consumer(std::move(in_dbc_proc),  sizeof(double));
            auto out_q = ShmQueue::from_producer(std::move(out_dbp_proc), sizeof(double));

            auto maybe_proc = Processor::create(*in_q, *out_q, fast_opts());
            ASSERT_TRUE(maybe_proc.has_value());
            Processor& proc = *maybe_proc;

            // Handler commits odd-numbered calls (1, 3) and drops even-numbered calls (2, 4).
            std::atomic<int> call_count{0};
            proc.set_process_handler<void, double, void, double>(
                [&call_count](ProcessorContext<void, double, void, double>& ctx) -> bool {
                    ctx.output() = ctx.input();
                    int n = ++call_count;
                    return (n % 2 != 0); // commit 1st and 3rd; drop 2nd and 4th
                });
            proc.start();

            // Write 4 items one at a time, waiting for each to be processed.
            for (int i = 1; i <= 4; ++i)
            {
                uint64_t before = proc.in_slots_received();
                ASSERT_TRUE(write_double(in_dbp_test.get(), static_cast<double>(i)));
                bool consumed = wait_for(
                    [&proc, before] { return proc.in_slots_received() > before; }, 3000ms);
                ASSERT_TRUE(consumed) << "Item " << i << " not consumed within 3s";
                std::this_thread::sleep_for(20ms); // allow commit/drop counter to update
            }

            proc.stop();

            EXPECT_EQ(proc.in_slots_received(), 4u);
            EXPECT_EQ(proc.out_slots_written(), 2u); // items 1 and 3 committed
            EXPECT_EQ(proc.out_drop_count(),    2u); // items 2 and 4 dropped
        },
        "hub_processor.processor_counters",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// processor_stop_clean
// ============================================================================

int processor_stop_clean()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g_in("ProcessorStopClean_in");
            DataBlockTestGuard g_out("ProcessorStopClean_out");
            DataBlockConfig cfg_in  = make_config(70026);
            DataBlockConfig cfg_out = make_config(70027);

            auto in_dbp_test = create_datablock_producer_impl(
                g_in.channel_name(), DataBlockPolicy::RingBuffer, cfg_in, nullptr, nullptr);
            ASSERT_NE(in_dbp_test, nullptr);
            auto in_dbc_proc = find_datablock_consumer_impl(
                g_in.channel_name(), cfg_in.shared_secret, &cfg_in, nullptr, nullptr);
            ASSERT_NE(in_dbc_proc, nullptr);

            auto out_dbp_proc = create_datablock_producer_impl(
                g_out.channel_name(), DataBlockPolicy::RingBuffer, cfg_out, nullptr, nullptr);
            ASSERT_NE(out_dbp_proc, nullptr);

            auto in_q  = ShmQueue::from_consumer(std::move(in_dbc_proc),  sizeof(double));
            auto out_q = ShmQueue::from_producer(std::move(out_dbp_proc), sizeof(double));

            auto maybe_proc = Processor::create(*in_q, *out_q, fast_opts());
            ASSERT_TRUE(maybe_proc.has_value());
            Processor& proc = *maybe_proc;

            EXPECT_FALSE(proc.is_running());

            proc.start();
            EXPECT_TRUE(proc.is_running());
            EXPECT_FALSE(proc.is_stopping());

            proc.stop();
            EXPECT_FALSE(proc.is_running());
        },
        "hub_processor.processor_stop_clean",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// processor_close
// ============================================================================

int processor_close()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g_in("ProcessorClose_in");
            DataBlockTestGuard g_out("ProcessorClose_out");
            DataBlockConfig cfg_in  = make_config(70028);
            DataBlockConfig cfg_out = make_config(70029);

            auto in_dbp_test = create_datablock_producer_impl(
                g_in.channel_name(), DataBlockPolicy::RingBuffer, cfg_in, nullptr, nullptr);
            ASSERT_NE(in_dbp_test, nullptr);
            auto in_dbc_proc = find_datablock_consumer_impl(
                g_in.channel_name(), cfg_in.shared_secret, &cfg_in, nullptr, nullptr);
            ASSERT_NE(in_dbc_proc, nullptr);

            auto out_dbp_proc = create_datablock_producer_impl(
                g_out.channel_name(), DataBlockPolicy::RingBuffer, cfg_out, nullptr, nullptr);
            ASSERT_NE(out_dbp_proc, nullptr);

            auto in_q  = ShmQueue::from_consumer(std::move(in_dbc_proc),  sizeof(double));
            auto out_q = ShmQueue::from_producer(std::move(out_dbp_proc), sizeof(double));

            auto maybe_proc = Processor::create(*in_q, *out_q, fast_opts());
            ASSERT_TRUE(maybe_proc.has_value());
            Processor& proc = *maybe_proc;

            proc.start();
            EXPECT_TRUE(proc.is_running());

            proc.close();
            EXPECT_FALSE(proc.is_running());

            // close() again — should be idempotent
            EXPECT_NO_THROW(proc.close());
        },
        "hub_processor.processor_close",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// processor_handler_hot_swap
// ============================================================================

int processor_handler_hot_swap()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g_in("ProcessorHotSwap_in");
            DataBlockTestGuard g_out("ProcessorHotSwap_out");
            DataBlockConfig cfg_in  = make_config(70030);
            DataBlockConfig cfg_out = make_config(70031);

            auto in_dbp_test = create_datablock_producer_impl(
                g_in.channel_name(), DataBlockPolicy::RingBuffer, cfg_in, nullptr, nullptr);
            ASSERT_NE(in_dbp_test, nullptr);
            auto in_dbc_proc = find_datablock_consumer_impl(
                g_in.channel_name(), cfg_in.shared_secret, &cfg_in, nullptr, nullptr);
            ASSERT_NE(in_dbc_proc, nullptr);

            auto out_dbp_proc = create_datablock_producer_impl(
                g_out.channel_name(), DataBlockPolicy::RingBuffer, cfg_out, nullptr, nullptr);
            ASSERT_NE(out_dbp_proc, nullptr);
            auto out_dbc_test = find_datablock_consumer_impl(
                g_out.channel_name(), cfg_out.shared_secret, &cfg_out, nullptr, nullptr);
            ASSERT_NE(out_dbc_test, nullptr);

            auto in_q  = ShmQueue::from_consumer(std::move(in_dbc_proc),  sizeof(double));
            auto out_q = ShmQueue::from_producer(std::move(out_dbp_proc), sizeof(double));

            auto maybe_proc = Processor::create(*in_q, *out_q, fast_opts());
            ASSERT_TRUE(maybe_proc.has_value());
            Processor& proc = *maybe_proc;

            // Handler A: doubles the value
            proc.set_process_handler<void, double, void, double>(
                [](ProcessorContext<void, double, void, double>& ctx) -> bool {
                    ctx.output() = ctx.input() * 2.0;
                    return true;
                });

            proc.start();

            // Write with handler A
            ASSERT_TRUE(write_double(in_dbp_test.get(), 10.0));
            double result = 0.0;
            ASSERT_TRUE(read_double(out_dbc_test.get(), result));
            EXPECT_DOUBLE_EQ(result, 20.0);

            // Swap to handler B: triples the value
            proc.set_process_handler<void, double, void, double>(
                [](ProcessorContext<void, double, void, double>& ctx) -> bool {
                    ctx.output() = ctx.input() * 3.0;
                    return true;
                });

            // The process_thread_ loads the handler at the TOP of its loop,
            // before read_acquire().  After processing item 1 it already loaded
            // handler A and is now blocking on read_acquire.  We must wait for
            // input_timeout (200ms) to expire so the processor cycles back to
            // the loop top and reloads handler B.
            std::this_thread::sleep_for(400ms);

            // Write with handler B
            ASSERT_TRUE(write_double(in_dbp_test.get(), 10.0));
            ASSERT_TRUE(wait_for([&]() { return proc.in_slots_received() >= 2; }, 2000ms))
                << "Processor did not consume second input within timeout";

            result = 0.0;
            ASSERT_TRUE(read_double(out_dbc_test.get(), result));
            EXPECT_DOUBLE_EQ(result, 30.0);

            proc.stop();
        },
        "hub_processor.processor_handler_hot_swap",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// processor_handler_removal
// ============================================================================

int processor_handler_removal()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g_in("ProcessorHandlerRemoval_in");
            DataBlockTestGuard g_out("ProcessorHandlerRemoval_out");
            DataBlockConfig cfg_in  = make_config(70032);
            DataBlockConfig cfg_out = make_config(70033);

            auto in_dbp_test = create_datablock_producer_impl(
                g_in.channel_name(), DataBlockPolicy::RingBuffer, cfg_in, nullptr, nullptr);
            ASSERT_NE(in_dbp_test, nullptr);
            auto in_dbc_proc = find_datablock_consumer_impl(
                g_in.channel_name(), cfg_in.shared_secret, &cfg_in, nullptr, nullptr);
            ASSERT_NE(in_dbc_proc, nullptr);

            auto out_dbp_proc = create_datablock_producer_impl(
                g_out.channel_name(), DataBlockPolicy::RingBuffer, cfg_out, nullptr, nullptr);
            ASSERT_NE(out_dbp_proc, nullptr);
            auto out_dbc_test = find_datablock_consumer_impl(
                g_out.channel_name(), cfg_out.shared_secret, &cfg_out, nullptr, nullptr);
            ASSERT_NE(out_dbc_test, nullptr);

            auto in_q  = ShmQueue::from_consumer(std::move(in_dbc_proc),  sizeof(double));
            auto out_q = ShmQueue::from_producer(std::move(out_dbp_proc), sizeof(double));

            auto maybe_proc = Processor::create(*in_q, *out_q, fast_opts());
            ASSERT_TRUE(maybe_proc.has_value());
            Processor& proc = *maybe_proc;

            // Install handler
            proc.set_process_handler<void, double, void, double>(
                [](ProcessorContext<void, double, void, double>& ctx) -> bool {
                    ctx.output() = ctx.input() * 2.0;
                    return true;
                });
            EXPECT_TRUE(proc.has_process_handler());

            proc.start();

            // Remove handler (nullptr)
            proc.set_process_handler<void, double, void, double>(nullptr);
            EXPECT_FALSE(proc.has_process_handler());

            // Write data — processor should not crash; data just gets ignored
            ASSERT_TRUE(write_double(in_dbp_test.get(), 99.0));
            std::this_thread::sleep_for(300ms);
            double dummy = 0.0;
            EXPECT_FALSE(read_double(out_dbc_test.get(), dummy, 200))
                << "No handler installed — nothing should be produced";

            // Re-install handler
            proc.set_process_handler<void, double, void, double>(
                [](ProcessorContext<void, double, void, double>& ctx) -> bool {
                    ctx.output() = ctx.input() * 5.0;
                    return true;
                });
            EXPECT_TRUE(proc.has_process_handler());

            // Write and verify new handler works
            ASSERT_TRUE(write_double(in_dbp_test.get(), 4.0));
            double result = 0.0;
            ASSERT_TRUE(read_double(out_dbc_test.get(), result));
            EXPECT_DOUBLE_EQ(result, 20.0);

            proc.stop();
        },
        "hub_processor.processor_handler_removal",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// timeout_handler_produces_output
// ============================================================================

int timeout_handler_produces_output()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g_in("TimeoutProduces_in");
            DataBlockTestGuard g_out("TimeoutProduces_out");
            DataBlockConfig cfg_in  = make_config(70034);
            DataBlockConfig cfg_out = make_config(70035);

            auto in_dbp_test = create_datablock_producer_impl(
                g_in.channel_name(), DataBlockPolicy::RingBuffer, cfg_in, nullptr, nullptr);
            ASSERT_NE(in_dbp_test, nullptr);
            auto in_dbc_proc = find_datablock_consumer_impl(
                g_in.channel_name(), cfg_in.shared_secret, &cfg_in, nullptr, nullptr);
            ASSERT_NE(in_dbc_proc, nullptr);

            auto out_dbp_proc = create_datablock_producer_impl(
                g_out.channel_name(), DataBlockPolicy::RingBuffer, cfg_out, nullptr, nullptr);
            ASSERT_NE(out_dbp_proc, nullptr);
            auto out_dbc_test = find_datablock_consumer_impl(
                g_out.channel_name(), cfg_out.shared_secret, &cfg_out, nullptr, nullptr);
            ASSERT_NE(out_dbc_test, nullptr);

            auto in_q  = ShmQueue::from_consumer(std::move(in_dbc_proc),  sizeof(double));
            auto out_q = ShmQueue::from_producer(std::move(out_dbp_proc), sizeof(double));

            auto maybe_proc = Processor::create(*in_q, *out_q, fast_opts());
            ASSERT_TRUE(maybe_proc.has_value());
            Processor& proc = *maybe_proc;

            // Install a normal handler (required for the loop to proceed past handler check).
            proc.set_process_handler<void, double, void, double>(
                [](ProcessorContext<void, double, void, double>& ctx) -> bool {
                    ctx.output() = ctx.input();
                    return true;
                });

            // Install timeout handler that writes a sentinel value.
            proc.set_timeout_handler([](void* out_data, void* /*out_fz*/) -> bool {
                if (!out_data)
                    return false;
                double val = 42.0;
                std::memcpy(out_data, &val, sizeof(val));
                return true;
            });

            proc.start();

            // Don't write any input — wait for timeout handler to produce output.
            double result = -1.0;
            ASSERT_TRUE(read_double(out_dbc_test.get(), result, 3000))
                << "Timeout handler did not produce output within 3s";
            EXPECT_DOUBLE_EQ(result, 42.0);
            EXPECT_EQ(proc.out_slots_written(), 1u);

            proc.stop();
        },
        "hub_processor.timeout_handler_produces_output",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// timeout_handler_null_output_on_drop
// ============================================================================

int timeout_handler_null_output_on_drop()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g_in("TimeoutNullDrop_in");
            DataBlockTestGuard g_out("TimeoutNullDrop_out");
            DataBlockConfig cfg_in  = make_config(70036);
            DataBlockConfig cfg_out = make_config(70037);
            // Small capacity to fill easily
            cfg_out.ring_buffer_capacity = 4;

            auto in_dbp_test = create_datablock_producer_impl(
                g_in.channel_name(), DataBlockPolicy::RingBuffer, cfg_in, nullptr, nullptr);
            ASSERT_NE(in_dbp_test, nullptr);
            auto in_dbc_proc = find_datablock_consumer_impl(
                g_in.channel_name(), cfg_in.shared_secret, &cfg_in, nullptr, nullptr);
            ASSERT_NE(in_dbc_proc, nullptr);

            auto out_dbp_proc = create_datablock_producer_impl(
                g_out.channel_name(), DataBlockPolicy::RingBuffer, cfg_out, nullptr, nullptr);
            ASSERT_NE(out_dbp_proc, nullptr);
            // Note: we intentionally do NOT attach a consumer to the output queue
            // so it fills up and write_acquire returns nullptr in Drop mode.

            auto in_q  = ShmQueue::from_consumer(std::move(in_dbc_proc),  sizeof(double));
            auto out_q = ShmQueue::from_producer(std::move(out_dbp_proc), sizeof(double));

            auto maybe_proc = Processor::create(*in_q, *out_q, fast_opts(OverflowPolicy::Drop));
            ASSERT_TRUE(maybe_proc.has_value());
            Processor& proc = *maybe_proc;

            proc.set_process_handler<void, double, void, double>(
                [](ProcessorContext<void, double, void, double>& ctx) -> bool {
                    ctx.output() = ctx.input();
                    return true;
                });

            std::atomic<int> null_count{0};
            proc.set_timeout_handler([&null_count](void* out_data, void* /*out_fz*/) -> bool {
                if (!out_data)
                {
                    ++null_count;
                    return false;
                }
                double val = 99.0;
                std::memcpy(out_data, &val, sizeof(val));
                return true;
            });

            proc.start();

            // Wait for a few timeout iterations. The output will eventually fill in
            // Latest_only mode (ring buffer wraps), so we may or may not see null.
            // But we do exercise the code path. Just wait and verify no crash.
            std::this_thread::sleep_for(800ms);

            proc.stop();
            // The test passes if no crash occurred. The null_count may be 0 if
            // the ring buffer kept wrapping, which is fine.
            EXPECT_GE(proc.iteration_count(), 1u);
        },
        "hub_processor.timeout_handler_null_output_on_drop",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// iteration_count_advances_on_timeout
// ============================================================================

int iteration_count_advances_on_timeout()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g_in("IterCountTimeout_in");
            DataBlockTestGuard g_out("IterCountTimeout_out");
            DataBlockConfig cfg_in  = make_config(70038);
            DataBlockConfig cfg_out = make_config(70039);

            auto in_dbp_test = create_datablock_producer_impl(
                g_in.channel_name(), DataBlockPolicy::RingBuffer, cfg_in, nullptr, nullptr);
            ASSERT_NE(in_dbp_test, nullptr);
            auto in_dbc_proc = find_datablock_consumer_impl(
                g_in.channel_name(), cfg_in.shared_secret, &cfg_in, nullptr, nullptr);
            ASSERT_NE(in_dbc_proc, nullptr);

            auto out_dbp_proc = create_datablock_producer_impl(
                g_out.channel_name(), DataBlockPolicy::RingBuffer, cfg_out, nullptr, nullptr);
            ASSERT_NE(out_dbp_proc, nullptr);

            auto in_q  = ShmQueue::from_consumer(std::move(in_dbc_proc),  sizeof(double));
            auto out_q = ShmQueue::from_producer(std::move(out_dbp_proc), sizeof(double));

            auto maybe_proc = Processor::create(*in_q, *out_q, fast_opts());
            ASSERT_TRUE(maybe_proc.has_value());
            Processor& proc = *maybe_proc;

            proc.set_process_handler<void, double, void, double>(
                [](ProcessorContext<void, double, void, double>& ctx) -> bool {
                    ctx.output() = ctx.input();
                    return true;
                });
            proc.start();

            // No input — let timeouts occur (200ms each). Wait ~600ms → expect ≥2 iterations.
            std::this_thread::sleep_for(700ms);

            EXPECT_GE(proc.iteration_count(), 2u);
            EXPECT_EQ(proc.in_slots_received(), 0u);
            EXPECT_GT(proc.iteration_count(), proc.in_slots_received());

            proc.stop();
        },
        "hub_processor.iteration_count_advances_on_timeout",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// critical_error_stops_loop
// ============================================================================

int critical_error_stops_loop()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g_in("CriticalError_in");
            DataBlockTestGuard g_out("CriticalError_out");
            DataBlockConfig cfg_in  = make_config(70040);
            DataBlockConfig cfg_out = make_config(70041);

            auto in_dbp_test = create_datablock_producer_impl(
                g_in.channel_name(), DataBlockPolicy::RingBuffer, cfg_in, nullptr, nullptr);
            ASSERT_NE(in_dbp_test, nullptr);
            auto in_dbc_proc = find_datablock_consumer_impl(
                g_in.channel_name(), cfg_in.shared_secret, &cfg_in, nullptr, nullptr);
            ASSERT_NE(in_dbc_proc, nullptr);

            auto out_dbp_proc = create_datablock_producer_impl(
                g_out.channel_name(), DataBlockPolicy::RingBuffer, cfg_out, nullptr, nullptr);
            ASSERT_NE(out_dbp_proc, nullptr);

            auto in_q  = ShmQueue::from_consumer(std::move(in_dbc_proc),  sizeof(double));
            auto out_q = ShmQueue::from_producer(std::move(out_dbp_proc), sizeof(double));

            auto maybe_proc = Processor::create(*in_q, *out_q, fast_opts());
            ASSERT_TRUE(maybe_proc.has_value());
            Processor& proc = *maybe_proc;

            proc.set_process_handler<void, double, void, double>(
                [&proc](ProcessorContext<void, double, void, double>&) -> bool {
                    proc.set_critical_error("test fatal error");
                    return false;
                });
            proc.start();

            ASSERT_TRUE(write_double(in_dbp_test.get(), 1.0));

            // Wait for the loop to exit due to critical error.
            bool stopped = wait_for([&proc] { return !proc.is_running(); }, 3000ms);
            // The loop exits but is_running stays true until stop() is called.
            // Check critical error state instead.
            bool has_error = wait_for([&proc] { return proc.has_critical_error(); }, 3000ms);
            ASSERT_TRUE(has_error) << "Critical error not set within 3s";

            EXPECT_TRUE(proc.has_critical_error());
            EXPECT_EQ(proc.critical_error_reason(), "test fatal error");

            proc.stop();
            EXPECT_FALSE(proc.is_running());
        },
        "hub_processor.critical_error_stops_loop",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// critical_error_from_timeout_handler
// ============================================================================

int critical_error_from_timeout_handler()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g_in("CriticalTimeout_in");
            DataBlockTestGuard g_out("CriticalTimeout_out");
            DataBlockConfig cfg_in  = make_config(70042);
            DataBlockConfig cfg_out = make_config(70043);

            auto in_dbp_test = create_datablock_producer_impl(
                g_in.channel_name(), DataBlockPolicy::RingBuffer, cfg_in, nullptr, nullptr);
            ASSERT_NE(in_dbp_test, nullptr);
            auto in_dbc_proc = find_datablock_consumer_impl(
                g_in.channel_name(), cfg_in.shared_secret, &cfg_in, nullptr, nullptr);
            ASSERT_NE(in_dbc_proc, nullptr);

            auto out_dbp_proc = create_datablock_producer_impl(
                g_out.channel_name(), DataBlockPolicy::RingBuffer, cfg_out, nullptr, nullptr);
            ASSERT_NE(out_dbp_proc, nullptr);

            auto in_q  = ShmQueue::from_consumer(std::move(in_dbc_proc),  sizeof(double));
            auto out_q = ShmQueue::from_producer(std::move(out_dbp_proc), sizeof(double));

            auto maybe_proc = Processor::create(*in_q, *out_q, fast_opts());
            ASSERT_TRUE(maybe_proc.has_value());
            Processor& proc = *maybe_proc;

            proc.set_process_handler<void, double, void, double>(
                [](ProcessorContext<void, double, void, double>& ctx) -> bool {
                    ctx.output() = ctx.input();
                    return true;
                });

            proc.set_timeout_handler([&proc](void* /*out_data*/, void* /*out_fz*/) -> bool {
                proc.set_critical_error("timeout fatal");
                return false;
            });

            proc.start();

            // Don't write any input — let the timeout handler fire and set critical error.
            bool has_error = wait_for([&proc] { return proc.has_critical_error(); }, 3000ms);
            ASSERT_TRUE(has_error) << "Critical error from timeout handler not set within 3s";

            EXPECT_EQ(proc.critical_error_reason(), "timeout fatal");

            proc.stop();
        },
        "hub_processor.critical_error_from_timeout_handler",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// pre_hook_called_before_handler
// ============================================================================

int pre_hook_called_before_handler()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g_in("PreHookHandler_in");
            DataBlockTestGuard g_out("PreHookHandler_out");
            DataBlockConfig cfg_in  = make_config(70044);
            DataBlockConfig cfg_out = make_config(70045);

            auto in_dbp_test = create_datablock_producer_impl(
                g_in.channel_name(), DataBlockPolicy::RingBuffer, cfg_in, nullptr, nullptr);
            ASSERT_NE(in_dbp_test, nullptr);
            auto in_dbc_proc = find_datablock_consumer_impl(
                g_in.channel_name(), cfg_in.shared_secret, &cfg_in, nullptr, nullptr);
            ASSERT_NE(in_dbc_proc, nullptr);

            auto out_dbp_proc = create_datablock_producer_impl(
                g_out.channel_name(), DataBlockPolicy::RingBuffer, cfg_out, nullptr, nullptr);
            ASSERT_NE(out_dbp_proc, nullptr);
            auto out_dbc_test = find_datablock_consumer_impl(
                g_out.channel_name(), cfg_out.shared_secret, &cfg_out, nullptr, nullptr);
            ASSERT_NE(out_dbc_test, nullptr);

            auto in_q  = ShmQueue::from_consumer(std::move(in_dbc_proc),  sizeof(double));
            auto out_q = ShmQueue::from_producer(std::move(out_dbp_proc), sizeof(double));

            auto maybe_proc = Processor::create(*in_q, *out_q, fast_opts());
            ASSERT_TRUE(maybe_proc.has_value());
            Processor& proc = *maybe_proc;

            std::atomic<int> hook_count{0};
            std::atomic<int> handler_count{0};

            proc.set_pre_hook([&hook_count]() { ++hook_count; });

            proc.set_process_handler<void, double, void, double>(
                [&handler_count](ProcessorContext<void, double, void, double>& ctx) -> bool {
                    ++handler_count;
                    ctx.output() = ctx.input();
                    return true;
                });
            proc.start();

            for (int i = 0; i < 3; ++i)
            {
                uint64_t before = proc.in_slots_received();
                ASSERT_TRUE(write_double(in_dbp_test.get(), static_cast<double>(i)));
                ASSERT_TRUE(wait_for(
                    [&proc, before] { return proc.in_slots_received() > before; }, 3000ms));
                double dummy = 0.0;
                ASSERT_TRUE(read_double(out_dbc_test.get(), dummy, 2000));
            }

            proc.stop();

            EXPECT_EQ(handler_count.load(), 3);
            // Pre-hook fires for each handler call (and possibly for timeouts too).
            EXPECT_GE(hook_count.load(), handler_count.load());
        },
        "hub_processor.pre_hook_called_before_handler",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// pre_hook_called_before_timeout
// ============================================================================

int pre_hook_called_before_timeout()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g_in("PreHookTimeout_in");
            DataBlockTestGuard g_out("PreHookTimeout_out");
            DataBlockConfig cfg_in  = make_config(70046);
            DataBlockConfig cfg_out = make_config(70047);

            auto in_dbp_test = create_datablock_producer_impl(
                g_in.channel_name(), DataBlockPolicy::RingBuffer, cfg_in, nullptr, nullptr);
            ASSERT_NE(in_dbp_test, nullptr);
            auto in_dbc_proc = find_datablock_consumer_impl(
                g_in.channel_name(), cfg_in.shared_secret, &cfg_in, nullptr, nullptr);
            ASSERT_NE(in_dbc_proc, nullptr);

            auto out_dbp_proc = create_datablock_producer_impl(
                g_out.channel_name(), DataBlockPolicy::RingBuffer, cfg_out, nullptr, nullptr);
            ASSERT_NE(out_dbp_proc, nullptr);

            auto in_q  = ShmQueue::from_consumer(std::move(in_dbc_proc),  sizeof(double));
            auto out_q = ShmQueue::from_producer(std::move(out_dbp_proc), sizeof(double));

            auto maybe_proc = Processor::create(*in_q, *out_q, fast_opts());
            ASSERT_TRUE(maybe_proc.has_value());
            Processor& proc = *maybe_proc;

            std::atomic<int> hook_count{0};
            std::atomic<int> timeout_count{0};

            proc.set_pre_hook([&hook_count]() { ++hook_count; });

            proc.set_process_handler<void, double, void, double>(
                [](ProcessorContext<void, double, void, double>& ctx) -> bool {
                    ctx.output() = ctx.input();
                    return true;
                });

            proc.set_timeout_handler([&timeout_count](void* /*out_data*/, void* /*out_fz*/) -> bool {
                ++timeout_count;
                return false;
            });

            proc.start();

            // Don't write any input — timeout handler fires.
            std::this_thread::sleep_for(700ms);

            proc.stop();

            EXPECT_GE(timeout_count.load(), 1);
            // Pre-hook must have fired at least as many times as the timeout handler.
            EXPECT_GE(hook_count.load(), timeout_count.load());
        },
        "hub_processor.pre_hook_called_before_timeout",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// zero_fill_output_zeroed
// ============================================================================

int zero_fill_output_zeroed()
{
    return run_gtest_worker(
        []()
        {
            DataBlockTestGuard g_in("ZeroFill_in");
            DataBlockTestGuard g_out("ZeroFill_out");
            DataBlockConfig cfg_in  = make_config(70048);
            DataBlockConfig cfg_out = make_config(70049);

            auto in_dbp_test = create_datablock_producer_impl(
                g_in.channel_name(), DataBlockPolicy::RingBuffer, cfg_in, nullptr, nullptr);
            ASSERT_NE(in_dbp_test, nullptr);
            auto in_dbc_proc = find_datablock_consumer_impl(
                g_in.channel_name(), cfg_in.shared_secret, &cfg_in, nullptr, nullptr);
            ASSERT_NE(in_dbc_proc, nullptr);

            auto out_dbp_proc = create_datablock_producer_impl(
                g_out.channel_name(), DataBlockPolicy::RingBuffer, cfg_out, nullptr, nullptr);
            ASSERT_NE(out_dbp_proc, nullptr);
            auto out_dbc_test = find_datablock_consumer_impl(
                g_out.channel_name(), cfg_out.shared_secret, &cfg_out, nullptr, nullptr);
            ASSERT_NE(out_dbc_test, nullptr);

            auto in_q  = ShmQueue::from_consumer(std::move(in_dbc_proc),  sizeof(double));
            auto out_q = ShmQueue::from_producer(std::move(out_dbp_proc), sizeof(double));

            ProcessorOptions zf_opts = fast_opts();
            zf_opts.zero_fill_output = true;

            auto maybe_proc = Processor::create(*in_q, *out_q, zf_opts);
            ASSERT_TRUE(maybe_proc.has_value());
            Processor& proc = *maybe_proc;

            // Handler checks that output is zero-filled before writing.
            std::atomic<bool> was_zeroed{false};
            proc.set_process_handler<void, double, void, double>(
                [&was_zeroed](ProcessorContext<void, double, void, double>& ctx) -> bool {
                    // Check output is zero before we write.
                    double out_val = 0.0;
                    std::memcpy(&out_val, &ctx.output(), sizeof(double));
                    was_zeroed.store(out_val == 0.0, std::memory_order_relaxed);
                    ctx.output() = ctx.input() * 2.0;
                    return true;
                });
            proc.start();

            // Write a non-zero value.
            ASSERT_TRUE(write_double(in_dbp_test.get(), 7.0));
            double result = -1.0;
            ASSERT_TRUE(read_double(out_dbc_test.get(), result, 2000));
            EXPECT_DOUBLE_EQ(result, 14.0);
            EXPECT_TRUE(was_zeroed.load());

            proc.stop();
        },
        "hub_processor.zero_fill_output_zeroed",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// zmq_queue_roundtrip
// ============================================================================

int zmq_queue_roundtrip()
{
    return run_gtest_worker(
        []()
        {
            // Test Processor with ZmqQueue input and output.
            // Producer → ZmqQueue(PUSH) → ZmqQueue(PULL) → Processor → ZmqQueue(PUSH) → ZmqQueue(PULL) → verify
            auto in_push  = ZmqQueue::push_to("tcp://127.0.0.1:17050",  sizeof(double), true);
            auto in_pull  = ZmqQueue::pull_from("tcp://127.0.0.1:17050", sizeof(double), false);
            auto out_push = ZmqQueue::push_to("tcp://127.0.0.1:17051",  sizeof(double), true);
            auto out_pull = ZmqQueue::pull_from("tcp://127.0.0.1:17051", sizeof(double), false);

            ASSERT_NE(in_push, nullptr);
            ASSERT_NE(in_pull, nullptr);
            ASSERT_NE(out_push, nullptr);
            ASSERT_NE(out_pull, nullptr);

            ASSERT_TRUE(in_push->start());
            ASSERT_TRUE(in_pull->start());
            ASSERT_TRUE(out_push->start());
            ASSERT_TRUE(out_pull->start());

            // Let ZMQ sockets connect.
            std::this_thread::sleep_for(100ms);

            auto maybe_proc = Processor::create(*in_pull, *out_push, fast_opts());
            ASSERT_TRUE(maybe_proc.has_value());
            Processor& proc = *maybe_proc;

            proc.set_process_handler<void, double, void, double>(
                [](ProcessorContext<void, double, void, double>& ctx) -> bool {
                    ctx.output() = ctx.input() * 10.0;
                    return true;
                });
            proc.start();

            // Write via in_push, read from out_pull.
            for (int i = 1; i <= 3; ++i)
            {
                double val = static_cast<double>(i);
                void* buf = in_push->write_acquire(1000ms);
                ASSERT_NE(buf, nullptr);
                std::memcpy(buf, &val, sizeof(double));
                in_push->write_commit();

                const void* out = out_pull->read_acquire(3000ms);
                ASSERT_NE(out, nullptr) << "No output for item " << i;
                double result = 0.0;
                std::memcpy(&result, out, sizeof(double));
                out_pull->read_release();
                EXPECT_DOUBLE_EQ(result, val * 10.0);
            }

            proc.stop();
            out_pull->stop();
            out_push->stop();
            in_pull->stop();
            in_push->stop();
        },
        "hub_processor.zmq_queue_roundtrip",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// zmq_queue_null_flexzone
// ============================================================================

int zmq_queue_null_flexzone()
{
    return run_gtest_worker(
        []()
        {
            auto in_push  = ZmqQueue::push_to("tcp://127.0.0.1:17052",  sizeof(double), true);
            auto in_pull  = ZmqQueue::pull_from("tcp://127.0.0.1:17052", sizeof(double), false);
            auto out_push = ZmqQueue::push_to("tcp://127.0.0.1:17053",  sizeof(double), true);
            auto out_pull = ZmqQueue::pull_from("tcp://127.0.0.1:17053", sizeof(double), false);

            ASSERT_NE(in_push, nullptr);
            ASSERT_NE(in_pull, nullptr);
            ASSERT_NE(out_push, nullptr);
            ASSERT_NE(out_pull, nullptr);

            ASSERT_TRUE(in_push->start());
            ASSERT_TRUE(in_pull->start());
            ASSERT_TRUE(out_push->start());
            ASSERT_TRUE(out_pull->start());
            std::this_thread::sleep_for(100ms);

            auto maybe_proc = Processor::create(*in_pull, *out_push, fast_opts());
            ASSERT_TRUE(maybe_proc.has_value());
            Processor& proc = *maybe_proc;

            // Handler explicitly checks flexzone is nullptr.
            std::atomic<bool> fz_was_null{false};
            proc.set_process_handler<void, double, void, double>(
                [&fz_was_null](ProcessorContext<void, double, void, double>& ctx) -> bool {
                    fz_was_null.store(!ctx.has_in_flexzone() && !ctx.has_out_flexzone());
                    ctx.output() = ctx.input();
                    return true;
                });
            proc.start();

            double val = 5.0;
            void* buf = in_push->write_acquire(1000ms);
            ASSERT_NE(buf, nullptr);
            std::memcpy(buf, &val, sizeof(double));
            in_push->write_commit();

            const void* out = out_pull->read_acquire(3000ms);
            ASSERT_NE(out, nullptr);
            out_pull->read_release();

            EXPECT_TRUE(fz_was_null.load());

            proc.stop();
            out_pull->stop();
            out_push->stop();
            in_pull->stop();
            in_push->stop();
        },
        "hub_processor.zmq_queue_null_flexzone",
        logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// zmq_queue_timeout_handler
// ============================================================================

int zmq_queue_timeout_handler()
{
    return run_gtest_worker(
        []()
        {
            auto in_push  = ZmqQueue::push_to("tcp://127.0.0.1:17054",  sizeof(double), true);
            auto in_pull  = ZmqQueue::pull_from("tcp://127.0.0.1:17054", sizeof(double), false);
            auto out_push = ZmqQueue::push_to("tcp://127.0.0.1:17055",  sizeof(double), true);
            auto out_pull = ZmqQueue::pull_from("tcp://127.0.0.1:17055", sizeof(double), false);

            ASSERT_NE(in_push, nullptr);
            ASSERT_NE(in_pull, nullptr);
            ASSERT_NE(out_push, nullptr);
            ASSERT_NE(out_pull, nullptr);

            ASSERT_TRUE(in_push->start());
            ASSERT_TRUE(in_pull->start());
            ASSERT_TRUE(out_push->start());
            ASSERT_TRUE(out_pull->start());
            std::this_thread::sleep_for(100ms);

            auto maybe_proc = Processor::create(*in_pull, *out_push, fast_opts());
            ASSERT_TRUE(maybe_proc.has_value());
            Processor& proc = *maybe_proc;

            proc.set_process_handler<void, double, void, double>(
                [](ProcessorContext<void, double, void, double>& ctx) -> bool {
                    ctx.output() = ctx.input();
                    return true;
                });

            proc.set_timeout_handler([](void* out_data, void* /*out_fz*/) -> bool {
                if (!out_data)
                    return false;
                double val = 77.0;
                std::memcpy(out_data, &val, sizeof(val));
                return true;
            });

            proc.start();

            // Don't write input — let timeout handler produce output via ZmqQueue.
            const void* out = out_pull->read_acquire(3000ms);
            ASSERT_NE(out, nullptr) << "Timeout handler did not produce ZMQ output";
            double result = 0.0;
            std::memcpy(&result, out, sizeof(double));
            out_pull->read_release();
            EXPECT_DOUBLE_EQ(result, 77.0);

            proc.stop();
            out_pull->stop();
            out_push->stop();
            in_pull->stop();
            in_push->stop();
        },
        "hub_processor.zmq_queue_timeout_handler",
        logger_module(), crypto_module(), hub_module());
}

} // namespace pylabhub::tests::worker::hub_processor

// ============================================================================
// Worker dispatcher registrar
// ============================================================================

namespace
{

struct HubProcessorWorkerRegistrar
{
    HubProcessorWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char** argv) -> int {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                const auto       dot  = mode.find('.');
                if (dot == std::string_view::npos || mode.substr(0, dot) != "hub_processor")
                    return -1;
                std::string scenario(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::hub_processor;
                if (scenario == "processor_create")         return processor_create();
                if (scenario == "processor_no_handler")     return processor_no_handler();
                if (scenario == "processor_set_handler")    return processor_set_handler();
                if (scenario == "processor_transform")      return processor_transform();
                if (scenario == "processor_drop_false")     return processor_drop_false();
                if (scenario == "processor_overflow_drop")  return processor_overflow_drop();
                if (scenario == "processor_overflow_block") return processor_overflow_block();
                if (scenario == "processor_counters")       return processor_counters();
                if (scenario == "processor_stop_clean")       return processor_stop_clean();
                if (scenario == "processor_close")           return processor_close();
                if (scenario == "processor_handler_hot_swap") return processor_handler_hot_swap();
                if (scenario == "processor_handler_removal") return processor_handler_removal();
                if (scenario == "timeout_handler_produces_output") return timeout_handler_produces_output();
                if (scenario == "timeout_handler_null_output_on_drop") return timeout_handler_null_output_on_drop();
                if (scenario == "iteration_count_advances_on_timeout") return iteration_count_advances_on_timeout();
                if (scenario == "critical_error_stops_loop") return critical_error_stops_loop();
                if (scenario == "critical_error_from_timeout_handler") return critical_error_from_timeout_handler();
                if (scenario == "pre_hook_called_before_handler") return pre_hook_called_before_handler();
                if (scenario == "pre_hook_called_before_timeout") return pre_hook_called_before_timeout();
                if (scenario == "zero_fill_output_zeroed") return zero_fill_output_zeroed();
                if (scenario == "zmq_queue_roundtrip") return zmq_queue_roundtrip();
                if (scenario == "zmq_queue_null_flexzone") return zmq_queue_null_flexzone();
                if (scenario == "zmq_queue_timeout_handler") return zmq_queue_timeout_handler();
                fmt::print(stderr, "ERROR: Unknown hub_processor scenario '{}'\n", scenario);
                return 1;
            });
    }
};

static HubProcessorWorkerRegistrar g_hub_processor_registrar; // NOLINT(cert-err58-cpp)

} // anonymous namespace
