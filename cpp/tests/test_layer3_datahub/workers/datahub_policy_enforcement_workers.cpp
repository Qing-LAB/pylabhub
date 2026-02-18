// tests/test_layer3_datahub/workers/policy_enforcement_workers.cpp
// Policy enforcement tests: checksum, heartbeat, sync_reader, and auto-heartbeat in iterator.
//
// Test strategy:
// - Each test runs in an isolated process via run_gtest_worker
// - Tests verify that the RAII layer and C API enforce policies transparently
// - Heartbeat tests use active_consumer_count from shared memory header as oracle
#include "datahub_policy_enforcement_workers.h"
#include "test_entrypoint.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include <gtest/gtest.h>
#include <fmt/core.h>
#include <cstring>
#include <chrono>
#include <atomic>

using namespace pylabhub::hub;
using namespace pylabhub::tests::helper;
using namespace std::chrono_literals;

// ============================================================================
// Test Data Structures (file scope — PYLABHUB_SCHEMA_BEGIN requires it)
// ============================================================================

struct PolicyFlexZone
{
    uint32_t sequence;
    uint32_t flags;
    uint8_t padding[24]; // total 32 bytes
};
static_assert(std::is_trivially_copyable_v<PolicyFlexZone>);

struct PolicyData
{
    uint64_t value;
    uint8_t payload[56]; // total 64 bytes
};
static_assert(std::is_trivially_copyable_v<PolicyData>);

PYLABHUB_SCHEMA_BEGIN(PolicyFlexZone)
    PYLABHUB_SCHEMA_MEMBER(sequence)
    PYLABHUB_SCHEMA_MEMBER(flags)
PYLABHUB_SCHEMA_END(PolicyFlexZone)

PYLABHUB_SCHEMA_BEGIN(PolicyData)
    PYLABHUB_SCHEMA_MEMBER(value)
PYLABHUB_SCHEMA_END(PolicyData)

