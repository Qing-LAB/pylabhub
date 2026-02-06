| Property       | Value                                      |
| -------------- | ------------------------------------------ |
| **HEP**        | `core-0002`                                |
| **Title**      | A Framework for Inter-Process Data Exchange|
| **Author**     | Quan Qing <quan.qing@asu.edu>              |
| **Status**     | Draft                                      |
| **Category**   | Core                                       |
| **Created**    | 2026-01-07                                 |
| **C++-Standard** | C++20                                      |

## Abstract

This Hub Enhancement Proposal (HEP) outlines a definitive design for the **Data Exchange Hub**. The architecture is centered around a **Message Hub** that provides a unified event model and coordinates access to high-performance, queue-like **DataBlocks**. Each DataBlock is a shared memory channel with a policy-based buffer, a stateful **Flexible Data Zone**, and robust safety features like magic numbers and shared secrets. All communication is managed by a strict, header-based messaging protocol and a central broker that actively tracks consumer heartbeats to ensure system integrity. This design provides a comprehensive, secure, and resilient framework for complex data-flow applications.

## Motivation

Modern scientific and laboratory environments often require integrating multiple, independent software tools and hardware instruments into a cohesive system. A centralized and standardized data exchange mechanism is needed to:
-   Decouple data producers from consumers in complex data-flow pipelines.
-   Provide a unified interface for system control and state monitoring with robust error detection.
-   Enable both low-latency "latest value" streaming and reliable, lossless data queuing.
-   Ensure data integrity, security, and comprehensive logging of experimental parameters and states.

The Data Exchange Hub aims to provide this foundational IPC layer, simplifying the development of complex, multi-process experimental control and data acquisition systems.

## Specification

The framework consists of two primary modules unified under a common discovery system:

1.  **The Message Hub**: A statically managed `Lifecycle` module built on ZeroMQ. It provides the core infrastructure for service discovery, control messaging, and data notifications, all governed by a strict messaging protocol.

2.  **The DataBlock Hub**: A module providing high-performance, shared-memory `DataBlock` channels. These channels function as policy-managed buffers and are coordinated entirely by the Message Hub.

### 1. High-Performance Channel (DataBlock Hub)

-   **Purpose**: To provide versatile and secure shared memory channels optimized for different data exchange patterns.
-   **Policy-Based Buffer Management**: The `StructuredDataBuffer` within a `DataBlock` is managed by a user-chosen policy (e.g., `SingleBuffer`, `DoubleBuffer`, `RingBuffer`) to best fit the application's needs.
-   **Stateful DataBlocks**: Every `DataBlock` contains a **Flexible Data Zone** to hold a MessagePack/JSON object, allowing each data block to be associated with a rich, variable "state".

#### 1.1. Shared Memory Layout and Security

A `DataBlock` segment contains a header followed by the data region.
1.  **`SharedMemoryHeader`**: Contains control primitives, security features, and metadata.
2.  **`FlexibleDataZone`**: The buffer for the state object.
3.  **`StructuredDataBuffer`**: The region for bulk data, interpreted by the chosen policy.

The `SharedMemoryHeader` is critical for security and coordination and will contain:
-   **Safety & Identification**:
    -   `uint64_t magic_number`: A constant value (e.g., `0xBADF00DFEEDFACEL`) to validate that the memory is an initialized `DataBlock` and to check for version compatibility.
    -   `uint64_t shared_secret`: A key that must be known by connecting consumers to prevent unauthorized access.
-   **Consumer Management**:
    -   `atomic<uint32_t> active_consumer_count`: An atomic counter for tracking attached consumers.
-   **Control & State**:
    -   **Process-Shared Mutex**: For ensuring atomic writes by a producer.
    -   **Policy-Specific State**: Atomics for managing the buffer policy (e.g., `write_index`, `commit_index`, `read_index` for a ring buffer).

### 2. Message Hub and Protocol

#### 2.1. Strict Message Protocol

