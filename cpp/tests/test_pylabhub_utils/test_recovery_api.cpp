#include "shared_test_helpers.h"
#include "utils/data_block.hpp"
#include "utils/message_hub.hpp"
#include "plh_slot_diagnostics.hpp"
#include "plh_slot_recovery.hpp"
#include "test_process_utils.h" // For running worker processes
#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <string>

using namespace pylabhub::hub;

class RecoveryApiTest : public ::testing::Test {
protected:
    void SetUp() override {
        hub = MessageHub::get_instance();
        ASSERT_NE(hub, nullptr);
    }

    void TearDown() override {
        // Clean up any created datablocks if necessary
    }

    MessageHub* hub;
    const std::string shm_name = "recovery_api_test_db";
    const uint64_t shared_secret = 12345;
};

// Worker process function to simulate a crashed writer
void crashed_writer_worker(const std::string& shm_name, uint64_t secret) {
    auto hub = MessageHub::get_instance();
    DataBlockConfig config;
    config.shared_secret = secret;
    config.flexible_zone_size = 1024;
    config.ring_buffer_capacity = 1;
    config.unit_block_size = DataBlockUnitSize::Size4K;

    auto producer = create_datablock_producer(*hub, shm_name, DataBlockPolicy::RingBuffer, config);
    if (!producer) return;

    // Acquire a slot but never release it, simulating a crash
    auto slot = producer->acquire_write_slot(1000);
    if (slot) {
        // Exit without releasing the slot
        std::exit(0);
    }
    std::exit(1); // Should not be reached
}

TEST_F(RecoveryApiTest, CanDetectAndRecoverCrashedWriter) {
    // Launch a worker process that will crash
    TestProcess process(crashed_writer_worker, shm_name, shared_secret);
    process.wait(); // Wait for the worker to exit

    // Now, the datablock should be in a stuck state
    
    // 1. Diagnose the stuck slot
    SlotDiagnostics diagnostics(shm_name, 0);
    ASSERT_TRUE(diagnostics.is_stuck());
    ASSERT_NE(diagnostics.get_write_lock_pid(), 0);

    // 2. Check that the process is indeed dead
    ASSERT_FALSE(is_process_alive(diagnostics.get_write_lock_pid()));

    // 3. Recover the slot
    SlotRecovery recovery(shm_name, 0);
    auto result = recovery.release_zombie_writer();
    ASSERT_EQ(result, RECOVERY_SUCCESS);

    // 4. Verify the slot is now free
    diagnostics.refresh();
    ASSERT_FALSE(diagnostics.is_stuck());
    ASSERT_EQ(diagnostics.get_write_lock_pid(), 0);
    ASSERT_EQ(diagnostics.get_slot_state(), static_cast<uint8_t>(SlotState::FREE));
}

// Worker process function to simulate a crashed consumer
void crashed_consumer_worker(const std::string& shm_name, uint64_t secret) {
    auto hub = MessageHub::get_instance();
    auto consumer = find_datablock_consumer(*hub, shm_name, secret);
    if (!consumer) {
        std::exit(1);
    }

    HeartbeatManager heartbeat(*consumer);
    if (heartbeat.is_registered()) {
        // Exit without the HeartbeatManager's destructor being called
        std::exit(0);
    }
    std::exit(1); // Should not be reached
}

TEST_F(RecoveryApiTest, CanDetectAndCleanupCrashedConsumer) {
    DataBlockConfig config;
    config.shared_secret = shared_secret;
    config.flexible_zone_size = 1024;
    config.ring_buffer_capacity = 1;
    config.unit_block_size = DataBlockUnitSize::Size4K;

    auto producer = create_datablock_producer(*hub, shm_name, DataBlockPolicy::RingBuffer, config);
    ASSERT_NE(producer, nullptr);

    // Launch a worker process that will crash
    TestProcess process(crashed_consumer_worker, shm_name, shared_secret);
    process.wait(); // Wait for the worker to exit

    // At this point, the consumer is gone, but its heartbeat entry remains.
    
    // 1. Cleanup dead consumers
    auto cleanup_result = datablock_cleanup_dead_consumers(shm_name.c_str());
    ASSERT_EQ(cleanup_result, RECOVERY_SUCCESS);

    // 2. Verify that the active consumer count is now 0
    auto consumer = find_datablock_consumer(*hub, shm_name, shared_secret);
    const auto* header = consumer->get_header();
    ASSERT_NE(header, nullptr);
    ASSERT_EQ(header->active_consumer_count.load(), 0);
}

TEST_F(RecoveryApiTest, IntegrityValidatorDetectsCorruption) {
    DataBlockConfig config;
    config.shared_secret = shared_secret;
    config.flexible_zone_size = 1024;
    config.ring_buffer_capacity = 1;
    config.unit_block_size = DataBlockUnitSize::Size4K;

    auto producer = create_datablock_producer(*hub, shm_name, DataBlockPolicy::RingBuffer, config);
    ASSERT_NE(producer, nullptr);

    // Manually corrupt the magic number
    auto consumer = find_datablock_consumer(*hub, shm_name, shared_secret);
    auto* header = const_cast<SharedMemoryHeader*>(consumer->get_header());
    header->magic_number = 0xDEADBEEF; // Corrupt value

    // Validate integrity
    IntegrityValidator validator(shm_name);
    auto result = validator.validate();
    ASSERT_EQ(result, RECOVERY_FAILED);
}
