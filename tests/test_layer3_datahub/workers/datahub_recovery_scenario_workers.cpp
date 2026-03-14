// tests/test_layer3_datahub/workers/datahub_recovery_scenario_workers.cpp
//
// Recovery scenario tests: zombie detection, force-reset, dead consumer cleanup.
//
// Each test injects a "broken" state into shared memory via DiagnosticHandle
// (DiagnosticHandle maps R/W, so SlotRWState atomics can be written directly),
// then exercises the recovery API to verify it detects and repairs the state.
//
// Dead PID strategy:
//   kDeadPid = 2147483647 (INT32_MAX).  No Linux process has this PID
//   (kernel max is 4194304).  kill(INT32_MAX, 0) returns ESRCH → is_process_alive=false.
//
// Secrets start at 77001.
//
// Scenario list:
//   1. zombie_writer_detected_and_released  — dead PID in write_lock → release_zombie_writer → FREE
//   2. zombie_readers_force_cleared         — reader_count>0, no live write_lock → release → 0
//   3. force_reset_slot_on_dead_writer      — WRITING + dead write_lock → force_reset (no force flag)
//   4. dead_consumer_cleanup               — fake heartbeat with dead PID → cleanup removes it
//   5. is_process_alive_false_for_nonexistent — datablock_is_process_alive sentinel check
//   6. force_reset_unsafe_when_writer_alive — RECOVERY_UNSAFE when alive PID holds write_lock

#include "datahub_recovery_scenario_workers.h"
#include "test_entrypoint.h"
#include "shared_test_helpers.h"
#include "plh_datahub.hpp"
#include "utils/recovery_api.hpp"
#include <gtest/gtest.h>
#include <fmt/core.h>
#include <cstring>

using namespace pylabhub::hub;
using namespace pylabhub::tests::helper;

