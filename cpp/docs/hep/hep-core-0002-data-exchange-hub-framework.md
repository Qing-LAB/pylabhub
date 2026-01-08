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
-   **Technology**: To be implemented using platform-native shared memory (e.g., `CreateFileMapping` on Windows, `shm_open` on POSIX) or a cross-platform wrapper library like Boost.Interprocess.
-   **Characteristics**:
    -   Provides zero-copy data access between processes on the same machine.
    -   Suitable for continuous, high-bandwidth data streams.
    -   Requires careful synchronization (e.g., via mutexes or semaphores stored in the shared memory segment) to prevent race conditions.

### 2. General-Purpose Channel (Messaging)

-   **Purpose**: Designed for asynchronous, message-based communication. Ideal for sending commands, exchanging state information, event notifications, and transferring smaller data payloads.
-   **Technology**: To be implemented using the **ZeroMQ** library, which provides robust, high-performance messaging patterns.
-   **Characteristics**:
    -   Supports various messaging patterns (Request-Reply, Publish-Subscribe, Push-Pull) to suit different use cases.
    -   Enables communication between processes on the same machine or across a network.
    -   Manages message queuing, delivery, and connection handling automatically.

### Use Case: Real-time Experiment Control

This framework directly addresses the scenario of real-time experiment integration:
-   **Data Streaming**: An acquisition process can publish a high-frequency data stream (e.g., a camera feed) to a specific topic on the High-Performance Channel.
-   **Real-time Display**: A separate GUI application can subscribe to this shared memory channel to display the data in real-time with minimal latency.
-   **Command and Control**: The GUI can send user-initiated commands (e.g., "change exposure time", "start/stop acquisition") over a `Request-Reply` socket on the General-Purpose (ZeroMQ) Channel.
-   **State Logging**: A dedicated logger service can subscribe to both command messages and state-change events on the ZeroMQ channel, recording a timestamped audit trail of all experiment operations.

## Open Questions & Future Work

-   Define a precise serialization format for messages (e.g., JSON, MessagePack, FlatBuffers).
-   Design a discovery mechanism for channels and topics.
-   Finalize the choice of a cross-platform shared memory library or abstraction layer.
-   Develop a comprehensive C++ API that simplifies the use of these underlying technologies.

## Copyright

This document is placed in the public domain or under the CC0-1.0-Universal license, whichever is more permissive.