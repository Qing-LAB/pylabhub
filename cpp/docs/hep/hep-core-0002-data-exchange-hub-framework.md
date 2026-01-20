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

This Hub Enhancement Proposal (HEP) outlines a definitive design for the **Data Exchange Hub**. The architecture is centered around a **Message Hub** that provides a unified event model for system control and discovery. This hub coordinates access to high-performance **DataBlocks**. Each DataBlock is a shared memory channel featuring a stateful **Flexible Data Zone** for MessagePack/JSON objects. Crucially, the behavior of its bulk **Structured Data Buffer** is determined by a user-selectable **Policy**, such as a single-buffer, double-buffer, or a lossless ring-buffer queue. This policy-based design provides a highly flexible, backpressure-aware, and extensible framework for a wide range of data-flow applications.

## Motivation

Modern scientific and laboratory environments often require integrating multiple, independent software tools and hardware instruments into a cohesive system. A centralized and standardized data exchange mechanism is needed to:
-   Decouple data producers from consumers in diverse data-flow pipelines.
-   Provide a unified interface for system control and state monitoring.
-   Enable both low-latency "latest value" streaming and reliable, lossless data queuing.
-   Ensure data integrity and comprehensive logging of experimental parameters and states.

The Data Exchange Hub aims to provide this foundational IPC layer, simplifying the development of complex, multi-process experimental control and data acquisition systems.

## Specification

The framework consists of two primary modules unified under a common discovery system:

1.  **The Message Hub**: A statically managed `Lifecycle` module built on ZeroMQ. It provides the core infrastructure for service discovery, control messaging, and data notifications.

2.  **The DataBlock Hub**: A module providing high-performance, shared-memory `DataBlock` channels. The behavior of each channel is determined by a specific buffer management policy.

### 1. High-Performance Channel (DataBlock Hub)

-   **Purpose**: To provide versatile shared memory channels that can be optimized for different data exchange patterns.
-   **Policy-Based Buffer Management**: The core innovation is that the `StructuredDataBuffer` within a `DataBlock` is managed by a specific, user-chosen policy. This allows the same underlying technology to serve multiple use cases.
-   **Standard Policies**:
    1.  **`SingleBufferPolicy`**: The buffer is a single slot. New data overwrites old data. Use case: "latest value" display for low-frequency sensors, where intermediate values are unimportant.
    2.  **`DoubleBufferPolicy`**: Provides two slots, a "front" and "back" buffer, which are swapped. A producer always writes to the back buffer, while a consumer reads from the front. This prevents tearing and guarantees a stable read. Use case: high-frequency real-time visualization.
    3.  **`RingBufferPolicy`**: The buffer is treated as a multi-slot queue (`N` slots). This provides backpressure (a producer must wait if the queue is full) and guarantees lossless data transfer. Use case: scientific data processing pipelines.
-   **Stateful DataBlocks**: Every `DataBlock`, regardless of policy, also contains a **Flexible Data Zone** to hold a MessagePack/JSON object. This allows each block of data to be associated with a rich, variable "state".

#### 1.1. Shared Memory Layout

A `DataBlock` segment contains a header followed by the data region.
1.  **`SharedMemoryHeader`**: Contains control primitives and metadata applicable to all policies (e.g., mutex, atomic flags for state, and policy-specific data like ring-buffer indices).
2.  **`FlexibleDataZone`**: The buffer for the state object.
3.  **`StructuredDataBuffer`**: The region for bulk data. This region's internal structure is interpreted by the chosen policy (e.g., as one block, two blocks, or `N` slots).

### 2. General-Purpose Channel (Message Hub)

The Message Hub provides the control and eventing plane for the entire system, built on **ZeroMQ** and using **MessagePack** as the primary serialization format.

#### 2.1. Data Notification Channels

All data availability is signaled via ZeroMQ `PUB/SUB` **Notification Channels**, ensuring a unified event model. The meaning of a notification depends on the `DataBlock` policy:
-   For `SingleBuffer` and `DoubleBuffer`, it means "a new frame is ready".
-   For `RingBuffer`, it means "slot `i` is now ready for consumption".

### 3. C++ API Design Principles

The API will be policy-driven. The `MessageHub` factory will create a `DataBlock` with a specific policy, and the returned producer/consumer objects will have an API tailored to that policy.

```cpp
// A conceptual sketch of the policy-based API
namespace pylabhub::hub {

enum class DataBlockPolicy { Single, DoubleBuffer, RingBuffer };

struct DataBlockConfig {
    size_t structured_buffer_size;
    size_t flexible_zone_size;
    int ring_buffer_capacity; // Only used by RingBufferPolicy
    // ... other config ...
};

class MessageHub {
public:
    // Factory method now takes a policy and config.
    static std::unique_ptr<IDataBlockProducer> create_datablock_producer(
        const std::string& name,
        DataBlockPolicy policy,
        const DataBlockConfig& config
    );
    // Returns a consumer appropriate for the block's policy.
    static std::unique_ptr<IDataBlockConsumer> find_datablock_consumer(
        const std::string& name
    );
    // ...
};

// Base interfaces
class IDataBlockProducer { /* ... */ };
class IDataBlockConsumer {
public:
    virtual void* notification_socket() const = 0;
};

// Example of a policy-specific interface
class IRingBufferConsumer : public IDataBlockConsumer {
public:
    virtual ReadSlot* begin_consume(uint64_t slot_index) = 0;
    virtual void end_consume(ReadSlot* slot) = 0;
};

} // namespace pylabhub::hub
```

### 4. Future Work

-   **Refactor `SharedMemoryHub.cpp`**: Refactor the implementation to create the base `DataBlock` infrastructure and the initial set of buffer management policies (`SingleBuffer`, `DoubleBuffer`, `RingBuffer`).
-   **API Implementation**: Implement the policy-based factory and the specific C++ producer/consumer classes for each policy.

## Copyright

This document is placed in the public domain or under the CC0-1.0-Universal license, whichever is more permissive.