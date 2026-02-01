# Use Case for HEP-core-0002: Ring Buffer Data Exchange

This document provides a detailed use case for the framework described in HEP-core-0002, focusing on the message flow for a `RingBuffer` `DataBlock` implementation.

## 1. Scenario Overview

A data acquisition process (**Producer**) continuously writes microscope image data into a shared memory `DataBlock` configured with a `RingBuffer` policy. A separate image processing application (**Consumer**) reads from this buffer for near real-time analysis. A central **Broker** (part of the `MessageHub`) coordinates service discovery, notifications, and client lifecycle.

**Actors:**
-   **Producer**: The application that creates the `DataBlock` and writes data to it.
-   **Consumer**: The application that connects to the `DataBlock` and reads data from it.
-   **Broker**: The central server process that manages registrations, heartbeats, and notifications.

## 2. Message and Protocol Details

All communication with the Broker uses the strict two-part message format over a ZeroMQ `ROUTER` socket.

-   **Frame 1: Header**: A 16-byte null-padded "magic string" (e.g., `"PYLABHUB_REG_REQ\0"`).
-   **Frame 2: Payload**: A MessagePack-serialized object.

## 3. Detailed Interaction Flow

### Step 1: System Initialization

1.  The **Broker** process is started, typically managed by the `pylabhub::utils::Lifecycle` system.
2.  It binds a `ROUTER` socket on a known port (e.g., `tcp://*:5555`) to listen for client requests.
3.  It binds a `PUB` socket on a known port (e.g., `tcp://*:5556`) to broadcast data and management notifications.

### Step 2: Producer Registration

1.  The **Producer** application starts. It intends to create a `DataBlock` named `"MICROSCOPE_DATA"`.
2.  The `MessageHub` instance within the producer connects to the Broker's `ROUTER` socket (`tcp://<broker_host>:5555`).
3.  It sends a **Registration Request** message:
    -   **Frame 1**: `"PYLABHUB_REG_REQ"`
    -   **Frame 2 (MessagePack)**:
        ```json
        {
          "channel_name": "MICROSCOPE_DATA",
          "channel_type": "DataBlock",
          "policy": "RingBuffer",
          "shm_name": "/plh_shm_microscope",
          "shm_size": 16777216, // 16 MB
          "shared_secret": 1234567890123456, // A 64-bit secret
          "notification_topic": "DB_NOTIFY.MICROSCOPE_DATA"
        }
        ```
4.  The **Broker** receives the request. It validates the message, stores the channel information in its registry, and sends a simple **OK Reply**:
    -   **Frame 1**: `"PYLABHUB_REG_ACK"`
    -   **Frame 2 (MessagePack)**: `{"status": "OK"}`

### Step 3: Consumer Discovery and Connection

1.  The **Consumer** application starts and needs to access the `"MICROSCOPE_DATA"` channel.
2.  Its `MessageHub` instance connects to the Broker's `ROUTER` socket.
3.  It sends a **Discovery Request** message:
    -   **Frame 1**: `"PYLABHUB_DISC_REQ"`
    -   **Frame 2 (MessagePack)**: `{"channel_name": "MICROSCOPE_DATA"}`
4.  The **Broker** looks up the channel and sends a **Discovery Reply** with the connection details:
    -   **Frame 1**: `"PYLABHUB_DISC_ACK"`
    -   **Frame 2 (MessagePack)**: The same metadata the producer registered.
5.  The **Consumer** receives the reply, uses the `shm_name` and `shared_secret` to map the `DataBlock` into its address space.
6.  The Consumer's `MessageHub` subscribes to the Broker's `PUB` socket (`tcp://<broker_host>:5556`), setting a topic filter for `"DB_NOTIFY.MICROSCOPE_DATA"`.
7.  The Consumer immediately sends its first **Heartbeat** and begins sending them periodically.
    -   **Frame 1**: `"PYLABHUB_HB_REQ"`
    -   **Frame 2 (MessagePack)**: `{"consumer_id": "<unique_id>", "channel_name": "MICROSCOPE_DATA"}`

### Step 4: Data Production and Notification

1.  The **Producer** acquires a write slot (e.g., index `5`) in the ring buffer, writes the image data, and commits the write.
2.  It then sends a **Data Notification** message to the Broker's `ROUTER` socket.
    -   **Frame 1**: `"PYLABHUB_DB_NOTIFY"`
    -   **Frame 2 (MessagePack)**:
        ```json
        {
          "channel_name": "MICROSCOPE_DATA",
          "slot_index": 5,
          "timestamp": 1673024400.123
        }
        ```
3.  The **Broker** receives the notification. It forwards the payload by publishing a two-part message on its `PUB` socket:
    -   **Part 1 (Topic)**: `"DB_NOTIFY.MICROSCODE_DATA"`
    -   **Part 2 (Payload)**: The original MessagePack payload from the producer.

### Step 5: Data Consumption

1.  The **Consumer**'s `SUB` socket receives the message.
2.  It deserializes the MessagePack payload to get the `slot_index` (5).
3.  It calls `begin_consume(5)` on its `DataBlockConsumer` interface, which gives it read access to the data in that slot.
4.  After processing the image, it calls `end_consume()` to release the slot, allowing the producer to reuse it.

### Step 6: Heartbeat and Failure Detection

1.  The **Consumer** continues to send periodic `PYLABHUB_HB_REQ` messages to the Broker.
2.  Later, the Consumer process crashes and stops sending heartbeats.
3.  The **Broker** detects the missing heartbeats after a configured timeout (e.g., 5 seconds).
4.  The Broker marks the consumer as "dead" and broadcasts a **Consumer Drop Notification** on its `PUB` socket:
    -   **Part 1 (Topic)**: `"MGMT.CONS_DROP"`
    -   **Part 2 (Payload)**:
        ```json
        {
          "channel_name": "MICROSCOPE_DATA",
          "consumer_id": "<unique_id_of_dead_consumer>"
        }
        ```
5.  The **Producer**, being subscribed to management topics, receives this notification. It can now inspect the `DataBlock` state and reclaim any ring buffer slots that were locked by the dead consumer, preventing a permanent stall.
