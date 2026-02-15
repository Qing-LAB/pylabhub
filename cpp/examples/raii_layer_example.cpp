/**
 * @file raii_layer_example.cpp
 * @brief Comprehensive example of Phase 3 C++ RAII Layer
 * 
 * Demonstrates:
 * - Type-safe with_transaction API
 * - Non-terminating slot iterator
 * - Result<T, E> error handling
 * - SlotRef and ZoneRef typed access
 * - Schema validation
 * - Explicit heartbeat updates
 */

#include <chrono>
#include <iostream>
#include <thread>
#include <atomic>
#include <fmt/format.h>

#include "utils/data_block.hpp"
#include "utils/message_hub.hpp"

using namespace std::chrono_literals;
using namespace pylabhub::hub;

// ============================================================================
// Data Structures
// ============================================================================

// Flexible zone structure (shared metadata)
struct FlexZoneMetadata
{
    std::atomic<uint32_t> active_producers{0};
    std::atomic<uint32_t> total_messages{0};
    std::atomic<bool> shutdown_flag{false};
    char description[64]{"RAII Layer Example"};
};

// Slot data structure (per-message data)
struct Message
{
    uint64_t sequence_num{0};
    uint64_t timestamp_ns{0};
    uint32_t producer_id{0};
    uint32_t checksum{0};
    char payload[128]{"Hello from RAII Layer!"};
};

// ============================================================================
// Producer Example: Type-Safe Transaction with Iterator
// ============================================================================

void producer_example(std::shared_ptr<MessageHub> hub)
{
    std::cout << "\n=== Producer: RAII Layer Example ===\n" << std::endl;

    // Create datablock with schema
    DataBlockConfig config;
    config.name = "raii_example";
    config.ring_buffer.num_slots = 16;
    config.ring_buffer.slot_bytes = 256;
    config.flex_zone_size = 4096;  // 4K for FlexZoneMetadata
    config.enable_checksum = ChecksumType::CRC32;

    auto producer = create_datablock_producer<Message>(
        hub, "raii_example", 
        CreationPolicy::CreateOrAttach,
        config);

    if (!producer)
    {
        std::cerr << "Failed to create producer" << std::endl;
        return;
    }

    std::cout << "Producer created successfully\n" << std::endl;

    // ========================================================================
    // Example 1: Basic with_transaction with typed access
    // ========================================================================
    std::cout << "--- Example 1: Basic Transaction ---" << std::endl;

    try
    {
        producer->with_transaction<FlexZoneMetadata, Message>(
            100ms,
            [](WriteTransactionContext<FlexZoneMetadata, Message> &ctx)
            {
                // Initialize flexible zone
                auto zone = ctx.flexzone();
                zone.get().active_producers.store(1, std::memory_order_relaxed);
                zone.get().total_messages.store(0, std::memory_order_relaxed);
                zone.get().shutdown_flag.store(false, std::memory_order_relaxed);
                
                std::cout << "Initialized flexible zone: " << zone.get().description << std::endl;
            });
    }
    catch (const std::exception &e)
    {
        std::cerr << "Transaction failed: " << e.what() << std::endl;
        return;
    }

    // ========================================================================
    // Example 2: Non-terminating Iterator with Result handling
    // ========================================================================
    std::cout << "\n--- Example 2: Non-Terminating Iterator ---" << std::endl;

    producer->with_transaction<FlexZoneMetadata, Message>(
        100ms,
        [](WriteTransactionContext<FlexZoneMetadata, Message> &ctx)
        {
            auto zone = ctx.flexzone();
            uint32_t messages_sent = 0;
            const uint32_t target_messages = 10;

            // Non-terminating iterator - yields Result<SlotRef, Error>
            for (auto slot_result : ctx.slots(50ms))
            {
                // Check for errors (timeout/no-slot/fatal)
                if (!slot_result.is_ok())
                {
                    auto err = slot_result.error();
                    if (err == SlotAcquireError::Timeout)
                    {
                        std::cout << "  [Timeout] No slot available, checking shutdown..." << std::endl;
                        
                        // Update heartbeat during idle time
                        ctx.update_heartbeat();
                        
                        // Check shutdown flag
                        if (zone.get().shutdown_flag.load(std::memory_order_relaxed))
                        {
                            std::cout << "  [Shutdown] Breaking out of iterator" << std::endl;
                            break;
                        }
                        continue;  // Retry
                    }
                    else if (err == SlotAcquireError::NoSlot)
                    {
                        std::cout << "  [NoSlot] Ring buffer full, waiting..." << std::endl;
                        std::this_thread::sleep_for(10ms);
                        continue;
                    }
                    else  // Fatal error
                    {
                        std::cerr << "  [Fatal] Unrecoverable error, exiting" << std::endl;
                        break;
                    }
                }

                // Success! Got a slot
                auto &slot = slot_result.content();
                
                // Type-safe access
                slot.get().sequence_num = messages_sent;
                slot.get().timestamp_ns = std::chrono::steady_clock::now().time_since_epoch().count();
                slot.get().producer_id = 1;
                slot.get().checksum = messages_sent * 42;  // Simple checksum
                
                // Raw access if needed
                // auto raw_span = slot.raw_access();
                
                std::cout << "  [Sent] Message #" << messages_sent 
                          << " (slot_id=" << slot.slot_id() << ")" << std::endl;

                // Commit the slot
                ctx.commit();

                // Update flexible zone counter
                zone.get().total_messages.fetch_add(1, std::memory_order_relaxed);
                
                messages_sent++;
                if (messages_sent >= target_messages)
                {
                    std::cout << "  [Complete] Sent " << messages_sent << " messages" << std::endl;
                    break;  // User-controlled break
                }
            }

            std::cout << "  Total messages in flexzone: " 
                      << zone.get().total_messages.load(std::memory_order_relaxed) << std::endl;
        });

    // ========================================================================
    // Example 3: Exception Safety
    // ========================================================================
    std::cout << "\n--- Example 3: Exception Safety ---" << std::endl;

    try
    {
        producer->with_transaction<FlexZoneMetadata, Message>(
            100ms,
            [](WriteTransactionContext<FlexZoneMetadata, Message> &ctx)
            {
                for (auto slot_result : ctx.slots(50ms))
                {
                    if (!slot_result.is_ok())
                    {
                        continue;
                    }

                    auto &slot = slot_result.content();
                    slot.get().sequence_num = 999;
                    
                    // Simulate error before commit
                    throw std::runtime_error("Simulated error - slot NOT committed");
                    
                    // This line never executes - slot is automatically cleaned up
                    ctx.commit();  // NOLINT
                    break;
                }
            });
    }
    catch (const std::exception &e)
    {
        std::cout << "  Caught exception (expected): " << e.what() << std::endl;
        std::cout << "  Context destructor ensures cleanup" << std::endl;
    }

    std::cout << "\nProducer example complete!" << std::endl;
}

