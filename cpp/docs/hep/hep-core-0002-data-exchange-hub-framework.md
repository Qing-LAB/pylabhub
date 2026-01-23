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

The API will be updated to reflect these robust, secure, and policy-driven concepts.

```cpp
// A conceptual sketch of the hardened, policy-based API
namespace pylabhub::hub {

enum class DataBlockPolicy { Single, DoubleBuffer, RingBuffer };

struct DataBlockConfig {
    uint64_t shared_secret;
    size_t structured_buffer_size;
    size_t flexible_zone_size;
    int ring_buffer_capacity; // Only for RingBufferPolicy
};

class MessageHub {
public:
    // Factory method takes a policy and secure config.
    static std::unique_ptr<IDataBlockProducer> create_datablock_producer(
        const std::string& name,
        DataBlockPolicy policy,
        const DataBlockConfig& config
    );
    // Consumer must know the secret to connect.
    static std::unique_ptr<IDataBlockConsumer> find_datablock_consumer(
        const std::string& name,
        uint64_t shared_secret
    );
};

// Consumers will be returned via policy-specific interfaces
class IRingBufferConsumer : public IDataBlockConsumer {
public:
    virtual ReadSlot* begin_consume(uint64_t slot_index) = 0;
    virtual void end_consume(ReadSlot* slot) = 0;
};

} // namespace pylabhub::hub
```

### 5. Future Work

-   **Refactor `SharedMemoryHub.cpp`**: Refactor the implementation to create the base `DataBlock` infrastructure, security features, and the initial set of buffer management policies.
-   **Broker Implementation**: Enhance the broker logic to handle consumer heartbeats and failure broadcasts.
-   **API Implementation**: Implement the full policy-based C++ API, ensuring all security and coordination rules are enforced.

## Copyright

This document is placed in the public domain or under the CC0-1.0-Universal license, whichever is more permissive.