To ensure performance and reliability, all messages exchanged via the Message Hub (using ZeroMQ) **must** adhere to a strict, two-part structure:

1.  **Frame 1: Header (16 bytes)**: A fixed-size `const char[16]` frame containing a "magic string" that uniquely identifies the message type. This allows for high-performance routing without payload deserialization. Examples:
    *   `"PYLABHUB_DB_NOTIFY"`: A `DataBlock` data-ready notification.
    *   `"PYLABHUB_HB_REQ"`: A client heartbeat request.
    *   `"PYLABHUB_REG_REQ"`: A channel registration request.
2.  **Frame 2: Payload**: The actual message content, serialized using **MessagePack**.

#### 2.2. Data Notification Channels

All data availability is signaled via ZeroMQ `PUB/SUB` **Notification Channels**, adhering to the strict message protocol. The payload of a `PYLABHUB_DB_NOTIFY` message would contain details like the `slot_index` that is ready for consumption.

### 3. Consumer Management and Coordination Protocol

A robust system requires that all participants are well-behaved and that failures are detected gracefully.

#### 3.1. Broker-Managed Heartbeats

The central broker is responsible for tracking not just producers, but every individual consumer.
-   **Consumer Heartbeats**: Each `DataBlockConsumer` instance must establish its own connection to the broker and send periodic heartbeats (`PYLABHUB_HB_REQ`).
-   **Drop-off Detection**: If the broker does not receive a heartbeat from a consumer within a configured timeout, it will consider that consumer "dead".
-   **Failure Broadcast**: The broker will then broadcast a special management message (e.g., `"PYLABHUB_CONS_DROP"`) containing the ID of the dropped consumer. Producers and other consumers can subscribe to this message to correctly manage resources, such as releasing a ring buffer slot that was held by the dead consumer.

#### 3.2. Well-Behaved Consumer Rules

A consumer of a `DataBlock` must adhere to the following protocol:
1.  On creation, it must know the `shared_secret` to successfully map the memory. It then registers with the broker and begins sending heartbeats.
2.  It subscribes to the appropriate Notification Channel.
3.  Upon receiving a notification for a slot, it may acquire a read lock on that slot (`begin_consume`).
4.  After it has finished processing the data, it **must** release the slot (`end_consume`) to make it available for future writes. Failure to do so will stall the pipeline.
5.  On destruction, its destructor must cleanly unregister from the broker.

### 4. C++ API Design Principles

The API is designed to expose a clean, policy-driven interface for high-performance inter-process communication using shared memory. It provides distinct interfaces for producers and consumers, orchestrated by the `MessageHub` for coordination.

