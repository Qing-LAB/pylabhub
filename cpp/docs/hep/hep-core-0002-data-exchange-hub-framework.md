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

This Hub Enhancement Proposal (HEP) outlines a revised design for the **Data Exchange Hub**, a core framework within the `pylabhub-utils` library. The new design is centered around a **Message Hub** that provides a unified event model for both system control and high-performance data exchange. It splits the framework into two distinct but cooperative modules: the Message Hub for control, discovery, and notifications, and a **DataBlock Hub** for passive, high-performance shared memory data buffers. This architecture simplifies client logic, enhances scalability, and unifies all inter-process communication under a single, consistent event-driven paradigm.

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

2.  **The DataBlock Hub**: A module providing high-performance, shared-memory-based channels for bulk data transfer. These channels are passive buffers, with all signaling handled by the Message Hub.

### 1. High-Performance Channel (DataBlock Hub)

-   **Purpose**: Designed for high-throughput, low-latency communication of large, structured data blocks.
-   **Technology**: Implemented using platform-native shared memory.
-   **Characteristics**:
    -   Provides zero-copy data access between processes on the same machine.
    -   Suitable for continuous, high-bandwidth data streams.
    -   All signaling is decoupled and managed by the Message Hub's Notification Channels.

#### 1.1. Shared Memory Layout

Each shared memory segment will be composed of a `SharedMemoryHeader` followed immediately by the raw data buffer.

The `SharedMemoryHeader` will contain:
-   **Control Block**: A minimal set of primitives for write-side integrity.
    -   **Process-Shared Mutex**: A mutex used exclusively by the producer to ensure atomic updates to the shared memory block. Consumers will not use this lock.
    -   **Atomic Flags**: A set of `std::atomic` integers or flags for lock-free state checking (e.g., `is_writing`, `frame_id`). A consumer can use these to quickly inspect state, but the primary mechanism for knowing when to read is the Message Hub's Notification Channel.
-   **Static Metadata Block**: A fixed-size `struct` containing performance-critical information needed to interpret the raw data buffer (e.g., `frame_id`, `timestamp`, `data_size`, `dimensions`).
-   **Dynamic Metadata Region**: An optional, fixed-size region for storing non-performance-critical metadata, preferably using **MessagePack**.

### 2. General-Purpose Channel (Message Hub)

-   **Purpose**: The Message Hub forms the control plane of the entire framework, handling commands, state, events, and data notifications.
-   **Technology**: Implemented using **ZeroMQ**.
-   **Serialization**: **MessagePack** is the primary serialization format. **JSON** is a supported alternative for human-readable messages.

#### 2.1. Data Notification Channels

To fully unify the event model, the system will not use embedded signaling primitives within shared memory. Instead, all data-ready notifications will be dispatched through the Message Hub.

-   **Pattern**: For each `DataBlock` channel created, the Message Hub establishes a corresponding, private ZeroMQ `PUB/SUB` topic. This is the **Notification Channel**.
-   **Producer Workflow**: After a producer writes a frame to a `DataBlock` and releases its lock, it publishes a small **notification message** to the associated Notification Channel. This message, serialized with MessagePack, contains metadata like the `frame_id` and `timestamp`.
-   **Consumer Workflow**: A consumer subscribes to the Notification Channel. It uses a standard `zmq_poll` loop to wait for notifications. When a message arrives, the consumer knows a new frame is ready to be read from the shared memory buffer.

**Benefits of this Approach:**
-   **Unified Event Loop**: Consumers can monitor control messages and data notifications within a single, simple `zmq_poll` loop, drastically simplifying client architecture.
-   **Enhanced Decoupling**: The DataBlock Hub becomes a truly passive data store, with all eventing logic consolidated within the Message Hub.
-   **Network Transparency**: Provides a seamless path for network extension. Remote clients can subscribe to notifications and receive data from a local proxy service.

### 3. Service Broker, Discovery, and Security (Message Hub Core)

The **Message Hub** will incorporate a dedicated Service Broker model built on ZeroMQ for robust and secure service management. This broker is responsible for registering and discovering all channels, including DataBlocks and their associated Notification Channels. All broker communication will be secured using **CurveZMQ**.

### 4. C++ API Design Principles

The C++ API will be designed around the `MessageHub` as the central, static entry point. Consumers of `DataBlock`s will receive data-ready signals via a ZeroMQ socket.

-   **`MessageHub` Class**: A `Lifecycle`-managed static class that serves as the factory for all channel types.
-   **Channel Classes**:
    -   `DataBlockProducer`: Its API remains `begin_publish`/`end_publish`, but `end_publish` now publishes a ZMQ notification.
    -   `DataBlockConsumer`: The API is redesigned to expose the notification mechanism. It provides access to the notification socket rather than a blocking `consume()` method.

```cpp
// A conceptual sketch of the notification-driven API
namespace pylabhub::hub {

class MessageHub { /* ... as before: initialize, shutdown, and channel factories ... */ };

class DataBlockConsumer {
public:
    // Non-blocking access to the data buffer. It is only safe
    // to call this after receiving a notification.
    const void* get_data() const;
    const SharedMemoryHeader* header() const;

    // Returns the ZMQ SUB socket for the Notification Channel.
    // The client can add this socket to a zmq::poll loop
    // to wait for data-ready events.
    void* notification_socket() const;

    // Helper method to parse a notification message received
    // on the notification_socket().
    static std::optional<Notification> parse_notification(const zmq::message_t& msg);
};

// Example Usage:
// auto consumer = MessageHub::find_datablock_consumer("my_data");
// zmq::pollitem_t items[] = { { *consumer->notification_socket(), 0, ZMQ_POLLIN, 0 } };
// while (true) {
//     zmq::poll(items, 1, -1);
//     if (items[0].revents & ZMQ_POLLIN) {
//         // Notification received, now it's safe to read from shared memory.
//         const SharedMemoryHeader* header = consumer->header();
//         std::cout << "New frame available: " << header->frame_id << std::endl;
//     }
// }

} // namespace pylabhub::hub
```

### 5. Use Case: Real-time Experiment Control

-   **Data Streaming**: An acquisition process uses a `DataBlockProducer` to publish a camera feed. After each frame, it sends a notification via the Message Hub. A GUI, subscribed to the notification channel, receives the message and then uses a `DataBlockConsumer` to read and display the frame from shared memory. All other control and state exchange happens over separate Message Hub channels.

### 6. Future Work

-   **Refactor `SharedMemoryHub.cpp`**: Refactor the existing implementation into the two-module design: the `Lifecycle`-managed **Message Hub** and the passive **DataBlock Hub**.
-   **API Implementation**: Implement the full notification-driven C++ API.
-   **Serialization**: Implement transparent support for both MessagePack and JSON.

## Copyright

This document is placed in the public domain or under the CC0-1.0-Universal license, whichever is more permissive.