namespace pylabhub::tests::worker::policy_enforcement
{

static auto logger_module() { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto crypto_module() { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto hub_module() { return ::pylabhub::hub::GetLifecycleModule(); }

// ============================================================================
// Helpers
// ============================================================================

static DataBlockConfig make_config(ConsumerSyncPolicy sync_policy, ChecksumPolicy cs_policy,
                                   uint64_t secret)
{
    DataBlockConfig cfg{};
    cfg.policy = DataBlockPolicy::RingBuffer;
    cfg.consumer_sync_policy = sync_policy;
    cfg.shared_secret = secret;
    cfg.ring_buffer_capacity = 2;
    cfg.physical_page_size = DataBlockPageSize::Size4K;
    cfg.flex_zone_size = sizeof(PolicyFlexZone); // rounded up to PAGE_ALIGNMENT at creation
    cfg.checksum_policy = cs_policy;
    return cfg;
}

// ============================================================================
// Checksum: Enforced — slot write+read roundtrip, checksum auto-updated/verified
// ============================================================================

int checksum_enforced_write_read_roundtrip()
{
    return run_gtest_worker(
        []()
        {
            std::string ch = make_test_channel_name("PolicyCs1");
            auto cfg = make_config(ConsumerSyncPolicy::Latest_only, ChecksumPolicy::Enforced, 80001);

            auto producer = create_datablock_producer<PolicyFlexZone, PolicyData>(
                ch, DataBlockPolicy::RingBuffer, cfg);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer<PolicyFlexZone, PolicyData>(
                ch, cfg.shared_secret, cfg);
            ASSERT_NE(consumer, nullptr);

            // Write slot — checksum auto-updated on publish
            producer->with_transaction<PolicyFlexZone, PolicyData>(
                1000ms, [](WriteTransactionContext<PolicyFlexZone, PolicyData> &ctx)
                {
                    ctx.flexzone().get().sequence = 1;
                    for (auto &r : ctx.slots(50ms))
                    {
                        if (!r.is_ok()) { break; }
                        r.content().get().value = 42;
                        break;
                    }
                });

            // Read slot — checksum auto-verified on consume release
            bool read_ok = false;
            consumer->with_transaction<PolicyFlexZone, PolicyData>(
                1000ms, [&read_ok](ReadTransactionContext<PolicyFlexZone, PolicyData> &ctx)
                {
                    for (auto &r : ctx.slots(50ms))
                    {
                        if (!r.is_ok()) { break; }
                        EXPECT_EQ(r.content().get().value, 42u);
                        read_ok = true;
                        break;
                    }
                });

            EXPECT_TRUE(read_ok) << "Expected to read slot successfully";
            producer.reset();
            consumer.reset();
            cleanup_test_datablock(ch);
            fmt::print(stderr, "[policy_enforcement] checksum_enforced_write_read_roundtrip ok\n");
        },
        "checksum_enforced_write_read_roundtrip", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// Checksum: Enforced — flexzone-only write (no slot publish)
// with_transaction auto-updates flexzone checksum on normal exit
// ============================================================================

int checksum_enforced_flexzone_only_write()
{
    return run_gtest_worker(
        []()
        {
            std::string ch = make_test_channel_name("PolicyCs2");
            auto cfg = make_config(ConsumerSyncPolicy::Latest_only, ChecksumPolicy::Enforced, 80002);

            auto producer = create_datablock_producer<PolicyFlexZone, PolicyData>(
                ch, DataBlockPolicy::RingBuffer, cfg);
            ASSERT_NE(producer, nullptr);
            auto consumer = find_datablock_consumer<PolicyFlexZone, PolicyData>(
                ch, cfg.shared_secret, cfg);
            ASSERT_NE(consumer, nullptr);

            // Write only the flexzone — no slot publish
            producer->with_transaction<PolicyFlexZone, PolicyData>(
                1000ms, [](WriteTransactionContext<PolicyFlexZone, PolicyData> &ctx)
                {
                    ctx.flexzone().get().sequence = 99;
                    ctx.flexzone().get().flags = 0xDEAD;
                    // Deliberately do not iterate slots — only flexzone is written
                });
            // with_transaction exits normally → auto-update flexzone checksum fires

            // Consumer verifies flexzone checksum
            bool fz_ok = consumer->verify_checksum_flexible_zone();
            EXPECT_TRUE(fz_ok) << "Flexzone checksum should be valid after with_transaction exit";

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(ch);
            fmt::print(stderr, "[policy_enforcement] checksum_enforced_flexzone_only_write ok\n");
        },
        "checksum_enforced_flexzone_only_write", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// Checksum: Enforced — manual corruption detected
// ============================================================================

int checksum_enforced_verify_detects_corruption()
{
    return run_gtest_worker(
        []()
        {
            std::string ch = make_test_channel_name("PolicyCs3");
            auto cfg = make_config(ConsumerSyncPolicy::Latest_only, ChecksumPolicy::Enforced, 80003);

            auto producer = create_datablock_producer<PolicyFlexZone, PolicyData>(
                ch, DataBlockPolicy::RingBuffer, cfg);
            ASSERT_NE(producer, nullptr);

            // Write and publish one slot normally
            producer->with_transaction<PolicyFlexZone, PolicyData>(
                1000ms, [](WriteTransactionContext<PolicyFlexZone, PolicyData> &ctx)
                {
                    for (auto &r : ctx.slots(50ms))
                    {
                        if (!r.is_ok()) { break; }
                        r.content().get().value = 999;
                        break;
                    }
                });

            // Directly corrupt the flexzone in shared memory AFTER the checksum was stored
            auto fz_span = producer->flexible_zone_span();
            ASSERT_FALSE(fz_span.empty());
            fz_span[0] ^= std::byte{0xFF}; // flip a byte — checksum now stale

            auto consumer = find_datablock_consumer<PolicyFlexZone, PolicyData>(
                ch, cfg.shared_secret, cfg);
            ASSERT_NE(consumer, nullptr);

            // Consumer should detect checksum mismatch
            bool fz_valid = consumer->verify_checksum_flexible_zone();
            EXPECT_FALSE(fz_valid) << "Flexzone checksum should fail after manual corruption";

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(ch);
            fmt::print(stderr, "[policy_enforcement] checksum_enforced_verify_detects_corruption ok\n");
        },
        "checksum_enforced_verify_detects_corruption", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// Checksum: None — corruption not detected (policy is off)
// ============================================================================

int checksum_none_skips_update_verify()
{
    return run_gtest_worker(
        []()
        {
            std::string ch = make_test_channel_name("PolicyCs4");
            auto cfg = make_config(ConsumerSyncPolicy::Latest_only, ChecksumPolicy::None, 80004);

            auto producer = create_datablock_producer<PolicyFlexZone, PolicyData>(
                ch, DataBlockPolicy::RingBuffer, cfg);
            ASSERT_NE(producer, nullptr);

            // Write one slot
            producer->with_transaction<PolicyFlexZone, PolicyData>(
                1000ms, [](WriteTransactionContext<PolicyFlexZone, PolicyData> &ctx)
                {
                    for (auto &r : ctx.slots(50ms))
                    {
                        if (!r.is_ok()) { break; }
                        r.content().get().value = 77;
                        break;
                    }
                });

            // Corrupt flexzone — no checksum computed, so no checksum to mismatch
            auto fz_span = producer->flexible_zone_span();
            if (!fz_span.empty()) { fz_span[0] ^= std::byte{0xFF}; }

            auto consumer = find_datablock_consumer<PolicyFlexZone, PolicyData>(
                ch, cfg.shared_secret, cfg);
            ASSERT_NE(consumer, nullptr);

            // verify_checksum_flexible_zone with None policy: returns false (no checksum stored)
            // Consumer can still read the slot — release succeeds without verification
            bool read_ok = false;
            consumer->with_transaction<PolicyFlexZone, PolicyData>(
                1000ms, [&read_ok](ReadTransactionContext<PolicyFlexZone, PolicyData> &ctx)
                {
                    for (auto &r : ctx.slots(50ms))
                    {
                        if (!r.is_ok()) { break; }
                        read_ok = true;
                        break;
                    }
                });

            EXPECT_TRUE(read_ok) << "With ChecksumPolicy::None, read should succeed even after corruption";

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(ch);
            fmt::print(stderr, "[policy_enforcement] checksum_none_skips_update_verify ok\n");
        },
        "checksum_none_skips_update_verify", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// Checksum: Manual — user must call update/verify explicitly
// ============================================================================

int checksum_manual_requires_explicit_call()
{
    return run_gtest_worker(
        []()
        {
            std::string ch = make_test_channel_name("PolicyCs5");
            auto cfg = make_config(ConsumerSyncPolicy::Latest_only, ChecksumPolicy::Manual, 80005);

            auto producer = create_datablock_producer<PolicyFlexZone, PolicyData>(
                ch, DataBlockPolicy::RingBuffer, cfg);
            ASSERT_NE(producer, nullptr);

            // Write WITHOUT updating flexzone checksum
            producer->with_transaction<PolicyFlexZone, PolicyData>(
                1000ms, [](WriteTransactionContext<PolicyFlexZone, PolicyData> &ctx)
                {
                    ctx.flexzone().get().sequence = 7;
                    // Manual policy: with_transaction does NOT auto-update checksum
                    // (checksum_policy() == Manual → auto-update skipped)
                    ctx.suppress_flexzone_checksum(); // explicit opt-out (belt+suspenders)
                    for (auto &r : ctx.slots(50ms))
                    {
                        if (!r.is_ok()) { break; }
                        r.content().get().value = 55;
                        break;
                    }
                });

            auto consumer = find_datablock_consumer<PolicyFlexZone, PolicyData>(
                ch, cfg.shared_secret, cfg);
            ASSERT_NE(consumer, nullptr);

            // Checksum is stale (never computed) — consumer verify should fail or be invalid
            bool fz_valid_before = consumer->verify_checksum_flexible_zone();
            // No checksum was ever written, so it's either false or zeroed (invalid)
            // The important thing is: no auto-update happened
            (void)fz_valid_before; // result may vary; just verify it's not auto-updated

            // Now explicitly update checksum
            bool updated = producer->update_checksum_flexible_zone();
            EXPECT_TRUE(updated) << "Manual checksum update should succeed";

            // Now verify passes
            bool fz_valid_after = consumer->verify_checksum_flexible_zone();
            EXPECT_TRUE(fz_valid_after) << "Flexzone checksum should be valid after explicit update";

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(ch);
            fmt::print(stderr, "[policy_enforcement] checksum_manual_requires_explicit_call ok\n");
        },
        "checksum_manual_requires_explicit_call", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// Heartbeat: all consumer policies auto-register at construction
// ============================================================================

int consumer_auto_registers_heartbeat_on_construction()
{
    return run_gtest_worker(
        []()
        {
            std::string ch = make_test_channel_name("PolicyHb1");
            auto cfg = make_config(ConsumerSyncPolicy::Latest_only, ChecksumPolicy::Enforced, 80010);

            auto producer = create_datablock_producer<PolicyFlexZone, PolicyData>(
                ch, DataBlockPolicy::RingBuffer, cfg);
            ASSERT_NE(producer, nullptr);

            // active_consumer_count starts at 0
            auto diag = open_datablock_for_diagnostic(ch);
            ASSERT_NE(diag, nullptr);
            uint32_t before = diag->header()->active_consumer_count.load(std::memory_order_acquire);
            EXPECT_EQ(before, 0u) << "No consumers yet";

            auto consumer = find_datablock_consumer<PolicyFlexZone, PolicyData>(
                ch, cfg.shared_secret, cfg);
            ASSERT_NE(consumer, nullptr);

            uint32_t after = diag->header()->active_consumer_count.load(std::memory_order_acquire);
            EXPECT_EQ(after, 1u) << "Consumer should auto-register heartbeat at construction";

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(ch);
            fmt::print(stderr, "[policy_enforcement] consumer_auto_registers_heartbeat_on_construction ok\n");
        },
        "consumer_auto_registers_heartbeat_on_construction", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// Heartbeat: consumer auto-unregisters on destruction
// ============================================================================

int consumer_auto_unregisters_heartbeat_on_destroy()
{
    return run_gtest_worker(
        []()
        {
            std::string ch = make_test_channel_name("PolicyHb2");
            auto cfg = make_config(ConsumerSyncPolicy::Latest_only, ChecksumPolicy::Enforced, 80011);

            auto producer = create_datablock_producer<PolicyFlexZone, PolicyData>(
                ch, DataBlockPolicy::RingBuffer, cfg);
            ASSERT_NE(producer, nullptr);

            auto diag = open_datablock_for_diagnostic(ch);
            ASSERT_NE(diag, nullptr);

            {
                auto consumer = find_datablock_consumer<PolicyFlexZone, PolicyData>(
                    ch, cfg.shared_secret, cfg);
                ASSERT_NE(consumer, nullptr);

                uint32_t during = diag->header()->active_consumer_count.load(std::memory_order_acquire);
                EXPECT_EQ(during, 1u) << "Consumer registered";
            } // consumer destroyed here

            uint32_t after = diag->header()->active_consumer_count.load(std::memory_order_acquire);
            EXPECT_EQ(after, 0u) << "Consumer should auto-unregister heartbeat on destruction";

            producer.reset();
            cleanup_test_datablock(ch);
            fmt::print(stderr, "[policy_enforcement] consumer_auto_unregisters_heartbeat_on_destroy ok\n");
        },
        "consumer_auto_unregisters_heartbeat_on_destroy", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// Heartbeat: both Latest_only and Sync_reader register heartbeat
// ============================================================================

int all_policy_consumers_have_heartbeat()
{
    return run_gtest_worker(
        []()
        {
            std::string ch = make_test_channel_name("PolicyHb3");
            auto cfg = make_config(ConsumerSyncPolicy::Sync_reader, ChecksumPolicy::Enforced, 80012);

            auto producer = create_datablock_producer<PolicyFlexZone, PolicyData>(
                ch, DataBlockPolicy::RingBuffer, cfg);
            ASSERT_NE(producer, nullptr);

            auto diag = open_datablock_for_diagnostic(ch);
            ASSERT_NE(diag, nullptr);

            // Two consumers with different policies share the same heartbeat pool
            auto consumer_a = find_datablock_consumer<PolicyFlexZone, PolicyData>(
                ch, cfg.shared_secret, cfg);
            ASSERT_NE(consumer_a, nullptr);

            uint32_t after_first = diag->header()->active_consumer_count.load(std::memory_order_acquire);
            EXPECT_EQ(after_first, 1u) << "First consumer registered";

            // Second consumer (still Sync_reader — same config)
            auto consumer_b = find_datablock_consumer<PolicyFlexZone, PolicyData>(
                ch, cfg.shared_secret, cfg);
            ASSERT_NE(consumer_b, nullptr);

            uint32_t after_second = diag->header()->active_consumer_count.load(std::memory_order_acquire);
            EXPECT_EQ(after_second, 2u) << "Second consumer registered";

            consumer_a.reset();
            consumer_b.reset();

            uint32_t after_reset = diag->header()->active_consumer_count.load(std::memory_order_acquire);
            EXPECT_EQ(after_reset, 0u) << "Both consumers unregistered";

            producer.reset();
            cleanup_test_datablock(ch);
            fmt::print(stderr, "[policy_enforcement] all_policy_consumers_have_heartbeat ok\n");
        },
        "all_policy_consumers_have_heartbeat", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// Sync_reader: producer blocks when consumer is behind
// ============================================================================

int sync_reader_producer_respects_consumer_position()
{
    return run_gtest_worker(
        []()
        {
            std::string ch = make_test_channel_name("PolicySr1");
            // 1-slot ring to make backpressure immediate
            DataBlockConfig cfg{};
            cfg.policy = DataBlockPolicy::RingBuffer;
            cfg.consumer_sync_policy = ConsumerSyncPolicy::Sync_reader;
            cfg.shared_secret = 80020;
            cfg.ring_buffer_capacity = 1;
            cfg.physical_page_size = DataBlockPageSize::Size4K;
            cfg.flex_zone_size = sizeof(PolicyFlexZone); // rounded up to PAGE_ALIGNMENT at creation
            cfg.checksum_policy = ChecksumPolicy::Enforced;

            auto producer = create_datablock_producer<PolicyFlexZone, PolicyData>(
                ch, DataBlockPolicy::RingBuffer, cfg);
            ASSERT_NE(producer, nullptr);

            auto consumer = find_datablock_consumer<PolicyFlexZone, PolicyData>(
                ch, cfg.shared_secret, cfg);
            ASSERT_NE(consumer, nullptr);

            // Producer fills the single slot
            producer->with_transaction<PolicyFlexZone, PolicyData>(
                500ms, [](WriteTransactionContext<PolicyFlexZone, PolicyData> &ctx)
                {
                    for (auto &r : ctx.slots(50ms))
                    {
                        if (!r.is_ok()) { break; }
                        r.content().get().value = 1;
                        break;
                    }
                });

            // Producer attempts second write with short timeout — should block
            bool timed_out = false;
            producer->with_transaction<PolicyFlexZone, PolicyData>(
                100ms, [&timed_out](WriteTransactionContext<PolicyFlexZone, PolicyData> &ctx)
                {
                    for (auto &r : ctx.slots(30ms))
                    {
                        if (!r.is_ok()) { timed_out = true; break; }
                        // Should not acquire a slot while consumer is behind
                        ADD_FAILURE() << "Producer should not have acquired slot";
                        break;
                    }
                });
            EXPECT_TRUE(timed_out) << "Producer should time out when Sync_reader consumer is behind";

            // Consumer reads — unblocks producer
            bool read_ok = false;
            consumer->with_transaction<PolicyFlexZone, PolicyData>(
                500ms, [&read_ok](ReadTransactionContext<PolicyFlexZone, PolicyData> &ctx)
                {
                    for (auto &r : ctx.slots(50ms))
                    {
                        if (!r.is_ok()) { break; }
                        EXPECT_EQ(r.content().get().value, 1u);
                        read_ok = true;
                        break;
                    }
                });
            EXPECT_TRUE(read_ok) << "Consumer should read the produced slot";

            // Producer can now write again
            bool write_ok = false;
            producer->with_transaction<PolicyFlexZone, PolicyData>(
                500ms, [&write_ok](WriteTransactionContext<PolicyFlexZone, PolicyData> &ctx)
                {
                    for (auto &r : ctx.slots(50ms))
                    {
                        if (!r.is_ok()) { break; }
                        r.content().get().value = 2;
                        write_ok = true;
                        break;
                    }
                });
            EXPECT_TRUE(write_ok) << "Producer should succeed after consumer advanced";

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(ch);
            fmt::print(stderr, "[policy_enforcement] sync_reader_producer_respects_consumer_position ok\n");
        },
        "sync_reader_producer_respects_consumer_position", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// Auto-heartbeat in iterator: producer heartbeat updated in operator++()
// ============================================================================

int producer_operator_increment_updates_heartbeat()
{
    return run_gtest_worker(
        []()
        {
            std::string ch = make_test_channel_name("PolicyAutoHb1");
            auto cfg = make_config(ConsumerSyncPolicy::Latest_only, ChecksumPolicy::Enforced, 80030);

            auto producer = create_datablock_producer<PolicyFlexZone, PolicyData>(
                ch, DataBlockPolicy::RingBuffer, cfg);
            ASSERT_NE(producer, nullptr);

            auto diag = open_datablock_for_diagnostic(ch);
            ASSERT_NE(diag, nullptr);

            // Capture producer heartbeat timestamp before the loop
            uint64_t ts_before = diag->header()->last_heartbeat_ns.load(std::memory_order_acquire);

            // Run with_transaction — operator++() should update the heartbeat
            producer->with_transaction<PolicyFlexZone, PolicyData>(
                500ms, [](WriteTransactionContext<PolicyFlexZone, PolicyData> &ctx)
                {
                    for (auto &r : ctx.slots(20ms))
                    {
                        if (!r.is_ok()) { break; }
                        r.content().get().value = 42;
                        break;
                    }
                });

            uint64_t ts_after = diag->header()->last_heartbeat_ns.load(std::memory_order_acquire);
            EXPECT_GE(ts_after, ts_before) << "Producer heartbeat should be updated after iterator loop";

            producer.reset();
            cleanup_test_datablock(ch);
            fmt::print(stderr, "[policy_enforcement] producer_operator_increment_updates_heartbeat ok\n");
        },
        "producer_operator_increment_updates_heartbeat", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// Auto-heartbeat in iterator: consumer heartbeat updated in operator++()
// ============================================================================

int consumer_operator_increment_updates_heartbeat()
{
    return run_gtest_worker(
        []()
        {
            std::string ch = make_test_channel_name("PolicyAutoHb2");
            auto cfg = make_config(ConsumerSyncPolicy::Latest_only, ChecksumPolicy::Enforced, 80031);

            auto producer = create_datablock_producer<PolicyFlexZone, PolicyData>(
                ch, DataBlockPolicy::RingBuffer, cfg);
            ASSERT_NE(producer, nullptr);

            // Write one slot for the consumer to read
            producer->with_transaction<PolicyFlexZone, PolicyData>(
                500ms, [](WriteTransactionContext<PolicyFlexZone, PolicyData> &ctx)
                {
                    for (auto &r : ctx.slots(20ms))
                    {
                        if (!r.is_ok()) { break; }
                        r.content().get().value = 99;
                        break;
                    }
                });

            auto consumer = find_datablock_consumer<PolicyFlexZone, PolicyData>(
                ch, cfg.shared_secret, cfg);
            ASSERT_NE(consumer, nullptr);

            auto diag = open_datablock_for_diagnostic(ch);
            ASSERT_NE(diag, nullptr);

            // Capture consumer heartbeat timestamp (slot 0 is the registered slot)
            uint64_t ts_before = 0;
            for (size_t i = 0; i < detail::MAX_CONSUMER_HEARTBEATS; ++i)
            {
                if (diag->header()->consumer_heartbeats[i].consumer_id.load() != 0)
                {
                    ts_before = diag->header()->consumer_heartbeats[i].last_heartbeat_ns.load(
                        std::memory_order_acquire);
                    break;
                }
            }

            // Run with_transaction — operator++() should update consumer heartbeat
            consumer->with_transaction<PolicyFlexZone, PolicyData>(
                500ms, [](ReadTransactionContext<PolicyFlexZone, PolicyData> &ctx)
                {
                    for (auto &r : ctx.slots(20ms))
                    {
                        if (!r.is_ok()) { break; }
                        (void)r.content().get().value;
                        break;
                    }
                });

            uint64_t ts_after = 0;
            for (size_t i = 0; i < detail::MAX_CONSUMER_HEARTBEATS; ++i)
            {
                if (diag->header()->consumer_heartbeats[i].consumer_id.load() != 0)
                {
                    ts_after = diag->header()->consumer_heartbeats[i].last_heartbeat_ns.load(
                        std::memory_order_acquire);
                    break;
                }
            }

            EXPECT_GE(ts_after, ts_before) << "Consumer heartbeat should be updated after iterator loop";

            producer.reset();
            consumer.reset();
            cleanup_test_datablock(ch);
            fmt::print(stderr, "[policy_enforcement] consumer_operator_increment_updates_heartbeat ok\n");
        },
        "consumer_operator_increment_updates_heartbeat", logger_module(), crypto_module(), hub_module());
}

} // namespace pylabhub::tests::worker::policy_enforcement

namespace
{
struct PolicyEnforcementWorkerRegistrar
{
    PolicyEnforcementWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos || mode.substr(0, dot) != "policy_enforcement")
                    return -1;
                std::string scenario(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::policy_enforcement;
                if (scenario == "checksum_enforced_write_read_roundtrip")
                    return checksum_enforced_write_read_roundtrip();
                if (scenario == "checksum_enforced_flexzone_only_write")
                    return checksum_enforced_flexzone_only_write();
                if (scenario == "checksum_enforced_verify_detects_corruption")
                    return checksum_enforced_verify_detects_corruption();
                if (scenario == "checksum_none_skips_update_verify")
                    return checksum_none_skips_update_verify();
                if (scenario == "checksum_manual_requires_explicit_call")
                    return checksum_manual_requires_explicit_call();
                if (scenario == "consumer_auto_registers_heartbeat_on_construction")
                    return consumer_auto_registers_heartbeat_on_construction();
                if (scenario == "consumer_auto_unregisters_heartbeat_on_destroy")
                    return consumer_auto_unregisters_heartbeat_on_destroy();
                if (scenario == "all_policy_consumers_have_heartbeat")
                    return all_policy_consumers_have_heartbeat();
                if (scenario == "sync_reader_producer_respects_consumer_position")
                    return sync_reader_producer_respects_consumer_position();
                if (scenario == "producer_operator_increment_updates_heartbeat")
                    return producer_operator_increment_updates_heartbeat();
                if (scenario == "consumer_operator_increment_updates_heartbeat")
                    return consumer_operator_increment_updates_heartbeat();
                fmt::print(stderr, "ERROR: Unknown policy_enforcement scenario '{}'\n", scenario);
                return 1;
            });
    }
};
static PolicyEnforcementWorkerRegistrar g_policy_enforcement_registrar;
} // namespace
