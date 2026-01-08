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

    **Shared Memory Layout**

    To standardize usage for signal translation, rapid messaging, and data streaming, each shared memory segment will adhere to a structured layout. This approach avoids ambiguity and provides essential utilities directly within the shared memory block. The layout will consist of a header followed by the raw data buffer.

    The header will contain:
    -   **Control Block**: This block will reside at the beginning of the shared memory segment and contain the necessary synchronization and state-management primitives. The C++ wrapper class will be responsible for initializing and managing these primitives.
        -   **Process-Shared Mutex**: A mutex that can be locked and unlocked by different processes sharing the memory segment. This will be implemented using platform-native APIs (e.g., a `pthread_mutex_t` with the `PTHREAD_PROCESS_SHARED` attribute on POSIX, or a named `Mutex` object on Windows).
        -   **Process-Shared Notification/Signaling**: A mechanism to allow processes to wait for a signal or event from another process. This is analogous to a condition variable. It will be implemented using platform-native primitives (e.g., a `pthread_cond_t` with `PTHREAD_PROCESS_SHARED` on POSIX, or a named `Event` or `Semaphore` on Windows).
        -   **Atomic Flags**: A set of `std::atomic` integers for lock-free state signaling (e.g., `data_ready`, `data_read`), providing a lightweight way to check status without locking the mutex.
    -   **Metadata Region**: A fixed-size character array reserved for storing configuration or state information in a structured format, such as JSON.

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

### 4. Use Case: Real-time Experiment Control

This framework directly addresses the scenario of real-time experiment integration:
-   **Service Registration**: An acquisition process starts and registers its high-performance shared memory channel with the central Service Broker.
-   **Service Discovery**: A GUI application queries the Broker to find the camera's channel and gets the details to connect to the shared memory.
-   **Data Streaming**: The acquisition process publishes the camera feed to the shared memory channel. The GUI subscribes and displays it in real-time.
-   **Command and Control**: The GUI sends commands (e.g., "change exposure time") via the secure, brokered General-Purpose Channel (ZeroMQ).
-   **State Logging**: A logger service subscribes to all registration and command messages on the Broker, recording a timestamped audit trail.

## Open Questions & Future Work

-   Finalize the serialization format for the High-Performance Channel (e.g., MessagePack, FlatBuffers, or custom binary). JSON has been chosen for the General-Purpose Channel.
-   Develop a comprehensive C++ API that simplifies the use of these underlying technologies, including the new broker interaction model.
-   Design the persistent storage mechanism for the Broker's server keys.

## Copyright

This document is placed in the public domain or under the CC0-1.0-Universal license, whichever is more permissive.