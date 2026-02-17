/**
 * @file raii_layer_example.cpp
 * @brief Comprehensive example of Phase 3 C++ RAII Layer with BLDS Schema Validation
 * 
 * Demonstrates:
 * - Type-safe with_transaction API
 * - Non-terminating slot iterator
 * - Result<T, E> error handling
 * - SlotRef and ZoneRef typed access
 * - BLDS schema generation and validation
 * - Explicit heartbeat updates
 * - Proper checksum configuration
 */

#include <chrono>
#include <iostream>
#include <thread>
#include <atomic>
#include <fmt/format.h>

#include "utils/data_block.hpp"
#include "utils/message_hub.hpp"
#include "utils/schema_blds.hpp"

using namespace std::chrono_literals;
using namespace pylabhub::hub;

// ============================================================================
// Data Structures with BLDS Schema Registration
// ============================================================================
//
// DataHub Architecture:
//   - FlexZone: Shared metadata region (1 instance, all consumers see it)
//   - DataBlock: Ring buffer of slots (N instances, FIFO queue)
//
// In with_transaction<FlexZoneT, DataBlockT>():
//   - FlexZoneT  → Type for ctx.flexzone() (shared state)
//   - DataBlockT → Type for ctx.slots() (per-message data)
// ============================================================================

/**
 * @brief FlexZone structure: Shared metadata visible to all consumers
 * 
 * Memory layout:
 *   - Located in "Flexible Zone" region (config.flex_zone_size bytes)
 *   - Single instance shared across producer and all consumers
 *   - Used for: event flags, counters, configuration, synchronization
 *   - Size constraint: sizeof(FlexZoneMetadata) <= config.flex_zone_size
 * 
 * Access in RAII layer: ctx.flexzone() → ZoneRef<FlexZoneMetadata>
 */
struct FlexZoneMetadata
{
    std::atomic<uint32_t> active_producers{0};
    std::atomic<uint32_t> total_messages{0};
    std::atomic<bool> shutdown_flag{false};
    char description[64]{"RAII Layer Example"};
};

// Register BLDS schema for FlexZoneMetadata
// NOTE: In Phase 3, this schema is NOT stored in SharedMemoryHeader
//       (only used for compile-time generation and documentation)
// TODO: Phase 4 will store flexzone_schema_hash separately
PYLABHUB_SCHEMA_BEGIN(FlexZoneMetadata)
    PYLABHUB_SCHEMA_MEMBER(active_producers)
    PYLABHUB_SCHEMA_MEMBER(total_messages)
    PYLABHUB_SCHEMA_MEMBER(shutdown_flag)
    PYLABHUB_SCHEMA_MEMBER(description)
PYLABHUB_SCHEMA_END(FlexZoneMetadata)

/**
 * @brief DataBlock structure: Per-slot message data in ring buffer
 * 
 * Memory layout:
 *   - Located in "Ring Buffer" region (num_slots × slot_bytes)
 *   - N instances (one per slot in ring buffer)
 *   - Used for: actual message payloads, per-event data
 *   - Size constraint: sizeof(Message) <= config.ring_buffer.slot_bytes
 * 
 * Access in RAII layer: slot.content() → SlotRef<Message>
 */
struct Message
{
    uint64_t sequence_num{0};
    uint64_t timestamp_ns{0};
    uint32_t producer_id{0};
    uint32_t checksum{0};
    char payload[128]{"Hello from RAII Layer!"};
};

// Register BLDS schema for Message
// NOTE: In Phase 3, THIS is the schema stored in SharedMemoryHeader::schema_hash
//       (validated when consumer attaches)
PYLABHUB_SCHEMA_BEGIN(Message)
    PYLABHUB_SCHEMA_MEMBER(sequence_num)
    PYLABHUB_SCHEMA_MEMBER(timestamp_ns)
    PYLABHUB_SCHEMA_MEMBER(producer_id)
    PYLABHUB_SCHEMA_MEMBER(checksum)
    PYLABHUB_SCHEMA_MEMBER(payload)
PYLABHUB_SCHEMA_END(Message)

// ============================================================================
// Helper: Print Schema Information
// ============================================================================

