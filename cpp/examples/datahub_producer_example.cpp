#include <iostream>
#include <vector>
#include <chrono>
#include <thread>
#include <numeric> // For std::iota

#include "plh_datahub.hpp"
#include "utils/lifecycle.hpp"
#include "utils/data_block.hpp"
#include "utils/message_hub.hpp"
#include "utils/logger.hpp"

// Define a sample data structure
struct SensorData {
    uint64_t timestamp_ns;
    double temperature;
    double humidity;
    char sensor_name[16];
    uint32_t sequence_num;
};

int main()
{
    // LifecycleGuard initializes Logger, CryptoUtils, and Data Exchange Hub (required for DataBlock).
    pylabhub::utils::LifecycleGuard lifecycle(pylabhub::utils::MakeModDefList(
        pylabhub::utils::Logger::GetLifecycleModule(),
        pylabhub::crypto::GetLifecycleModule(),
        pylabhub::hub::GetLifecycleModule()));

    const std::string channel_name = "sensor_data_channel";
    const uint64_t shared_secret = 0xBAD5ECRET; // Example secret

    pylabhub::hub::MessageHub hub; // MessageHub is a placeholder in this example

    // --- Producer Side ---
    std::cout << "--- Producer Application Starting ---" << std::endl;

    pylabhub::hub::DataBlockConfig config;
    config.policy = pylabhub::hub::DataBlockPolicy::DoubleBuffer;
    config.consumer_sync_policy = pylabhub::hub::ConsumerSyncPolicy::Latest_only;
    config.shared_secret = shared_secret;
    config.flexible_zone_size = 0; // No flexible zone for this example
    config.physical_page_size = pylabhub::hub::DataBlockPageSize::Size4K; // 4KB page size
    config.ring_buffer_capacity = 2; // Double buffer for stable writes
    config.enable_checksum = true;
    config.checksum_policy = pylabhub::hub::ChecksumPolicy::Enforced;

    std::unique_ptr<pylabhub::hub::DataBlockProducer> producer;
    try {
        producer = pylabhub::hub::create_datablock_producer(
            hub, channel_name, pylabhub::hub::DataBlockPolicy::DoubleBuffer, config);
        if (!producer) {
            std::cerr << "Failed to create DataBlockProducer!" << std::endl;
            return 1;
        }
        std::cout << "DataBlockProducer created for channel: " << channel_name << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error creating producer: " << e.what() << std::endl;
        return 1;
    }

    // --- Example 1: Using with_write_transaction (Recommended for C++) ---
    std::cout << "
--- Example 1: with_write_transaction ---" << std::endl;
    SensorData data1 = {
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now().time_since_epoch()).count(),
        25.5, 60.2, "TempSensor01", 1
    };

    try {
        pylabhub::hub::with_write_transaction(
            *producer, 1000, [&](pylabhub::hub::WriteTransactionContext& ctx) {
                // Write data to the slot buffer
                auto& slot = ctx.slot();
                auto buffer_span = slot.buffer_span();
                if (buffer_span.size() < sizeof(SensorData)) {
                    throw std::runtime_error("Slot buffer too small for SensorData");
                }
                std::memcpy(buffer_span.data(), &data1, sizeof(SensorData));
                
                // Commit the data, making it visible to consumers
                slot.commit(sizeof(SensorData));
                std::cout << "with_write_transaction: SensorData sequence " << data1.sequence_num << " committed." << std::endl;
            });
    } catch (const std::exception& e) {
        std::cerr << "with_write_transaction failed: " << e.what() << std::endl;
    }
    
    // Demonstrate exception handling with with_write_transaction
    std::cout << "