namespace pylabhub::tests::worker::recovery_scenarios
{

static auto logger_module() { return ::pylabhub::utils::Logger::GetLifecycleModule(); }
static auto crypto_module() { return ::pylabhub::crypto::GetLifecycleModule(); }
static auto hub_module() { return ::pylabhub::hub::GetLifecycleModule(); }

// INT32_MAX as a dead PID — guaranteed not to be a live process on any Linux system.
static constexpr uint64_t kDeadPid = 2147483647ULL;

static DataBlockConfig make_recovery_config(uint64_t secret)
{
    DataBlockConfig cfg{};
    cfg.policy = DataBlockPolicy::RingBuffer;
    cfg.consumer_sync_policy = ConsumerSyncPolicy::Latest_only;
    cfg.shared_secret = secret;
    cfg.ring_buffer_capacity = 2;
    cfg.physical_page_size = DataBlockPageSize::Size4K;
    cfg.checksum_policy = ChecksumPolicy::None;
    return cfg;
}

// ============================================================================
// 1. zombie_writer_detected_and_released
// Create DataBlock, acquire write slot (write_lock = my_pid, state = WRITING).
// Via DiagnosticHandle overwrite write_lock to kDeadPid (zombie writer simulation).
// Call release_zombie_writer → RECOVERY_SUCCESS; verify write_lock cleared.
// ============================================================================

int zombie_writer_detected_and_released()
{
    return run_gtest_worker(
        []()
        {
            // Pre-check: confirm kDeadPid is actually dead on this system.
            ASSERT_FALSE(datablock_is_process_alive(kDeadPid))
                << "Test invariant: kDeadPid (" << kDeadPid << ") must not be a live process";

            std::string channel = make_test_channel_name("ZombieWriter");
            DataBlockConfig cfg = make_recovery_config(77001);

            auto producer = create_datablock_producer_impl(channel,
                                                           DataBlockPolicy::RingBuffer,
                                                           cfg, nullptr, nullptr);
            ASSERT_NE(producer, nullptr);

            // Acquire a write slot — slot 0 enters WRITING with write_lock = my_pid
            auto wh = producer->acquire_write_slot(500);
            ASSERT_NE(wh, nullptr) << "Must acquire write slot for zombie simulation";

            // Inject zombie: overwrite write_lock to kDeadPid via DiagnosticHandle
            {
                auto diag = open_datablock_for_diagnostic(channel);
                ASSERT_NE(diag, nullptr);
                SlotRWState *rw = diag->slot_rw_state(0);
                ASSERT_NE(rw, nullptr);
                // Verify slot is in WRITING state before injection
                EXPECT_EQ(rw->slot_state.load(std::memory_order_acquire),
                          SlotRWState::SlotState::WRITING);
                // Overwrite write_lock to dead PID (simulating a zombie writer)
                rw->write_lock.store(kDeadPid, std::memory_order_release);
            }

            // Recovery: release_zombie_writer should detect dead PID → FREE
            RecoveryResult r = datablock_release_zombie_writer(channel.c_str(), 0);
            EXPECT_EQ(r, RECOVERY_SUCCESS) << "release_zombie_writer must succeed for dead write_lock";

            // Verify the slot is now FREE with write_lock cleared
            {
                auto diag = open_datablock_for_diagnostic(channel);
                ASSERT_NE(diag, nullptr);
                SlotRWState *rw = diag->slot_rw_state(0);
                ASSERT_NE(rw, nullptr);
                EXPECT_EQ(rw->write_lock.load(std::memory_order_acquire), 0u)
                    << "write_lock must be 0 after zombie writer released";
                EXPECT_EQ(rw->slot_state.load(std::memory_order_acquire),
                          SlotRWState::SlotState::FREE)
                    << "slot_state must be FREE after zombie writer released";

                // Verify recovery_actions_count was incremented
                SharedMemoryHeader *hdr = diag->header();
                ASSERT_NE(hdr, nullptr);
                EXPECT_GT(hdr->recovery_actions_count.load(std::memory_order_acquire), 0u)
                    << "recovery_actions_count must be > 0 after a recovery action";
            }

            // wh still holds a write handle whose write_lock was overwritten then cleared by
            // recovery. Restore the slot to a WRITING + our_pid state so release_write_slot
            // can abort cleanly (its abort path requires write_lock == current PID).  We
            // already verified the recovery result above; this is purely a cleanup step.
            {
                auto diag = open_datablock_for_diagnostic(channel);
                ASSERT_NE(diag, nullptr);
                SlotRWState *rw = diag->slot_rw_state(0);
                ASSERT_NE(rw, nullptr);
                rw->write_lock.store(pylabhub::platform::get_pid(), std::memory_order_release);
                rw->slot_state.store(SlotRWState::SlotState::WRITING, std::memory_order_release);
            }
            static_cast<void>(producer->release_write_slot(*wh)); // abort the write cleanly
            producer.reset();
            cleanup_test_datablock(channel);
        },
        "zombie_writer_detected_and_released", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// 2. zombie_readers_force_cleared
// Create DataBlock, commit slot 0 (write_lock released on commit).
// Inject reader_count = 3 via DiagnosticHandle (simulate 3 zombie readers).
// write_lock = 0 → producer_is_alive = false → release_zombie_readers should
// proceed with force=false and return RECOVERY_SUCCESS.
// ============================================================================

int zombie_readers_force_cleared()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("ZombieReaders");
            DataBlockConfig cfg = make_recovery_config(77002);

            auto producer = create_datablock_producer_impl(channel,
                                                           DataBlockPolicy::RingBuffer,
                                                           cfg, nullptr, nullptr);
            ASSERT_NE(producer, nullptr);

            // Write + commit → slot 0 is COMMITTED, write_lock = 0
            {
                auto wh = producer->acquire_write_slot(500);
                ASSERT_NE(wh, nullptr);
                uint64_t payload = 0xABCD1234ULL;
                std::memcpy(wh->buffer_span().data(), &payload, sizeof(payload));
                EXPECT_TRUE(wh->commit(sizeof(payload)));
                EXPECT_TRUE(producer->release_write_slot(*wh));
            }

            // Inject zombie readers: set reader_count = 3 via DiagnosticHandle
            {
                auto diag = open_datablock_for_diagnostic(channel);
                ASSERT_NE(diag, nullptr);
                SlotRWState *rw = diag->slot_rw_state(0);
                ASSERT_NE(rw, nullptr);
                EXPECT_EQ(rw->slot_state.load(std::memory_order_acquire),
                          SlotRWState::SlotState::COMMITTED);
                EXPECT_EQ(rw->write_lock.load(std::memory_order_acquire), 0u);
                // Inject zombie: 3 readers that never released
                rw->reader_count.store(3, std::memory_order_release);
            }

            // Recovery: write_lock=0 → producer_is_alive=false → can release without force
            RecoveryResult r = datablock_release_zombie_readers(channel.c_str(), 0, false);
            EXPECT_EQ(r, RECOVERY_SUCCESS) << "release_zombie_readers must succeed (no live write_lock)";

            // Verify reader_count cleared
            {
                auto diag = open_datablock_for_diagnostic(channel);
                ASSERT_NE(diag, nullptr);
                SlotRWState *rw = diag->slot_rw_state(0);
                ASSERT_NE(rw, nullptr);
                EXPECT_EQ(rw->reader_count.load(std::memory_order_acquire), 0u)
                    << "reader_count must be 0 after zombie readers released";
                // Slot remains COMMITTED (no state change when not DRAINING)
                EXPECT_EQ(rw->slot_state.load(std::memory_order_acquire),
                          SlotRWState::SlotState::COMMITTED)
                    << "slot_state must remain COMMITTED after reader cleanup";

                SharedMemoryHeader *hdr = diag->header();
                ASSERT_NE(hdr, nullptr);
                EXPECT_GT(hdr->recovery_actions_count.load(std::memory_order_acquire), 0u)
                    << "recovery_actions_count must be > 0 after recovery action";
            }

            producer.reset();
            cleanup_test_datablock(channel);
        },
        "zombie_readers_force_cleared", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// 3. force_reset_slot_on_dead_writer
// Create DataBlock, acquire write slot (WRITING, write_lock = my_pid).
// Inject dead PID into write_lock.
// force_reset_slot(slot, force=false) must succeed — dead write_lock is not "alive",
// so no safety guard fires even without force=true.
// ============================================================================

int force_reset_slot_on_dead_writer()
{
    return run_gtest_worker(
        []()
        {
            ASSERT_FALSE(datablock_is_process_alive(kDeadPid))
                << "Test invariant: kDeadPid must not be a live process";

            std::string channel = make_test_channel_name("ForceResetDeadWriter");
            DataBlockConfig cfg = make_recovery_config(77003);

            auto producer = create_datablock_producer_impl(channel,
                                                           DataBlockPolicy::RingBuffer,
                                                           cfg, nullptr, nullptr);
            ASSERT_NE(producer, nullptr);

            // Acquire write slot → slot 0 is WRITING with write_lock = my_pid
            auto wh = producer->acquire_write_slot(500);
            ASSERT_NE(wh, nullptr);

            // Inject dead PID
            {
                auto diag = open_datablock_for_diagnostic(channel);
                ASSERT_NE(diag, nullptr);
                SlotRWState *rw = diag->slot_rw_state(0);
                ASSERT_NE(rw, nullptr);
                EXPECT_EQ(rw->slot_state.load(std::memory_order_acquire),
                          SlotRWState::SlotState::WRITING);
                rw->write_lock.store(kDeadPid, std::memory_order_release);
            }

            // force_reset without force flag — dead write_lock means no safety block
            RecoveryResult r = datablock_force_reset_slot(channel.c_str(), 0, false);
            EXPECT_EQ(r, RECOVERY_SUCCESS)
                << "force_reset_slot must succeed for dead write_lock without force flag";

            {
                auto diag = open_datablock_for_diagnostic(channel);
                ASSERT_NE(diag, nullptr);
                SlotRWState *rw = diag->slot_rw_state(0);
                ASSERT_NE(rw, nullptr);
                EXPECT_EQ(rw->write_lock.load(std::memory_order_acquire), 0u)
                    << "write_lock must be 0 after force_reset";
                EXPECT_EQ(rw->slot_state.load(std::memory_order_acquire),
                          SlotRWState::SlotState::FREE)
                    << "slot_state must be FREE after force_reset";
                EXPECT_EQ(rw->reader_count.load(std::memory_order_acquire), 0u);

                SharedMemoryHeader *hdr = diag->header();
                ASSERT_NE(hdr, nullptr);
                EXPECT_GT(hdr->recovery_actions_count.load(std::memory_order_acquire), 0u);
            }

            // Restore slot state so wh can abort cleanly before producer is destroyed.
            {
                auto diag = open_datablock_for_diagnostic(channel);
                ASSERT_NE(diag, nullptr);
                SlotRWState *rw = diag->slot_rw_state(0);
                ASSERT_NE(rw, nullptr);
                rw->write_lock.store(pylabhub::platform::get_pid(), std::memory_order_release);
                rw->slot_state.store(SlotRWState::SlotState::WRITING, std::memory_order_release);
            }
            static_cast<void>(producer->release_write_slot(*wh));
            producer.reset();
            cleanup_test_datablock(channel);
        },
        "force_reset_slot_on_dead_writer", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// 4. dead_consumer_cleanup
// Inject a fake consumer heartbeat entry with kDeadPid into the header.
// datablock_cleanup_dead_consumers must remove it and decrement active_consumer_count.
// ============================================================================

int dead_consumer_cleanup()
{
    return run_gtest_worker(
        []()
        {
            ASSERT_FALSE(datablock_is_process_alive(kDeadPid))
                << "Test invariant: kDeadPid must not be a live process";

            std::string channel = make_test_channel_name("DeadConsumerCleanup");
            DataBlockConfig cfg = make_recovery_config(77004);

            auto producer = create_datablock_producer_impl(channel,
                                                           DataBlockPolicy::RingBuffer,
                                                           cfg, nullptr, nullptr);
            ASSERT_NE(producer, nullptr);

            // Inject fake dead consumer heartbeat into slot 0 of the heartbeat table
            {
                auto diag = open_datablock_for_diagnostic(channel);
                ASSERT_NE(diag, nullptr);
                SharedMemoryHeader *hdr = diag->header();
                ASSERT_NE(hdr, nullptr);

                // Slot 0 in the heartbeat pool: set consumer_pid to dead PID
                hdr->consumer_heartbeats[0].consumer_pid.store(kDeadPid,
                                                               std::memory_order_release);
                // Increment active_consumer_count to reflect this "registered" consumer
                hdr->active_consumer_count.fetch_add(1, std::memory_order_relaxed);
            }

            // Cleanup: should detect dead PID in heartbeat slot 0 and clear it
            RecoveryResult r = datablock_cleanup_dead_consumers(channel.c_str());
            EXPECT_EQ(r, RECOVERY_SUCCESS) << "cleanup_dead_consumers must return SUCCESS";

            // Verify the heartbeat entry was cleared
            {
                auto diag = open_datablock_for_diagnostic(channel);
                ASSERT_NE(diag, nullptr);
                SharedMemoryHeader *hdr = diag->header();
                ASSERT_NE(hdr, nullptr);

                uint64_t consumer_pid =
                    hdr->consumer_heartbeats[0].consumer_pid.load(std::memory_order_acquire);
                EXPECT_EQ(consumer_pid, 0u)
                    << "Dead consumer's heartbeat slot must be zeroed after cleanup";

                EXPECT_GT(hdr->recovery_actions_count.load(std::memory_order_acquire), 0u)
                    << "recovery_actions_count must be > 0 after cleanup removed a dead consumer";
            }

            producer.reset();
            cleanup_test_datablock(channel);
        },
        "dead_consumer_cleanup", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// 5. is_process_alive_false_for_nonexistent
// Verify datablock_is_process_alive(kDeadPid) returns false (no DataBlock needed).
// Verify datablock_is_process_alive(my_pid) returns true.
// ============================================================================

int is_process_alive_false_for_nonexistent()
{
    return run_gtest_worker(
        []()
        {
            EXPECT_FALSE(datablock_is_process_alive(kDeadPid))
                << "is_process_alive must return false for PID " << kDeadPid;

            uint64_t my_pid = pylabhub::platform::get_pid();
            EXPECT_TRUE(datablock_is_process_alive(my_pid))
                << "is_process_alive must return true for current process PID " << my_pid;
        },
        "is_process_alive_false_for_nonexistent", logger_module(), crypto_module(), hub_module());
}

// ============================================================================
// 6. force_reset_unsafe_when_writer_alive
// Create DataBlock, acquire write slot — write_lock = my_pid (alive).
// force_reset_slot(slot, force=false) must return RECOVERY_UNSAFE.
// release_zombie_writer must also return RECOVERY_UNSAFE.
// Then manually clear write_lock so producer.reset() doesn't crash.
// ============================================================================

int force_reset_unsafe_when_writer_alive()
{
    return run_gtest_worker(
        []()
        {
            std::string channel = make_test_channel_name("ForceResetUnsafe");
            DataBlockConfig cfg = make_recovery_config(77006);

            auto producer = create_datablock_producer_impl(channel,
                                                           DataBlockPolicy::RingBuffer,
                                                           cfg, nullptr, nullptr);
            ASSERT_NE(producer, nullptr);

            // Acquire write slot — write_lock = my_pid (alive process)
            auto wh = producer->acquire_write_slot(500);
            ASSERT_NE(wh, nullptr);

            // force_reset without force flag: alive write_lock → RECOVERY_UNSAFE
            RecoveryResult r_reset = datablock_force_reset_slot(channel.c_str(), 0, false);
            EXPECT_EQ(r_reset, RECOVERY_UNSAFE)
                << "force_reset_slot must return RECOVERY_UNSAFE when writer is alive";

            // release_zombie_writer: alive write_lock → RECOVERY_UNSAFE
            RecoveryResult r_zombie = datablock_release_zombie_writer(channel.c_str(), 0);
            EXPECT_EQ(r_zombie, RECOVERY_UNSAFE)
                << "release_zombie_writer must return RECOVERY_UNSAFE when writer is alive";

            // Clean up: release the write slot properly before destroying producer
            // (wh destructor will call release_write_slot, which aborts if not committed)
            static_cast<void>(producer->release_write_slot(*wh));
            producer.reset();
            cleanup_test_datablock(channel);
        },
        "force_reset_unsafe_when_writer_alive", logger_module(), crypto_module(), hub_module());
}

} // namespace pylabhub::tests::worker::recovery_scenarios

// ============================================================================
// Worker dispatcher registration
// ============================================================================

namespace
{
struct RecoveryScenarioWorkerRegistrar
{
    RecoveryScenarioWorkerRegistrar()
    {
        register_worker_dispatcher(
            [](int argc, char **argv) -> int
            {
                if (argc < 2)
                    return -1;
                std::string_view mode = argv[1];
                auto dot = mode.find('.');
                if (dot == std::string_view::npos || mode.substr(0, dot) != "recovery_scenarios")
                    return -1;
                std::string scenario(mode.substr(dot + 1));
                using namespace pylabhub::tests::worker::recovery_scenarios;
                if (scenario == "zombie_writer_detected_and_released")
                    return zombie_writer_detected_and_released();
                if (scenario == "zombie_readers_force_cleared")
                    return zombie_readers_force_cleared();
                if (scenario == "force_reset_slot_on_dead_writer")
                    return force_reset_slot_on_dead_writer();
                if (scenario == "dead_consumer_cleanup")
                    return dead_consumer_cleanup();
                if (scenario == "is_process_alive_false_for_nonexistent")
                    return is_process_alive_false_for_nonexistent();
                if (scenario == "force_reset_unsafe_when_writer_alive")
                    return force_reset_unsafe_when_writer_alive();
                fmt::print(stderr, "ERROR: Unknown recovery_scenarios scenario '{}'\n", scenario);
                return 1;
            });
    }
};
static RecoveryScenarioWorkerRegistrar s_recovery_scenario_registrar;
} // namespace
