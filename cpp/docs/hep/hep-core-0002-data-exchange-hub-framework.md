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

This Hub Enhancement Proposal (HEP) outlines a design framework for the **Data Exchange Hub**, a core module within the `pylabhub-utils` library. This module will provide robust mechanisms for inter-process communication (IPC), enabling disparate applications and components to share data and commands efficiently. The framework proposes a dual-strategy approach, utilizing both high-performance shared memory and flexible message-based communication to meet diverse technical requirements.

## Motivation

Modern scientific and laboratory environments often require integrating multiple, independent software tools and hardware instruments into a cohesive system. A centralized and standardized data exchange mechanism is needed to:
-   Decouple data producers (e.g., acquisition instruments) from consumers (e.g., real-time analysis, data loggers, user interfaces).
-   Provide a unified interface for system control and state monitoring.
-   Enable real-time data streaming and analysis without compromising performance.
-   Ensure data integrity and comprehensive logging of experimental parameters and states.

The Data Exchange Hub aims to provide this foundational IPC layer, simplifying the development of complex, multi-process experimental control and data acquisition systems.

## Specification

The Data Exchange Hub will provide two primary mechanisms for communication, exposed through a unified C++ API.

### 1. High-Performance Channel (Shared Memory)

-   **Purpose**: Designed for high-throughput, low-latency communication of large, structured data blocks (e.g., image frames, ADC waveforms, spectral data).
-   **Technology**: To be implemented using a custom C++ wrapper that abstracts platform-native shared memory (e.g., `CreateFileMapping` on Windows, `shm_open` on POSIX) to avoid external library dependencies like Boost.Interprocess.
-   **Characteristics**:
    -   Provides zero-copy data access between processes on the same machine.
    -   Suitable for continuous, high-bandwidth data streams.
    -   Requires careful synchronization (e.g., via mutexes or semaphores stored in the shared memory segment) to prevent race conditions.

#### 1.1. Shared Memory Layout

To standardize usage and optimize performance, each shared memory segment will be composed of a `SharedMemoryHeader` followed immediately by the raw data buffer. This layout provides fast access to critical metadata and synchronization primitives.

The `SharedMemoryHeader` will contain:
-   **Control Block**: Residing at the beginning of the segment, this block contains the necessary synchronization and state-management primitives, managed transparently by the C++ API wrapper.
    -   **Process-Shared Mutex**: A mutex lockable by any process sharing the memory segment. It will be implemented using platform-native APIs (e.g., a `pthread_mutex_t` with the `PTHREAD_PROCESS_SHARED` attribute on POSIX, or a named `Mutex` object on Windows).
    -   **Process-Shared Notification/Signaling**: A condition variable-like mechanism for processes to wait for signals (e.g., `data_ready`). It will be implemented using platform-native primitives (e.g., a `pthread_cond_t` with `PTHREAD_PROCESS_SHARED` on POSIX, or a named `Event`/`Semaphore` on Windows).
    -   **Atomic Flags**: A set of `std::atomic` integers or flags for lock-free state signaling (e.g., `is_writing`, `frame_id`), providing a lightweight way to check status without locking the mutex.
-   **Static Metadata Block**: A fixed-size `struct` containing performance-critical information needed to interpret the raw data buffer. This enables consumers to access metadata with zero parsing overhead. It will include fields such as:
    -   `uint64_t frame_id`: A monotonically increasing counter for the data frame.
    -   `double timestamp`: A high-resolution timestamp of data acquisition.
    -   `uint64_t data_size`: The exact size of the valid data in the buffer.
    -   `uint32_t data_type_hash`: A hash identifying the data type (e.g., `fnv1a('uint8_image')`).
    -   `uint64_t dimensions[4]`: An array for the dimensions of the data (e.g., width, height, depth).
-   **Dynamic Metadata Region**: An optional, fixed-size character array (e.g., 1-2 KB) reserved for storing non-performance-critical metadata in a structured format like length-prefixed JSON.

The total size of the shared memory segment will be aligned to page-size boundaries to ensure efficient memory management.

### 2. General-Purpose Channel (Messaging)

-   **Purpose**: Designed for asynchronous, message-based communication. Ideal for sending commands, exchanging state information, event notifications, and transferring smaller data payloads.
-   **Technology**: To be implemented using the **ZeroMQ** library, which provides robust, high-performance messaging patterns.
-   **Characteristics**:
    -   Supports various messaging patterns (Request-Reply, Publish-Subscribe, Push-Pull) to suit different use cases.
    -   Enables communication between processes on the same machine or across a network.
    -   Manages message queuing, delivery, and connection handling automatically.

### 3. Service Broker, Discovery, and Security

To provide a robust, secure, and centralized mechanism for service registration, discovery, and monitoring, the framework will adopt a dedicated Service Broker model built on ZeroMQ.

-   **Broker Architecture**: A central "Service Broker" process will act as the authority for the entire Data Exchange Hub. It will listen on a well-known TCP port using a ZeroMQ `ROUTER` socket.
-   **Request/Approval Flow**: 
    -   A producer process that wishes to offer a channel will connect to the Broker and send a "register" request.
    -   The Broker will validate the request and, upon approval, add the channel to its internal, in-memory registry.
    -   Consumer processes will query the Broker to discover available channels and receive the necessary connection details.
-   **Watchdog**: The Broker will also serve as the primary watchdog. Registered services must send periodic "heartbeat" messages to the Broker. If a heartbeat is not received within a configured timeout, the Broker will automatically de-register the service, ensuring the system view is always current.

