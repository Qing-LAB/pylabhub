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

This Hub Enhancement Proposal (HEP) outlines a revised design for the **Data Exchange Hub**. The architecture is centered around a **Message Hub** that provides a unified event model for system control and discovery. This hub coordinates access to passive, high-performance **DataBlocks**. Each DataBlock is a shared memory segment featuring a dual-zone layout: a **Structured Data Buffer** for high-throughput, numpy-friendly data arrays, and a **Flexible Data Zone** for variable-sized, MessagePack/JSON formatted information. This design provides a comprehensive solution for both high-performance data streaming and convenient, structured state exchange, all managed under a single, consistent event-driven paradigm.

## Motivation

Modern scientific and laboratory environments often require integrating multiple, independent software tools and hardware instruments into a cohesive system. A centralized and standardized data exchange mechanism is needed to:
-   Decouple data producers (e.g., acquisition instruments) from consumers (e.g., real-time analysis, data loggers, user interfaces).
-   Provide a unified interface for system control and state monitoring.
-   Enable real-time data streaming and analysis without compromising performance.
-   Ensure data integrity and comprehensive logging of experimental parameters and states.

The Data Exchange Hub aims to provide this foundational IPC layer, simplifying the development of complex, multi-process experimental control and data acquisition systems.

## Specification

The Data Exchange Hub framework is composed of two primary modules, each providing a distinct communication channel, but unified under a common discovery and management system.

1.  **The Message Hub**: A statically managed `Lifecycle` module that provides the core infrastructure for messaging, service discovery, control, and data notifications. It is built on ZeroMQ and serves as the central broker.

2.  **The DataBlock Hub**: A module providing high-performance, shared-memory channels (`DataBlocks`). These channels are passive buffers, with all signaling handled by the Message Hub.

### 1. High-Performance Channel (DataBlock Hub)

-   **Purpose**: To provide shared memory channels (`DataBlocks`) that accommodate both high-throughput, fixed-format data and variable, self-describing data structures.
-   **Dual-Zone Layout**: Each `DataBlock` is composed of two distinct data areas:
    1.  **Structured Data Buffer**: A large, contiguous memory block for high-performance streaming of numpy-friendly data types (e.g., image frames, waveforms).
    2.  **Flexible Data Zone**: A memory area designed to hold a single, variable-size MessagePack or JSON object, ideal for complex configuration, parameters, or state that doesn't fit a rigid `struct`.
-   **Coordination**: All access and updates are coordinated via the **Message Hub**, which provides discovery, write-side integrity (mutex), and data-ready notifications.

#### 1.1. Shared Memory Layout

A `DataBlock` shared memory segment is partitioned into three contiguous regions:
1.  **`SharedMemoryHeader`**: Contains control primitives and metadata for both data zones.
2.  **`FlexibleDataZone`**: The buffer for MessagePack/JSON data. Its capacity is defined at creation.
3.  **`StructuredDataBuffer`**: The buffer for bulk binary data. Its capacity is also defined at creation.

The `SharedMemoryHeader` will contain:
-   **Control Block**:
    -   **Process-Shared Mutex**: A mutex for ensuring atomic write operations across both zones. Producers must lock this before writing to either zone.
    -   **Atomic Flags**: `is_writing`, etc., for lock-free state inspection by consumers.
-   **Zone Metadata**:
    -   `uint64_t flex_zone_capacity`: The total allocated size of the `FlexibleDataZone`.
    -   `uint64_t flex_zone_size`: The size of the valid data currently in the `FlexibleDataZone`.
    -   `uint64_t flex_zone_version`: A version counter for the `FlexibleDataZone`, incremented on each update.
    -   `uint64_t structured_buffer_capacity`: The total allocated size of the `StructuredDataBuffer`.
    -   `uint64_t structured_buffer_size`: The size of the valid data in the `StructuredDataBuffer`.
    -   `uint64_t structured_buffer_version`: A version counter for the `StructuredDataBuffer`, incremented on each update.
    -   `double timestamp`: A high-resolution timestamp of the last update.

### 2. General-Purpose Channel (Message Hub)