void print_schema_info(const std::string &type_name, const pylabhub::schema::SchemaInfo &schema)
{
    std::cout << "\n--- Schema Info: " << type_name << " ---" << std::endl;
    std::cout << "  Name:    " << schema.name << std::endl;
    std::cout << "  Version: " << schema.version.to_string() << std::endl;
    std::cout << "  Size:    " << schema.struct_size << " bytes" << std::endl;
    std::cout << "  BLDS:    " << schema.blds << std::endl;
    std::cout << "  Hash:    ";
    for (size_t i = 0; i < 8; ++i)  // Print first 8 bytes for brevity
    {
        std::cout << fmt::format("{:02x}", schema.hash[i]);
    }
    std::cout << "..." << std::endl;
}

// ============================================================================
// Producer Example: Type-Safe Transaction with Iterator
// ============================================================================

void producer_example(std::shared_ptr<MessageHub> hub)
{
    std::cout << "\n=== Producer: RAII Layer Example ===\n" << std::endl;

    // ========================================================================
    // Step 1: Generate and print schema information
    // ========================================================================
    std::cout << "--- Step 1: Schema Generation ---" << std::endl;
    
    auto flexzone_schema = pylabhub::schema::generate_schema_info<FlexZoneMetadata>(
        "FlexZoneMetadata",
        pylabhub::schema::SchemaVersion{1, 0, 0}
    );
    print_schema_info("FlexZoneMetadata", flexzone_schema);
    
    auto message_schema = pylabhub::schema::generate_schema_info<Message>(
        "Message",
        pylabhub::schema::SchemaVersion{1, 0, 0}
    );
    print_schema_info("Message", message_schema);

    // ========================================================================
    // Step 2: Create datablock producer with schema validation
    // ========================================================================
    std::cout << "\n--- Step 2: Producer Creation ---" << std::endl;
    
    DataBlockConfig config;
    config.name = "raii_example";
    
    // Set physical page size (allocation unit)
    config.physical_page_size = DataBlockPageSize::Size_4K;  // 4096 bytes
    
    // Set logical slot size (0 = use physical_page_size)
    config.logical_unit_size = 0;  // Use 4K per slot (default)
    // Or explicitly: config.logical_unit_size = 8192;  // 2 pages per slot
    
    // Set number of slots
    config.ring_buffer_capacity = 16;  // 16 slots in ring buffer
    
    // Set flexible zone size (MUST be 0 or multiple of 4096)
    config.flex_zone_size = 4096;  // 1 page (must fit FlexZoneMetadata)
    
    // Set policies
    config.policy = DataBlockPolicy::RingBuffer;
    config.consumer_sync_policy = ConsumerSyncPolicy::Single_reader;
    config.checksum_type = ChecksumType::BLAKE2b;  // Only supported algorithm
    config.checksum_policy = ChecksumPolicy::Enforced;  // Automatic checksums

    // PHASE 4 API: Create producer with BOTH schema types
    // Both FlexZoneMetadata and Message schemas will be:
    //   - Generated at compile-time (BLDS)
    //   - Stored in SharedMemoryHeader (flexzone_schema_hash + datablock_schema_hash)
    //   - Validated when consumer attaches
    auto producer = create_datablock_producer<FlexZoneMetadata, Message>(
        //                                     ^^^^^^^^^^^^^^^^  ^^^^^^^ 
        //                                     Both types provided!
        *hub, "raii_example", 
        DataBlockPolicy::RingBuffer,
        config
        // No schema instances needed - generated from template types
    );

    if (!producer)
    {
        std::cerr << "Failed to create producer" << std::endl;
        return;
    }

    std::cout << "✓ Producer created with dual schema validation" << std::endl;
    std::cout << "  FlexZoneMetadata BLDS hash → flexzone_schema_hash[32]" << std::endl;
    std::cout << "  Message BLDS hash → datablock_schema_hash[32]" << std::endl;

    // ========================================================================
    // Example 1: Basic with_transaction with typed access
    // ========================================================================
    std::cout << "\n--- Example 1: Basic Transaction ---" << std::endl;
    std::cout << "  with_transaction<FlexZoneMetadata, Message>() uses:" << std::endl;
    std::cout << "    - FlexZoneMetadata for ctx.flexzone() access" << std::endl;
    std::cout << "    - Message for ctx.slots() access" << std::endl;

    try
    {
        producer->with_transaction<FlexZoneMetadata, Message>(
            //                     ^^^^^^^^^^^^^^^^  ^^^^^^^ 
            //                     FlexZone type     DataBlock/slot type
            100ms,
            [](WriteTransactionContext<FlexZoneMetadata, Message> &ctx)
            {
                // Access flexible zone (shared metadata)
                auto zone = ctx.flexzone();  // Returns ZoneRef<FlexZoneMetadata>
                zone.get().active_producers.store(1, std::memory_order_relaxed);
                zone.get().total_messages.store(0, std::memory_order_relaxed);
                zone.get().shutdown_flag.store(false, std::memory_order_relaxed);
                
                std::cout << "✓ Initialized flexible zone: " << zone.get().description << std::endl;
                std::cout << "  NOTE: with_transaction() performs full validation:" << std::endl;
                std::cout << "    ✓ Compile-time: static_assert(is_trivially_copyable)" << std::endl;
                std::cout << "    ✓ Runtime sizeof(): FlexZoneMetadata=" << sizeof(FlexZoneMetadata) 
                          << " <= " << 4096 << std::endl;
                std::cout << "    ✓ Runtime sizeof(): Message=" << sizeof(Message) 
                          << " <= " << 4096 << std::endl;
                std::cout << "    ✓ BLDS hash: Validated at producer/consumer creation (Phase 4)" << std::endl;
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
            for (auto &slot : ctx.slots(50ms))
            {
                // Check for errors (timeout/no-slot/fatal)
                if (!slot.is_ok())
                {
                    auto err = slot.error();
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
                auto &content = slot.content();
                
                // Type-safe access
                content.get().sequence_num = messages_sent;
                content.get().timestamp_ns = std::chrono::steady_clock::now().time_since_epoch().count();
                content.get().producer_id = 1;
                content.get().checksum = messages_sent * 42;  // Simple checksum
                
                // Raw access if needed
                // auto raw_span = content.raw_access();
                
                std::cout << "  [Sent] Message #" << messages_sent 
                          << " (slot_id=" << content.slot_id() << ")" << std::endl;

                // Commit the slot
                ctx.publish();

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
                for (auto &slot : ctx.slots(50ms))
                {
                    if (!slot.is_ok())
                    {
                        continue;
                    }

                    auto &content = slot.content();
                    content.get().sequence_num = 999;
                    
                    // Simulate error before commit
                    throw std::runtime_error("Simulated error - slot NOT committed");
                    
                    // This line never executes - slot is automatically cleaned up
                    ctx.publish();  // NOLINT
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

    // ========================================================================
    // Step 1: Attach to datablock with schema validation
    // ========================================================================
    std::cout << "--- Step 1: Consumer Attachment ---" << std::endl;
    
    DataBlockConfig expected_config;
    expected_config.name = "raii_example";
    expected_config.physical_page_size = DataBlockPageSize::Size_4K;
    expected_config.logical_unit_size = 0;  // Match producer (use physical)
    expected_config.ring_buffer_capacity = 16;
    expected_config.flex_zone_size = 4096;

    // PHASE 4 API: Attach consumer with BOTH schema types
    // Both schemas will be validated against producer's stored hashes
    auto consumer = find_datablock_consumer<FlexZoneMetadata, Message>(
        //                                   ^^^^^^^^^^^^^^^^  ^^^^^^^ 
        //                                   Both types must match producer!
        *hub, "raii_example", 
        0,  // Shared secret (0 = default/discover)
        expected_config
        // No schema instances needed - generated from template types
    );

    if (!consumer)
    {
        std::cerr << "Failed to attach consumer" << std::endl;
        return;
    }

    std::cout << "✓ Consumer attached with dual schema validation" << std::endl;
    std::cout << "  FlexZoneMetadata schema hash verified" << std::endl;
    std::cout << "  Message schema hash verified" << std::endl;

    // ========================================================================
    // Step 2: Read with validation and heartbeat
    // ========================================================================
    std::cout << "\n--- Step 2: Reading Messages with RAII Transaction ---" << std::endl;

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

            for (auto &slot : ctx.slots(50ms))
            {
                if (!slot.is_ok())
                {
                    auto err = slot.error();
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

                auto &content = slot.content();
                
                // Validate read (checksum, staleness, etc.)
                if (!ctx.validate_read())
                {
                    std::cerr << "  [Invalid] Slot validation failed" << std::endl;
                    continue;
                }

                // Type-safe read
                const auto &msg = content.get();
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
