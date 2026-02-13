#include <iostream>
#include <vector>
#include <chrono>
#include <thread>
#include <numeric> // For std::iota
#include <optional>
#include <monostate> // For std::monostate with optional

#include "plh_datahub.hpp"
#include "utils/lifecycle.hpp"
#include "utils/data_block.hpp"
#include "utils/message_hub.hpp"
#include "utils/logger.hpp"

// Define a sample data structure (must match producer)
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
    const uint64_t shared_secret = 0xBAD5ECRET; // Example secret (must match producer)

    pylabhub::hub::MessageHub hub; // MessageHub is a placeholder in this example

    // --- Consumer Side ---
    std::cout << "--- Consumer Application Starting ---" << std::endl;

    std::unique_ptr<pylabhub::hub::DataBlockConsumer> consumer;
    try {
        consumer = pylabhub::hub::find_datablock_consumer(hub, channel_name, shared_secret);
        if (!consumer) {
            std::cerr << "Failed to find DataBlockConsumer! Is producer running?" << std::endl;
            return 1;
        }
        std::cout << "DataBlockConsumer found for channel: " << channel_name << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error finding consumer: " << e.what() << std::endl;
        return 1;
    }

    // --- Example 1: Using with_read_transaction ---
    std::cout << "
--- Example 1: with_read_transaction ---" << std::endl;
    // Assume producer has written some data at slot_id 0
    try {
        pylabhub::hub::with_read_transaction(
            *consumer, 0, 1000, [&](pylabhub::hub::ReadTransactionContext& ctx) {
                const auto& slot = ctx.slot();
                SensorData data;
                slot.read(&data, sizeof(SensorData));
                std::cout << "with_read_transaction: Read sequence " << data.sequence_num 
                          << ", Temp: " << data.temperature << std::endl;
            });
    } catch (const std::runtime_error& e) {
        std::cerr << "with_read_transaction failed for slot 0: " << e.what() << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Caught unexpected exception: " << e.what() << std::endl;
    }

    // --- Example 2: Using with_next_slot with DataBlockSlotIterator ---
    std::cout << "
--- Example 2: with_next_slot with DataBlockSlotIterator ---" << std::endl;
    pylabhub::hub::DataBlockSlotIterator iterator = consumer->slot_iterator();
    iterator.seek_latest(); // Start from the latest available slot

    int slots_read = 0;
    while (slots_read < 5) { // Try to read up to 5 slots
        std::optional<std::monostate> result = pylabhub::hub::with_next_slot(
            iterator, 100, // Wait up to 100ms for a new slot
            [&](const pylabhub::hub::SlotConsumeHandle& slot) {
                SensorData data;
                slot.read(&data, sizeof(SensorData));
                std::cout << "with_next_slot: Read sequence " << data.sequence_num 
                          << ", Temp: " << data.temperature << std::endl;
                slots_read++;
            });

        if (!result.has_value()) {
            std::cout << "with_next_slot: No new data for 100ms, waiting..." << std::endl;
            // Optionally sleep to prevent busy-waiting if producer is slow
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    std::cout << "with_next_slot: Finished reading " << slots_read << " slots." << std::endl;

    // --- Example 3: Using ReadTransactionGuard ---
    std::cout << "
--- Example 3: ReadTransactionGuard ---" << std::endl;
    // Producer needs to write more data for this example if ring_buffer_capacity is small
    // Assuming producer is still writing, or we read a slot we know exists
    try {
        // Acquire guard for slot_id 0 (if it's still valid/accessible)
        pylabhub::hub::ReadTransactionGuard guard(*consumer, 0, 1000);
        if (static_cast<bool>(guard)) { // Check if slot was successfully acquired
            auto slot_opt = guard.slot();
            if (slot_opt) {
                SensorData data;
                slot_opt->get().read(&data, sizeof(SensorData));
                std::cout << "ReadTransactionGuard: Read sequence " << data.sequence_num 
                          << ", Temp: " << data.temperature << std::endl;
            }
        } else {
            std::cerr << "ReadTransactionGuard: Failed to acquire slot 0." << std::endl;
        }
    } catch (const std::runtime_error& e) {
        std::cerr << "ReadTransactionGuard failed: " << e.what() << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Caught unexpected exception: " << e.what() << std::endl;
    }


    std::cout << "
--- Consumer Application Finished ---" << std::endl;

    return 0;
}