-   **Purpose**: The Message Hub forms the control plane of the entire framework, handling commands, state, events, and data notifications.
-   **Technology**: Implemented using **ZeroMQ**.
-   **Serialization**: **MessagePack** is the primary serialization format. **JSON** is a supported alternative.

#### 2.1. Data Notification Channels

To fully unify the event model, the system will not use embedded signaling primitives within shared memory. Instead, all data-ready notifications will be dispatched through the Message Hub.

-   **Pattern**: For each `DataBlock`, the Message Hub establishes a corresponding ZeroMQ `PUB/SUB` topic, the **Notification Channel**.
-   **Producer Workflow**: After a producer locks the mutex, writes to one or both data zones, and updates the header, it unlocks the mutex and then publishes a **notification message** to the Notification Channel. This message will indicate which zone(s) were updated.
-   **Consumer Workflow**: A consumer subscribes to the Notification Channel. It uses a `zmq_poll` loop to wait for notifications. When a message arrives, the consumer knows new data is ready and can safely read from the corresponding zone(s) in the shared memory buffer.

### 3. Service Broker, Discovery, and Security (Message Hub Core)

The **Message Hub** will incorporate a dedicated Service Broker model built on ZeroMQ for robust and secure service management. This broker is responsible for registering and discovering all channels, including DataBlocks and their associated Notification Channels. All broker communication will be secured using **CurveZMQ**.

### 4. C++ API Design Principles

The C++ API will be designed around the `MessageHub` as the central, static entry point.

-   **`MessageHub` Class**: A `Lifecycle`-managed static class that serves as the factory for all channel types.
-   **Channel Classes**:
    -   `DataBlockProducer`: Its API will provide separate access to the flexible and structured zones. `end_publish` will trigger the ZMQ notification.
    -   `DataBlockConsumer`: Exposes the notification socket for use in a poll loop, and provides non-blocking access to the data zones.

```cpp
// A conceptual sketch of the revised API
namespace pylabhub::hub {

class MessageHub {
public:
    // Managed by the Lifecycle system.
    static bool initialize(const BrokerConfig& config);
    static void shutdown();

    // Factory for creating a DataBlock with two distinct data zones.
    static std::unique_ptr<DataBlockProducer> create_datablock_producer(
        const std::string& name,
        size_t structured_buffer_size,
        size_t flexible_zone_size
    );
    static std::unique_ptr<DataBlockConsumer> find_datablock_consumer(const std::string& name);
    
    // ... other ZMQ channel factories ...
};

class DataBlockProducer {
public:
    // Lock the block for writing
    void begin_publish();

    // Get pointers to the data zones (valid after begin_publish)
    void* get_flexible_zone_buffer();
    void* get_structured_data_buffer();

    // Unlock the block and send notification
    void end_publish(const ZoneUpdateInfo& update_info);
};

class DataBlockConsumer {
public:
    // Non-blocking access to data zones. Safe to call after a notification.
    const void* get_flexible_zone_data() const;
    const void* get_structured_data() const;
    const SharedMemoryHeader* header() const;

    // Returns the ZMQ SUB socket for the Notification Channel.
    void* notification_socket() const;
};

} // namespace pylabhub::hub
```

### 5. Use Case: Real-time Experiment Control

-   **Data Streaming**: An acquisition process uses a `DataBlockProducer` to publish a camera feed into the `StructuredDataBuffer`.
-   **Parameter Update**: Concurrently, it can write a MessagePack object with the latest camera settings (exposure, gain) into the `FlexibleDataZone`.
-   **Notification**: It calls `end_publish` once, which sends a single notification that both zones were updated.
-   **Consumption**: A GUI client receives the notification, reads the settings from the flexible zone to update its display, and reads the image from the structured buffer to render it.

### 6. Future Work

-   **Refactor `SharedMemoryHub.cpp`**: Refactor the existing implementation into the two-module design: the `Lifecycle`-managed **Message Hub** and the passive **DataBlock Hub** with its dual-zone layout.
-   **API Implementation**: Implement the full notification-driven C++ API.

## Copyright

This document is placed in the public domain or under the CC0-1.0-Universal license, whichever is more permissive.
