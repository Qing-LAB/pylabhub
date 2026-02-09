#include "shared_test_helpers.h"
#include "utils/data_block.hpp"
#include "utils/message_hub.hpp"
#include <gtest/gtest.h>
#include <chrono>
#include <iostream>
#include <numeric>

using namespace pylabhub::hub;

class BenchmarkTest : public ::testing::Test {
protected:
    void SetUp() override {
        hub = MessageHub::get_instance();
        ASSERT_NE(hub, nullptr);
    }

    void TearDown() override {}

    MessageHub* hub;
    const std::string shm_name = "benchmark_test_db";
    const uint64_t shared_secret = 98765;
};

TEST_F(BenchmarkTest, WritePerformance) {
    DataBlockConfig config;
    config.shared_secret = shared_secret;
    config.flexible_zone_size = 0;
    config.ring_buffer_capacity = 128;
    config.unit_block_size = DataBlockUnitSize::Size4K;

    auto producer = create_datablock_producer(*hub, shm_name, DataBlockPolicy::RingBuffer, config);
    ASSERT_NE(producer, nullptr);

    const int num_messages = 100000;
    std::vector<char> test_data(1024, 'b');

    auto start_time = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_messages; ++i) {
        with_write_transaction(*producer, 1000, [&](SlotWriteHandle& slot) {
            slot.write(test_data.data(), test_data.size());
            slot.commit(test_data.size());
        });
    }

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    double seconds = duration.count() / 1000.0;
    double msgs_per_sec = num_messages / seconds;

    std::cout << "[ BENCHMARK ] WritePerformance: "
              << msgs_per_sec << " msgs/sec ("
              << num_messages << " messages in " << seconds << "s)" << std::endl;
    
    // Set a reasonable baseline. This will vary wildly based on hardware.
    // This is more of a smoke test to ensure it's not drastically slow.
    ASSERT_GT(msgs_per_sec, 1000); 
}