// ============================================================================
// Consumer Example: Type-Safe Reading with Schema Validation
// ============================================================================

void consumer_example(std::shared_ptr<MessageHub> hub)
{
    std::cout << "\n=== Consumer: RAII Layer Example ===\n" << std::endl;

    // Attach to existing datablock with schema validation
    DataBlockConfig expected_config;
    expected_config.name = "raii_example";
    expected_config.ring_buffer.num_slots = 16;
    expected_config.ring_buffer.slot_bytes = 256;
    expected_config.flex_zone_size = 4096;

    auto consumer = find_datablock_consumer<Message>(
        hub, "raii_example", 
        std::nullopt,  // No shared secret
        expected_config);

    if (!consumer)
    {
        std::cerr << "Failed to attach consumer" << std::endl;
        return;
    }

    std::cout << "Consumer attached successfully\n" << std::endl;

    // ========================================================================
    // Example: Read with validation and heartbeat
    // ========================================================================
    std::cout << "--- Consumer: Reading Messages ---" << std::endl;

    consumer->with_transaction<FlexZoneMetadata, Message>(
        100ms,
        [](ReadTransactionContext<FlexZoneMetadata, Message> &ctx)
        {
            auto zone = ctx.flexzone();
            uint32_t messages_read = 0;
            const uint32_t max_to_read = 10;

            std::cout << "Flexible zone: " << zone.get().description << std::endl;
            std::cout << "Total messages: " 
                      << zone.get().total_messages.load(std::memory_order_relaxed) << std::endl;

            for (auto slot_result : ctx.slots(50ms))
            {
                if (!slot_result.is_ok())
                {
                    auto err = slot_result.error();
                    if (err == SlotAcquireError::Timeout)
                    {
                        std::cout << "  [Timeout] No new messages" << std::endl;
                        
                        // Update heartbeat during idle
                        ctx.update_heartbeat();
                        
                        // Check shutdown
                        if (zone.get().shutdown_flag.load(std::memory_order_relaxed))
                        {
                            break;
                        }
                        
                        if (messages_read >= max_to_read)
                        {
                            break;  // Read enough
                        }
                        continue;
                    }
                    else
                    {
                        std::cerr << "  [Error] Failed to acquire slot" << std::endl;
                        break;
                    }
                }

                auto &slot = slot_result.value();
                
                // Validate read (checksum, staleness, etc.)
                if (!ctx.validate_read())
                {
                    std::cerr << "  [Invalid] Slot validation failed" << std::endl;
                    continue;
                }

                // Type-safe read
                const auto &msg = slot.get();
                std::cout << "  [Read] seq=" << msg.sequence_num
                          << " timestamp=" << msg.timestamp_ns
                          << " producer=" << msg.producer_id
                          << " checksum=" << msg.checksum
                          << " payload=\"" << msg.payload << "\""
                          << std::endl;

                messages_read++;
                if (messages_read >= max_to_read)
                {
                    std::cout << "  [Done] Read " << messages_read << " messages" << std::endl;
                    break;
                }
            }
        });

    std::cout << "\nConsumer example complete!" << std::endl;
}

// ============================================================================
// Main
// ============================================================================

int main()
{
    std::cout << "╔════════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║       PyLabHub C++ RAII Layer - Comprehensive Example       ║" << std::endl;
    std::cout << "╚════════════════════════════════════════════════════════════╝" << std::endl;

    try
    {
        // Initialize message hub
        auto hub = create_message_hub("/tmp/pylabhub_raii_example");
        if (!hub)
        {
            std::cerr << "Failed to create message hub" << std::endl;
            return 1;
        }

        // Run producer
        producer_example(hub);

        // Small delay
        std::this_thread::sleep_for(100ms);

        // Run consumer
        consumer_example(hub);

        std::cout << "\n╔════════════════════════════════════════════════════════════╗" << std::endl;
        std::cout << "║                    All Examples Complete!                   ║" << std::endl;
        std::cout << "╚════════════════════════════════════════════════════════════╝" << std::endl;

        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
}