--- Example 1.1: with_write_transaction with exception (should not commit) ---" << std::endl;
    SensorData data_exception = {
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now().time_since_epoch()).count(),
        99.9, 10.0, "FaultySensor", 2
    };
    try {
        pylabhub::hub::with_write_transaction(
            *producer, 1000, [&](pylabhub::hub::WriteTransactionContext& ctx) {
                auto buffer_span = ctx.slot().buffer_span();
                std::memcpy(buffer_span.data(), &data_exception, sizeof(SensorData));
                ctx.slot().commit(sizeof(SensorData));
                std::cout << "with_write_transaction: About to throw, data should not commit." << std::endl;
                throw std::runtime_error("Simulated network error during post-write processing");
            });
    } catch (const std::runtime_error& e) {
        std::cerr << "Caught expected exception from with_write_transaction: " << e.what() << std::endl;
        // Verify that commit_index did not advance
        // (Note: This verification logic would need access to internal header details,
        //  which is typically not available to end-users directly.
        //  For a real test, you'd check consumer side or internal metrics.)
    }


    // --- Example 2: Using WriteTransactionGuard (for more explicit control) ---
    std::cout << "
--- Example 2: WriteTransactionGuard ---" << std::endl;
    SensorData data2 = {
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now().time_since_epoch()).count(),
        30.1, 65.8, "TempSensor02", 3
    };

    try {
        // Acquire the guard
        pylabhub::hub::WriteTransactionGuard guard(*producer, 1000);
        if (!guard) {
            std::cerr << "WriteTransactionGuard: Failed to acquire slot." << std::endl;
            return 1;
        }

        // Access the slot through the guard
        auto slot_opt = guard.slot();
        if (!slot_opt) {
            std::cerr << "WriteTransactionGuard: slot() returned nullopt." << std::endl;
            return 1;
        }
        pylabhub::hub::SlotWriteHandle& slot = slot_opt->get();
        auto buffer_span = slot.buffer_span();
        if (buffer_span.size() < sizeof(SensorData)) {
            throw std::runtime_error("Slot buffer too small for SensorData");
        }
        std::memcpy(buffer_span.data(), &data2, sizeof(SensorData));
        
        // Explicitly commit the data (REQUIRED by WriteTransactionGuard pattern)
        slot.commit(sizeof(SensorData));
        std::cout << "WriteTransactionGuard: SensorData sequence " << data2.sequence_num << " committed." << std::endl;

        // Optionally mark guard as committed (for internal tracking/debugging, not implicit action)
        guard.commit(); 
        
        // Guard goes out of scope here and automatically releases the slot
    } catch (const std::exception& e) {
        std::cerr << "WriteTransactionGuard usage failed: " << e.what() << std::endl;
    }

    // Demonstrate explicit abort with WriteTransactionGuard
    std::cout << "
--- Example 2.1: WriteTransactionGuard with explicit abort ---" << std::endl;
    SensorData data_abort = {
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now().time_since_epoch()).count(),
        -5.0, 90.0, "AbortSensor", 4
    };
    try {
        pylabhub::hub::WriteTransactionGuard guard(*producer, 1000);
        if (!guard) {
            std::cerr << "WriteTransactionGuard (abort): Failed to acquire slot." << std::endl;
            return 1;
        }
        auto slot_opt = guard.slot();
        if (!slot_opt) {
            std::cerr << "WriteTransactionGuard (abort): slot() returned nullopt." << std::endl;
            return 1;
        }
        pylabhub::hub::SlotWriteHandle& slot = slot_opt->get();
        std::memcpy(slot.buffer_span().data(), &data_abort, sizeof(SensorData));
        slot.commit(sizeof(SensorData)); // Data is committed to the slot, but will be aborted

        std::cout << "WriteTransactionGuard: SensorData sequence " << data_abort.sequence_num << " written, then aborted." << std::endl;
        guard.abort(); // Explicitly abort; slot released, but not considered committed by the system
        // Data in slot will effectively be ignored by consumers if commit_index doesn't advance past it
    } catch (const std::exception& e) {
        std::cerr << "WriteTransactionGuard (abort) usage failed: " << e.what() << std::endl;
    }


    std::cout << "
--- Producer Application Finished ---" << std::endl;

    // A small delay to allow potential consumers to read
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    return 0;
}