#### 3.1. Broker Security using CurveZMQ

All communication with the Service Broker must be encrypted and authenticated. This will be implemented using the **CurveZMQ** protocol, a standard feature of the ZeroMQ library.

-   **Key Management**: The Broker will maintain a long-term public/private server key pair. All clients (producers and consumers) must know the Broker's public key to connect.
-   **Authentication and Encryption**: CurveZMQ will be used to establish a secure session for all clients. This prevents eavesdropping and ensures that only authorized applications can participate in the Data Exchange Hub.

#### 3.2. Broker Key Management

To ensure security across restarts, the Broker's CurveZMQ key pair must be managed persistently and securely.

-   **Storage**: The Broker's private key will be stored in a file on disk, encrypted using the authenticated encryption facilities of `libsodium` (a ZeroMQ dependency). The public key will be stored in plaintext.
-   **Decryption**: The password to decrypt the private key will be provided to the Broker on startup via a command-line argument, a configuration file, or an environment variable (e.g., `PYLABHUB_BROKER_SECRET`). This prevents the secret from being stored in plaintext.
-   **Key Generation**: A helper utility (`pylabhub-broker-keygen`) will be provided to generate a new key pair and encrypt the private key. To simplify initial setup, the Broker will, on first run, detect the absence of a key file, automatically generate a new one, and prompt for a password if running in an interactive terminal.
-   **Public Key Distribution**: The Broker will log its public key to the console on startup, allowing administrators to easily retrieve and distribute it to client applications.

### 4. C++ API Design Principles

The C++ API will be designed to be abstract, intuitive, and safe, hiding the underlying complexity of IPC and broker communication. The design will follow a factory pattern centered around a main `Hub` class.

-   **`Hub` Class**: The primary entry point for applications. An instance of this class will manage the connection to the Service Broker, handle authentication and heartbeats, and act as a factory for creating communication channels.
-   **Channel Classes**: The `Hub` class will provide methods to create or discover channels, returning RAII-style objects that manage the channel's lifecycle.
    -   `SharedMemoryProducer`/`SharedMemoryConsumer`: For the High-Performance Channel. These classes will provide simple methods like `begin_publish()`, `end_publish()`, and `consume()` that transparently handle all underlying synchronization and memory management.
    -   `ZmqPublisher`/`ZmqSubscriber`: For one-to-many message distribution.
    -   `ZmqRequestServer`/`ZmqRequestClient`: For request-reply style command and control.
-   **Transparency**: The API will completely abstract away broker interactions (registration, discovery), key management, and synchronization primitives. The application developer will only interact with high-level concepts like "channels," "messages," and "data buffers."

```cpp
// A conceptual sketch of the proposed API
namespace pylabhub::hub {

class Hub {
public:
    // Connects to broker, handles auth, and starts heartbeat thread.
    static std::unique_ptr<Hub> connect(const BrokerConfig& config);

    // High-Performance Channel
    std::unique_ptr<SharedMemoryProducer> create_shm_producer(const std::string& name, size_t size);
    std::unique_ptr<SharedMemoryConsumer> find_shm_consumer(const std::string& name);

    // General-Purpose Channel (Request-Reply example)
    std::unique_ptr<ZmqRequestServer> create_req_server(const std::string& service_name);
    std::unique_ptr<ZmqRequestClient> find_req_client(const std::string& service_name);

    // ... other ZMQ patterns
};

} // namespace pylabhub::hub
```

### 5. Use Case: Real-time Experiment Control

This framework directly addresses the scenario of real-time experiment integration:
-   **Service Registration**: An acquisition process starts and, via its `Hub` object, creates a `SharedMemoryProducer`, which automatically registers the channel with the central Service Broker.
-   **Service Discovery**: A GUI application uses its `Hub` object to find and create a `SharedMemoryConsumer`, which queries the Broker to get the connection details for the shared memory segment.
-   **Data Streaming**: The acquisition process uses the `SharedMemoryProducer` to publish the camera feed. The GUI uses the `SharedMemoryConsumer` to display it in real-time. The API handles all synchronization.
-   **Command and Control**: The GUI, through a `ZmqRequestClient`, sends commands (e.g., "change exposure time") to a `ZmqRequestServer` running in the acquisition process.
-   **State Logging**: A logger service subscribes to all registration and command messages on the Broker, recording a timestamped audit trail.

## 6. Future Work

With the core design principles established, future work will focus on implementation and refinement:

-   **Broker Implementation**: Implement the broker logic within the existing `hubshell` executable. This involves adding the ZeroMQ `ROUTER` loop, registry logic, and heartbeat mechanism to `hubshell.cpp`. Develop necessary key management utilities.
-   **C++ API Implementation**: Implement the full C++ API as outlined, ensuring it is cross-platform, robust, and well-documented. This includes the platform-specific wrappers for shared memory and synchronization primitives.
-   **Serialization**: While JSON is chosen for the General-Purpose Channel, and a hybrid static/dynamic model for the High-Performance Channel header, a lightweight binary format like MessagePack could be considered for the body of complex ZeroMQ messages if needed.
-   **Language Bindings**: Once the C++ core is stable, develop bindings for other languages, particularly Python, to maximize the Hub's utility in diverse environments.

## Copyright

This document is placed in the public domain or under the CC0-1.0-Universal license, whichever is more permissive.