```cpp
// Namespace for all Data Exchange Hub components
namespace pylabhub::hub {

// Forward declaration for MessageHub
class MessageHub;

/**
 * @enum DataBlockPolicy
 * @brief Defines the buffer management strategy for a DataBlock.
 */
enum class DataBlockPolicy { Single, DoubleBuffer, RingBuffer };

/**
 * @struct DataBlockConfig
 * @brief Configuration for creating a new DataBlock.
 *
 * This configuration is provided by the producer and is essential for
 * creating the shared memory segment and initializing its header.
 */
struct DataBlockConfig {
    uint64_t shared_secret;           // Key for unauthorized access prevention
    size_t structured_buffer_size;    // Size of the bulk data buffer
    size_t flexible_zone_size;        // Size of the flexible data zone for MessagePack/JSON
    int ring_buffer_capacity;         // Only for RingBufferPolicy: number of slots
};

/**
 * @class IDataBlockProducer
 * @brief Interface for a DataBlock producer.
 *
 * This interface defines the contract for writing data to a shared DataBlock.
 * Specific implementations will manage policy-based buffering.
 */
class IDataBlockProducer {
public:
    virtual ~IDataBlockProducer() = default;

    /**
     * @brief Acquires a slot for writing data.
     * @param timeout_ms Maximum time to wait for a slot to become available.
     * @return Pointer to the writeable structured data buffer, or nullptr on timeout/failure.
     */
    virtual char* begin_write(int timeout_ms = 0) = 0;

    /**
     * @brief Commits the data written to the slot, making it available for consumers.
     * @param bytes_written The actual number of bytes written to the structured buffer.
     * @param flexible_data Optional JSON object for the flexible data zone.
     * @return True on success, false on failure.
     */
    virtual bool end_write(size_t bytes_written, const nlohmann::json* flexible_data = nullptr) = 0;

    /**
     * @brief Accesses the mutable flexible data zone directly.
     * @return Pointer to the flexible data zone.
     */
    virtual char* flexible_zone() = 0;
};

/**
 * @class IDataBlockConsumer
 * @brief Interface for a DataBlock consumer.
 *
 * This interface defines the contract for reading data from a shared DataBlock.
 * Specific implementations will manage policy-based buffering.
 */
class IDataBlockConsumer {
public:
    virtual ~IDataBlockConsumer() = default;

    /**
     * @brief Acquires a slot for reading data.
     * @param timeout_ms Maximum time to wait for data to become available.
     * @return Pointer to the readable structured data buffer, or nullptr on timeout/failure.
     */
    virtual const char* begin_consume(int timeout_ms = 0) = 0;

    /**
     * @brief Releases the consumed slot.
     * @return True on success, false on failure.
     */
    virtual bool end_consume() = 0;

    /**
     * @brief Accesses the read-only flexible data zone directly.
     * @return Pointer to the flexible data zone.
     */
    virtual const char* flexible_zone() const = 0;
};

/**
 * @brief Factory function to create a DataBlock producer.
 * @param hub A connected MessageHub instance for broker communication.
 * @param name The unique name for the DataBlock channel.
 * @param policy The buffer management policy to use.
 * @param config The configuration for the DataBlock.
 * @return A unique_ptr to the producer, or nullptr on failure.
 */
std::unique_ptr<IDataBlockProducer> create_datablock_producer(
    MessageHub &hub,
    const std::string &name,
    DataBlockPolicy policy,
    const DataBlockConfig &config
);

/**
 * @brief Factory function to find and connect to a DataBlock as a consumer.
 * @param hub A connected MessageHub instance for broker communication.
 * @param name The name of the DataBlock channel to find.
 * @param shared_secret The secret required to access the channel.
 * @return A unique_ptr to the consumer, or nullptr on failure.
 */
std::unique_ptr<IDataBlockConsumer> find_datablock_consumer(
    MessageHub &hub,
    const std::string &name,
    uint64_t shared_secret
);

} // namespace pylabhub::hub
```

#### 4.1. Internal DataBlock Management

The core shared memory segment management is handled by an internal `DataBlock` helper class (defined in `src/utils/data_block.cpp`). This class encapsulates the Boost.Interprocess `managed_shared_memory` and provides direct access to the `SharedMemoryHeader`, `FlexibleDataZone`, and `StructuredDataBuffer`. It handles the creation and destruction of the named shared memory object, ensuring proper lifecycle management.

#### 4.2. Synchronization Primitives

The `SharedMemoryHeader` plays a crucial role in coordinating access and ensuring data integrity within the shared memory segment. It contains several atomic variables and a process-shared mutex:

*   **`uint64_t magic_number`**: A fixed constant (`0xBADF00DFEEDFACEL`) used by consumers to verify that the shared memory segment is indeed a valid `DataBlock` and is compatible with the expected version.
*   **`uint64_t shared_secret`**: A security key that must be supplied by consumers to gain access, preventing unauthorized processes from reading or writing to the DataBlock.
*   **`uint32_t version`**: Indicates the version of the `SharedMemoryHeader` layout, allowing for robust version checking and potential backward/forward compatibility.
*   **`uint32_t header_size`**: Stores the size of the `SharedMemoryHeader` structure itself, which is used to calculate the starting offsets of the `FlexibleDataZone` and `StructuredDataBuffer` within the shared memory.
*   **`std::atomic<uint32_t> active_consumer_count`**: An atomic counter that tracks the number of currently active consumers connected to this `DataBlock`. This is crucial for the broker's heartbeat mechanism and for producers to manage resources. Consumers increment this on successful connection and decrement on disconnection.
*   **`std::atomic<uint64_t> write_index`**: A policy-specific atomic variable, primarily used by the producer to indicate the next available buffer slot or write position within the `StructuredDataBuffer`.
*   **`std::atomic<uint64_t> commit_index`**: A policy-specific atomic variable, used by the producer to signal that the data in a particular slot or at a specific write position has been fully written and is ready for consumers to read.
*   **`std::atomic<uint64_t> read_index`**: A policy-specific atomic variable, used by consumers to track which data slots have been processed or are currently being read.
*   **`std::atomic<uint64_t> current_slot_id`**: A monotonically increasing identifier assigned to each new data unit (slot) written by the producer. Consumers can use this ID to detect new data availability and ensure ordered processing, especially in ring buffer scenarios.
*   **`char mutex_storage[64]`**: This provides storage for a process-shared mutex (e.g., `pthread_mutex_t` on POSIX systems, or named mutexes on Windows). This mutex is used to protect critical sections of the `SharedMemoryHeader` and coordinate complex multi-step operations (like buffer management) that require exclusive access, particularly for producers.

### Example Usage

Here are conceptual examples demonstrating how a producer and consumer might interact with the Data Exchange Hub API:

```cpp
#include "plh_service.hpp" // For LOGGER_INFO etc.
#include "utils/data_block.hpp"
#include "utils/message_hub.hpp"
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>

// --- Producer Example ---
void producer_example(const std::string& name, uint64_t secret) {
    pylabhub::hub::MessageHub message_hub;
    // Assume message_hub is connected to the broker (e.g., message_hub.connect("tcp://localhost:5555", "SERVER_KEY");)

    pylabhub::hub::DataBlockConfig config;
    config.shared_secret = secret;
    config.structured_buffer_size = 1024; // 1KB buffer for structured data
    config.flexible_zone_size = 256;      // 256 bytes for flexible data (JSON)
    config.ring_buffer_capacity = 4;      // For RingBuffer policy

    try {
        std::unique_ptr<pylabhub::hub::IDataBlockProducer> producer =
            pylabhub::hub::create_datablock_producer(message_hub, name, pylabhub::hub::DataBlockPolicy::RingBuffer, config);

        if (!producer) {
            LOGGER_ERROR("Producer: Failed to create DataBlock '{}'.", name);
            return;
        }

        LOGGER_INFO("Producer: DataBlock '{}' created. Starting to write data...", name);

        for (int i = 0; i < 10; ++i) {
            char* buffer = producer->begin_write(100); // Wait up to 100ms for a slot
            if (buffer) {
                // Write some structured data
                std::string msg = "Hello from producer " + std::to_string(i);
                size_t bytes_to_write = std::min(msg.length() + 1, config.structured_buffer_size);
                memcpy(buffer, msg.c_str(), bytes_to_write);

                // Write some flexible data (JSON)
                nlohmann::json flex_data;
                flex_data["sequence"] = i;
                flex_data["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                flex_data["producer_id"] = "my_app_1";

                if (producer->end_write(bytes_to_write, &flex_data)) {
                    LOGGER_INFO("Producer: Wrote data for sequence {}", i);
                    // In a real scenario, producer would notify broker via MessageHub
                    // message_hub.send_notification("PYLABHUB_DB_NOTIFY", {{"name", name}, {"slot_id", producer->current_slot_id()}});
                } else {
                    LOGGER_ERROR("Producer: Failed to commit write for sequence {}", i);
                }
            } else {
                LOGGER_WARN("Producer: Could not acquire write slot for sequence {}", i);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        LOGGER_INFO("Producer: Finished writing data to DataBlock '{}'.", name);

    } catch (const std::exception& e) {
        LOGGER_ERROR("Producer: An error occurred: {}", e.what());
    }
}

// --- Consumer Example ---
void consumer_example(const std::string& name, uint64_t secret) {
    pylabhub::hub::MessageHub message_hub;
    // Assume message_hub is connected to the broker

    try {
        std::unique_ptr<pylabhub::hub::IDataBlockConsumer> consumer =
            pylabhub::hub::find_datablock_consumer(message_hub, name, secret);

        if (!consumer) {
            LOGGER_ERROR("Consumer: Failed to find DataBlock '{}' with secret {:#x}.", name, secret);
            return;
        }

        LOGGER_INFO("Consumer: Connected to DataBlock '{}'. Starting to read data...", name);

        for (int i = 0; i < 15; ++i) { // Try to read more than producer writes to test blocking
            const char* buffer = consumer->begin_consume(500); // Wait up to 500ms for data
            if (buffer) {
                // Read structured data
                std::string received_msg(buffer);
                LOGGER_INFO("Consumer: Received structured data: '{}'", received_msg);

                // Read flexible data (conceptual: needs deserialization from flexible_zone())
                // In a real scenario, flexible_zone() would contain MessagePack data.
                // const char* flex_zone_ptr = consumer->flexible_zone();
                // nlohmann::json flex_data = nlohmann::json::from_msgpack(flex_zone_ptr, ...);
                // LOGGER_INFO("Consumer: Flexible data sequence: {}", flex_data["sequence"].get<int>());

                if (!consumer->end_consume()) {
                    LOGGER_ERROR("Consumer: Failed to release consumed slot.");
                }
            } else {
                LOGGER_WARN("Consumer: No data available for consumption after 500ms.");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        LOGGER_INFO("Consumer: Finished reading data from DataBlock '{}'.", name);

    } catch (const std::exception& e) {
        LOGGER_ERROR("Consumer: An error occurred: {}", e.what());
    }
}

// int main() {
//     // Initialize logging and other framework components as needed
//     // For demonstration, assume LOGGER_INFO, LOGGER_ERROR are functional.
//
//     const std::string db_name = "MyTestDataBlock";
//     const uint64_t db_secret = 0x123456789ABCDEF0;
//
//     std::thread prod_thread(producer_example, db_name, db_secret);
//     std::this_thread::sleep_for(std::chrono::milliseconds(1000)); // Give producer time to create
//     std::thread cons_thread(consumer_example, db_name, db_secret);
//
//     prod_thread.join();
//     cons_thread.join();
//
//     // DataBlock shared memory is automatically removed when the last DataBlock
//     // instance that created it is destroyed.
//
//     return 0;
// }
```

### 5. Future Work

-   **Broker Implementation**: Enhance the broker logic to handle consumer heartbeats and failure broadcasts. This is critical for robust consumer management and resource release.
-   **Full Policy-Based Buffer Management**: Implement the `begin_write`, `end_write`, `begin_consume`, and `end_consume` methods within the `IDataBlockProducer` and `IDataBlockConsumer` concrete implementations (e.g., for `SingleBuffer`, `DoubleBuffer`, `RingBuffer` policies). This includes managing the `write_index`, `commit_index`, `read_index`, `current_slot_id` atomics and the process-shared mutex.
-   **Flexible Data Zone Serialization**: Integrate MessagePack serialization/deserialization for the `FlexibleDataZone` within the producer and consumer implementations.
-   **API Implementation**: Implement the full policy-based C++ API, ensuring all security and coordination rules are enforced.
-   **Testing**: Develop comprehensive unit and integration tests for all DataBlock policies and scenarios.

## Copyright

This document is placed in the public domain or under the CC0-1.0-Universal license, whichever is more permissive.
