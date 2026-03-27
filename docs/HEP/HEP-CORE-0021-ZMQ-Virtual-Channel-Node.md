# HEP-CORE-0021: ZMQ Virtual Channel Node

| Property       | Value                                                                      |
|----------------|----------------------------------------------------------------------------|
| **HEP**        | `HEP-CORE-0021`                                                            |
| **Title**      | ZMQ Virtual Channel Node — Broker-Aware Peer-to-Peer ZMQ Endpoints        |
| **Status**     | Implemented — 2026-03-06                                                   |
| **Created**    | 2026-03-05                                                                 |
| **Area**       | Framework Architecture (`BrokerService`, `hub::Producer`, `hub::Consumer`) |
| **Depends on** | HEP-CORE-0002 (DataHub), HEP-CORE-0007 (Protocol), HEP-CORE-0017 (Pipeline) |

---

## 1. Motivation

The current framework has two tiers of data transport:

1. **SHM (DataBlock)**: Structured, schema-validated, ring-buffered shared memory.
   The broker is the service directory — producers register channels, consumers
   discover them via `CONSUMER_REG_REQ`. Fully broker-aware.

2. **ZMQ PUSH/PULL (direct)**: Peer-to-peer, bypasses the broker entirely.
   Endpoints are hardcoded in configuration files. The broker has no knowledge
   of these channels. Not discoverable.

The second tier creates problems:

- **No service directory**: A consumer cannot ask the broker "where is channel X?".
  It must know the endpoint in advance via out-of-band configuration.
- **Brittle configuration**: Both sides of the ZMQ pair must hardcode matching
  endpoints. A change in one requires manual updates to the other.
- **Asymmetric topology**: In a cross-hub bridge scenario, the processor on Hub B
  must have the ZMQ endpoint of the processor on Hub A hardcoded — even though
  the broker on Hub A already knows about that processor.
- **Silent failure**: If the ZMQ endpoint changes (port reuse, redeployment), no
  error is surfaced at the broker level. The consumer simply hangs.

**This HEP introduces the ZMQ Virtual Channel Node** — a lightweight channel
registration type that makes ZMQ peer-to-peer endpoints broker-aware and
discoverable, using the same REG_REQ / CONSUMER_REG_ACK protocol as SHM channels.

---

## 2. Design Principles

1. **Virtual, not structural**: A ZMQ node is a connection point, not a data
   container. Unlike a DataBlock (which has slots, schema, ring buffer, mutex,
   and a shared memory segment), a ZMQ node has only an endpoint string. There
   is no data structure to define, no memory to allocate, no schema required.

2. **Broker as directory, not relay**: The broker stores the ZMQ endpoint in the
   channel entry and echoes it back on consumer registration. It does not relay
   or touch the data stream. The actual data still flows peer-to-peer.

3. **Symmetric with SHM registration**: The producer side registers first
   (`REG_REQ` with `transport=zmq`). The consumer side discovers via
   `CONSUMER_REG_REQ` and gets back the endpoint. Same handshake, different
   payload. Existing channel lifecycle (heartbeat, CHANNEL_CLOSING_NOTIFY,
   FORCE_SHUTDOWN, metrics) applies unchanged.

4. **Transport is producer's declaration**: The producer declares `transport=zmq`
   at registration time. Consumers do not need to know or configure the transport
   type — the broker tells them. This is the same as SHM: the producer creates
   the DataBlock, consumers discover its name and secret.

5. **Schema is optional**: Since a ZMQ node carries raw bytes, schema validation
   is not enforced at the framework level. Schema fields in REG_REQ are optional
   for ZMQ nodes. Applications may document their wire format separately.

6. **Cross-hub discovery via `in_hub_dir`**: A processor on Hub B can discover
   a ZMQ channel registered on Hub A by pointing `in_hub_dir` at Hub A's
   directory (which contains `hub.pubkey`). Hub B's broker is not involved —
   Hub B's processor connects directly to Hub A's broker for discovery, then
   establishes the ZMQ connection directly to the endpoint.

---

## 3. Architecture — ZMQ Virtual Node in the Framework

### 3.1 Overall System View

```mermaid
graph LR
    subgraph "Hub A"
        direction TB
        BrokerA["BrokerService\n(ROUTER :5570)"]
        ProdA["hub::Producer\n(ZMQ PUSH)\ntransport=zmq\nendpoint=:5580"]
        QA["ZmqQueue\n(PUSH socket)"]
        ProdA --> QA
        ProdA -- "REG_REQ\ntransport=zmq\nendpoint=:5580" --> BrokerA
        BrokerA -- "stores endpoint\nin ChannelEntry" --> BrokerA
    end

    subgraph "Hub B"
        direction TB
        BrokerB["BrokerService\n(ROUTER :5571)"]
        ConsB["hub::Consumer\n(ZMQ PULL)\ntransport=zmq"]
        QB["ZmqQueue\n(PULL socket)"]
        ConsB --> QB
    end

    ConsB -- "CONSUMER_REG_REQ\n(via in_hub_dir → Hub A)" --> BrokerA
    BrokerA -- "CONSUMER_REG_ACK\ntransport=zmq\nendpoint=:5580" --> ConsB

    QA -- "ZMQ data frames\ntcp://:5580\n(peer-to-peer)" --> QB

    style BrokerA fill:#ddf,stroke:#66a
    style BrokerB fill:#ddf,stroke:#66a
    style QA fill:#ffd,stroke:#aa6
    style QB fill:#ffd,stroke:#aa6
```

### 3.2 Channel Types Side-by-Side

```mermaid
graph TD
    subgraph "SHM Channel (DataBlock)"
        direction LR
        P1["hub::Producer"] -- "REG_REQ\ntransport=shm\nshm_name, secret" --> B1["Broker"]
        B1 -- "CONSUMER_REG_ACK\nshm_name, secret" --> C1["hub::Consumer"]
        P1 -- "mmap segment\n(OS-backed)" --> C1
    end

    subgraph "ZMQ Virtual Channel"
        direction LR
        P2["hub::Producer"] -- "REG_REQ\ntransport=zmq\nzmq_endpoint=:5580" --> B2["Broker"]
        B2 -- "CONSUMER_REG_ACK\nzmq_endpoint=:5580" --> C2["hub::Consumer"]
        P2 -- "ZMQ PUSH/PULL\n(network/IPC)" --> C2
    end
```

---

## 4. ShmQueue vs ZmqQueue — Comparison

Both `hub::ShmQueue` and `hub::ZmqQueue` implement the abstract `hub::Queue`
interface. The processor and consumer script hosts call the same API regardless
of transport. This section explains when to choose each.

### 4.1 Interface

The framework uses two independent abstract interfaces (split 2026-03-09):

```cpp
// hub::QueueReader (hub_queue.hpp) — read-side abstract interface
class QueueReader {
public:
    virtual bool        start()   = 0;  // ShmQueue: no-op; ZmqQueue: bind/connect + recv thread
    virtual void        stop()    = 0;
    virtual const void* read_acquire(int timeout_ms) noexcept = 0;
    virtual void        read_release() noexcept = 0;
    virtual const void* read_flexzone() noexcept = 0;
    virtual uint64_t    last_seq()  const noexcept = 0;
    virtual size_t      capacity()  const = 0;
    virtual std::string policy_info() const = 0;
    virtual void        set_verify_checksum(bool slot, bool fz) noexcept = 0;
    virtual size_t      item_size() const noexcept = 0;
    virtual bool        is_running() const noexcept = 0;
    virtual QueueMetrics metrics() const noexcept = 0;
};

// hub::QueueWriter (hub_queue.hpp) — write-side abstract interface
class QueueWriter {
public:
    virtual bool  start()   = 0;
    virtual void  stop()    = 0;
    virtual void* write_acquire(int timeout_ms) noexcept = 0;
    virtual void  write_commit() noexcept  = 0;
    virtual void  write_discard() noexcept = 0;  // formerly write_abort()
    virtual void* write_flexzone() noexcept = 0;
    virtual void  set_checksum_options(bool slot, bool fz) noexcept = 0;
    virtual size_t      capacity()  const = 0;
    virtual std::string policy_info() const = 0;
    virtual size_t      item_size() const noexcept = 0;
    virtual bool        is_running() const noexcept = 0;
    virtual QueueMetrics metrics() const noexcept = 0;
};
```

`ShmQueue` and `ZmqQueue` both inherit from `QueueReader` and `QueueWriter`.
Factories return `unique_ptr<QueueReader>` (consumer side) or `unique_ptr<QueueWriter>` (producer side).

### 4.2 Feature Comparison

| Dimension | `ShmQueue` | `ZmqQueue` |
|-----------|-----------|-----------|
| **Transport layer** | OS shared memory (`mmap`) | ZMQ PUSH/PULL sockets |
| **Network reach** | Same machine only | Same machine, LAN, WAN |
| **Latency** | Sub-microsecond (cache-warm) | ~1–10 µs (loopback), variable on LAN |
| **Throughput** | Very high (memcpy rate) | High, limited by TCP/socket overhead |
| **Item size** | Fixed (slot size from schema) | Variable or fixed (raw bytes) |
| **Overflow policy** | `Block` or `Drop` (ring buffer) | `Block` (bounded recv queue) or `Drop` |
| **Schema validation** | Enforced at framework level | Not enforced (raw bytes) |
| **Broker awareness** | Full (name + secret in DISC_ACK) | Full (endpoint in DISC_ACK) — this HEP |
| **Flexzone support** | Yes (variable-length tail) | No (future: second ZMQ frame) |
| **Checksum support** | Yes (`BLAKE2b-256` of slot) | Deferred (HEP-CORE-0023) |
| **start() cost** | None (no-op) | Socket creation + thread spawn |
| **Reader model** | Single-consumer ring (latest or sequential) | Any-consumer (fire-and-forget) |
| **Multiple consumers** | Yes, each consumer has own read pointer | Yes (multiple PULL endpoints per PUSH) |
| **Process restart** | Consumer re-attaches after producer restart | PULL reconnects automatically |
| **Typical use case** | High-frequency sensor data, same machine, structured | Cross-machine bridge, raw byte streaming |
| **Cross-hub** | No (SHM segment is local) | Yes (ZMQ socket is network-capable) |

### 4.3 When to Use Each

**Use ShmQueue when:**
- Producer and consumer are on the same machine
- Data has a schema (`FieldDef` → `DataBlock` slots)
- You need flexzone (variable-length data appended to fixed schema)
- You need checksum verification of individual slots
- You need ring-buffer semantics (overwrite old data on overflow)
- Latency is critical and you want to avoid socket overhead

**Use ZmqQueue when:**
- Producer and consumer are on different machines (cross-network)
- You are bridging two hubs (processor-a on Hub A → processor-b on Hub B)
- Data format is opaque bytes (no schema enforcement needed)
- You need dynamic endpoint discovery (via ZMQ Virtual Channel Node)
- Multiple consumers on different machines need the same stream

### 4.4 Internal Structure

```mermaid
graph TD
    subgraph "ShmQueue (read side)"
        direction TB
        SC["DataBlockConsumer\n(attach to mmap segment)"]
        RB["Ring Buffer\n(slot_count slots)"]
        SL["SharedSpinLock\n(per-slot atomic PID)"]
        SC --> RB --> SL
    end

    subgraph "ShmQueue (write side)"
        direction TB
        SP["DataBlockProducer\n(create mmap segment)"]
        WB["Ring Buffer\n(acquire → write → release)"]
        WL["SharedSpinLock\n(atomic swap)"]
        SP --> WB --> WL
    end

    subgraph "ZmqQueue (PUSH side)"
        direction TB
        ZPS["ZMQ PUSH socket\n(bind or connect)"]
        ZPW["write() → zmq_send()\n(fire-and-forget)"]
        ZPS --> ZPW
    end

    subgraph "ZmqQueue (PULL side)"
        direction TB
        ZPL["ZMQ PULL socket\n(connect or bind)"]
        RT["recv_thread_\n(zmq_recv loop)"]
        BQ["bounded_queue_\n(item_count items)"]
        ZPL --> RT --> BQ
    end
```

---

## 5. Protocol Changes

### 5.1 REG_REQ Extension

A new optional `transport` field is added to `REG_REQ`. Existing registrations
without this field are treated as `transport=shm` (backward-compatible).

```
REG_REQ (producer → broker)
  channel_name          string   Channel identifier
  transport             string   "shm" (default) | "zmq"
  --- SHM fields (transport=shm) ---
  shm_name              string   Shared memory segment name
  shm_secret            uint64   Shared secret for DataBlock attachment
  slot_count            uint32   Ring buffer capacity
  schema_id             string   (optional) Named schema identifier
  schema_hash           string   (optional) BLAKE2b-256 of BLDS
  --- ZMQ fields (transport=zmq) ---
  zmq_endpoint          string   Bind address, e.g. "tcp://127.0.0.1:5580"
  zmq_bind              bool     true = producer binds; false = producer connects
  --- Common fields ---
  producer_uid          string
  producer_name         string
```

### 5.2 CONSUMER_REG_ACK Extension

The broker echoes back the transport descriptor from the channel entry.

```
CONSUMER_REG_ACK (broker → consumer)
  channel_name          string
  transport             string   "shm" | "zmq"
  --- SHM fields (transport=shm) ---
  shm_name              string
  shm_secret            uint64
  producer_zmq_pubkey   string   (for ZMQ ctrl socket auth, unchanged)
  --- ZMQ fields (transport=zmq) ---
  zmq_endpoint          string   Connect address for the consumer
  --- Common fields ---
  producer_uid          string
  producer_name         string
  schema_id             string   (optional)
  schema_hash           string   (optional)
```

### 5.3 Broker Internal State

The broker's `ChannelEntry` struct gains a `transport` discriminator and a
`zmq_endpoint` field (populated only for `transport=zmq`). No other broker
state changes. Channel lifecycle management (timeout, heartbeat tracking,
CHANNEL_CLOSING_NOTIFY, FORCE_SHUTDOWN, metrics) is identical for both transport types.

---

## 6. Protocol Sequence Diagrams

### 6.1 ZMQ Channel Registration and Discovery (same hub)

```mermaid
sequenceDiagram
    participant P as Producer<br/>(transport=zmq)
    participant B as Broker<br/>(ROUTER :5570)
    participant C as Consumer

    P->>B: REG_REQ {transport=zmq,<br/>zmq_endpoint=tcp://:5580,<br/>channel="lab.raw"}
    B->>B: Store ChannelEntry<br/>{transport=zmq,<br/>zmq_endpoint=tcp://:5580}
    B->>P: REG_ACK {status=ok}
    Note over P: Bind PUSH socket at :5580<br/>(ZmqQueue.start())

    loop heartbeat every 5s
        P->>B: HEARTBEAT_REQ {channel="lab.raw"}
        B->>P: HEARTBEAT_ACK
    end

    C->>B: CONSUMER_REG_REQ {channel="lab.raw"}
    B->>B: Lookup ChannelEntry<br/>transport=zmq
    B->>C: CONSUMER_REG_ACK {transport=zmq,<br/>zmq_endpoint=tcp://:5580}
    Note over C: Connect PULL socket<br/>to tcp://:5580<br/>(ZmqQueue.start())

    P-->>C: ZMQ data frames<br/>(peer-to-peer, not through broker)
```

### 6.2 Cross-Hub Discovery via in_hub_dir

```mermaid
sequenceDiagram
    participant P as Processor-A<br/>(Hub A, PUSH :5580)
    participant BA as Broker A<br/>(:5570)
    participant PB as Processor-B<br/>(Hub B, in_hub_dir=hub-a/)
    participant BB as Broker B<br/>(:5571)

    Note over P,BA: Hub A side: producer registered
    P->>BA: REG_REQ {transport=zmq,<br/>channel="lab.bridge.raw",<br/>zmq_endpoint=tcp://X:5580}
    BA->>P: REG_ACK

    Note over PB,BB: Hub B processor starts
    PB->>BA: CONSUMER_REG_REQ {channel="lab.bridge.raw"}<br/>(direct connection to Hub A broker)
    BA->>PB: CONSUMER_REG_ACK {transport=zmq,<br/>zmq_endpoint=tcp://X:5580}
    Note over PB: Discovered endpoint without<br/>hardcoding in config file

    PB->>BB: REG_REQ {channel="lab.bridge.processed",<br/>transport=shm, ...}
    BB->>PB: REG_ACK

    P-->>PB: ZMQ data frames<br/>tcp://X:5580 (peer-to-peer)
    PB-->>BB: SHM slot writes
```

### 6.3 SHM Channel Registration and Discovery (for comparison)

```mermaid
sequenceDiagram
    participant P as Producer<br/>(transport=shm)
    participant B as Broker
    participant C as Consumer

    P->>B: REG_REQ {transport=shm,<br/>shm_name="pylabhub_lab_raw",<br/>shm_secret=3333333333,<br/>channel="lab.raw"}
    B->>B: Store ChannelEntry<br/>{shm_name, shm_secret}
    B->>P: REG_ACK {status=ok}

    C->>B: CONSUMER_REG_REQ {channel="lab.raw"}
    B->>C: CONSUMER_REG_ACK {transport=shm,<br/>shm_name="pylabhub_lab_raw",<br/>shm_secret=3333333333}
    Note over C: Attach to shared memory<br/>segment (mmap)

    P-->>C: SHM slot writes<br/>(ring buffer, in-process)
```

---

## 7. Hub Layer Changes

### 7.1 ProducerOptions

```cpp
enum class ChannelTransport { Shm, Zmq };

struct ProducerOptions
{
    // existing fields ...
    ChannelTransport transport{ChannelTransport::Shm};
    std::string      zmq_endpoint;   // bind address (transport=Zmq only)
    bool             zmq_bind{true}; // producer binds by default
};
```

When `transport=Zmq`, `hub::Producer::create()`:
1. Sends `REG_REQ{transport=zmq, zmq_endpoint}` to the broker
2. Creates and binds (or connects) a ZMQ PUSH socket at `zmq_endpoint`
3. Manages the PUSH socket lifetime (start/stop with the producer)
4. `producer.queue()` returns a `hub::ZmqQueue*` wrapping the PUSH socket

When `transport=Shm` (default), behavior is unchanged.

### 7.2 Consumer::connect()

`Consumer::connect()` reads `transport` from `CONSUMER_REG_ACK` and acts accordingly:

```cpp
// Inside Consumer::connect() after receiving CONSUMER_REG_ACK:
if (ack.transport == "shm")
{
    shm_consumer = find_datablock_consumer_impl(ack.shm_name, ack.shm_secret, ...);
    // queue_ = ShmQueue wrapping shm_consumer
}
else if (ack.transport == "zmq")
{
    // queue_ = ZmqQueue::pull_from(ack.zmq_endpoint, slot_size, bind=false, ...)
}
```

`consumer.queue()` returns the appropriate `hub::Queue*`. The caller (ProcessorScriptHost,
ConsumerScriptHost) uses the queue uniformly — no transport branching at the script host level.

### 7.3 ProcessorScriptHost Simplification (Implemented 2026-03-09)

`ProcessorScriptHost` uses transport-resolved queues via `hub::QueueReader` / `hub::QueueWriter`:

```cpp
in_queue_  = in_consumer_->queue_reader();   // QueueReader* — ShmQueue or ZmqQueue
out_queue_ = out_producer_->queue_writer();  // QueueWriter* — ShmQueue or ZmqQueue
```

`ProcessorConfig` fields `in_transport`, `zmq_in_endpoint`, `zmq_in_bind` are
**removed** — input transport auto-discovered from the broker (`CONSUMER_REG_ACK`).
Output-side fields retained:

```json
{
  "out_transport":    "zmq",
  "zmq_out_endpoint": "tcp://127.0.0.1:5580",
  "zmq_out_bind":     true
}
```

Only the **output side** needs explicit configuration; input side discovers dynamically.
`zmq_out_bind` defaults to `true` (PUSH binds; PULL connects).

---

## 8. Relation to DataBlock (SHM)

```
                  ┌─────────────────────────────────────────────────┐
                  │                 Channel Types                    │
                  ├─────────────────────┬───────────────────────────┤
                  │    SHM (DataBlock)  │    ZMQ Virtual Node       │
                  ├─────────────────────┼───────────────────────────┤
                  │ Data structure      │ Connection point only      │
                  │ Slots + ring buffer │ No slots, no buffer        │
                  │ Schema required     │ Schema optional            │
                  │ Shared memory seg   │ No memory allocation       │
                  │ OS IPC (mmap)       │ Network/IPC (ZMQ socket)   │
                  │ Same-machine only   │ Cross-machine capable      │
                  │ Broker-registered   │ Broker-registered ✓        │
                  │ Broker-discovered   │ Broker-discovered ✓        │
                  │ Heartbeat/metrics   │ Heartbeat/metrics ✓        │
                  │ Lifecycle events    │ Lifecycle events ✓         │
                  └─────────────────────┴───────────────────────────┘
```

Both types participate fully in the channel lifecycle. The difference is purely
in what the broker stores (SHM name + secret vs. ZMQ endpoint) and what the
framework creates on the consumer side (DataBlockConsumer vs. ZMQ PULL socket).

---

## 9. Cross-Hub Discovery

For the dual-hub bridge scenario:

```
Hub A:  Processor-A registers "lab.bridge.raw.bridge" {transport=zmq, endpoint=tcp://X:5580}
Hub B:  Processor-B has in_hub_dir → Hub A

Processor-B startup:
  1. Connects to Hub A broker (via in_hub_dir + hub.pubkey)
  2. Sends CONSUMER_REG_REQ for "lab.bridge.raw.bridge"
  3. Hub A broker returns CONSUMER_REG_ACK{transport=zmq, zmq_endpoint=tcp://X:5580}
  4. Processor-B creates ZmqQueue PULL at tcp://X:5580
  5. Connects to Hub B broker (via out_hub_dir + hub.pubkey) → registers out_channel
```

Processor-B config becomes:
```json
{
  "in_hub_dir":  "../hub-a",
  "in_channel":  "lab.bridge.raw.bridge",
  "out_hub_dir": "../hub-b",
  "out_channel": "lab.bridge.processed",
  "out_transport": "shm"
}
```

No `zmq_in_endpoint` in Processor-B — it is discovered from Hub A at runtime.

---

## 10. Robustness Analysis

| Scenario | Behavior |
|----------|----------|
| ZMQ endpoint not yet bound (producer not started) | `CONSUMER_REG_REQ` succeeds (broker has the entry); ZMQ PULL socket connects and queues internally until PUSH becomes available. Same as SHM race condition — producer must be running before consumer reads. |
| Producer restarts with new ephemeral port | Producer sends new `REG_REQ` with updated `zmq_endpoint`. Broker updates channel entry. Existing consumers receive `CHANNEL_CLOSING_NOTIFY` (broker sees producer re-register), then can re-discover via `CONSUMER_REG_REQ`. |
| Consumer discovers endpoint but PUSH never binds | ZMQ PULL socket's reconnect loop retries silently. After `channel_timeout`, broker drops the channel entry (no heartbeat from producer). |
| Cross-machine deployment | ZMQ endpoints are network addresses — works natively. SHM channels remain same-machine only (no change). |
| Schema validation | Schema fields in REG_REQ are optional for `transport=zmq`. If provided, broker stores them and echoes back — consumer may validate format independently. |

---

## 11. What This Is Not

- **Not a message broker relay**: The broker stores the endpoint, not the data.
  ZMQ data flows directly between producer and consumer.
- **Not a replacement for SHM**: SHM (DataBlock) provides structured, schema-validated,
  ring-buffered IPC with hardware-level synchronization. ZMQ nodes trade structure
  for flexibility and network reach.
- **Not cross-hub channel federation**: A ZMQ channel registered on Hub A is not
  automatically visible on Hub B. Hub B must explicitly point `in_hub_dir` at Hub A
  to discover it. Hub federation (broadcast relay) is a separate feature (HEP-CORE-0022).

---

## 12. Implementation Status

| Phase | Scope | Status | Key files |
|-------|-------|--------|-----------|
| 1 | Protocol: `transport` + `zmq_endpoint` in REG_REQ / CONSUMER_REG_ACK / ChannelEntry | Done | `broker_service.cpp`, `messenger.cpp` |
| 2 | `hub::Producer` ZMQ mode: `ProducerOptions::transport`, PUSH socket management, `producer.queue()` | Done | `hub_producer.hpp/cpp` |
| 3 | `hub::Consumer` ZMQ discovery: read transport from ACK, create ZmqQueue PULL, `consumer.queue()` | Done | `hub_consumer.hpp/cpp` |
| 4 | `ProcessorScriptHost` simplification: remove manual ZmqQueue creation; use `producer/consumer.queue()` | Done | `processor_script_host.hpp/cpp` |
| 5 | `ProcessorConfig` cleanup: remove `in_transport`/`zmq_in_endpoint`; keep `out_transport`/`zmq_out_endpoint` | Done | `processor_config.hpp/cpp` |
| 6 | Demo update: Processor-B uses `in_hub_dir` only, no hardcoded ZMQ endpoint | Done | `share/py-demo-dual-processor-bridge/` |
| Tests | 12 L3 protocol tests (ZmqVirtualChannelTest) | Done | `test_datahub_zmq_virtual_channel.cpp` |
| 7 | Ephemeral port resolution: `ENDPOINT_UPDATE_REQ`, broker readiness guard, `establish_channel` rename | Design | §16 (2026-03-26) |

---

## 13. ZmqQueue Internal Design

`hub::ZmqQueue` is schema-mandatory. Every ZmqQueue must be created with a non-empty
`std::vector<ZmqSchemaField>` + packing string. Wire format is msgpack:

```
fixarray[4]
  [0] magic       : uint32  = 0x51484C50 ('PLHQ')
  [1] schema_tag  : bin8    = first 8 bytes of BLAKE2b-256(BLDS) (or zeros if absent)
  [2] seq         : uint64  = monotonic send counter (gaps detected as recv_gap_count)
  [3] payload     : array[N] — one element per schema field
```

Payload encoding: scalars as native msgpack types; arrays/string/bytes as `bin` (raw bytes).

**Packing**: `"aligned"` = C `ctypes.LittleEndianStructure` natural alignment; `"packed"` = no padding.
Both sides must use identical schema + packing. `"natural"` is not a valid value (renamed to `"aligned"` 2026-03-08).

**Factories**:
- `ZmqQueue::pull_from(endpoint, schema, item_size, bind, buffer_depth, packing)` → PULL (recv_thread_)
- `ZmqQueue::push_to(endpoint, schema, item_size, bind, buffer_depth, packing)` → PUSH (send_thread_)

**Metrics**: `recv_overflow_count`, `recv_frame_error_count`, `recv_gap_count`, `send_drop_count`, `send_retry_count`, `overrun_count` — all `std::atomic<uint64_t>` on `ZmqQueue::metrics()`.

**InboxQueue (ROUTER/DEALER pair)**: implemented 2026-03-08.
- `hub::InboxQueue` (ROUTER): binds `inbox_endpoint`; `recv_one(timeout)` + `send_ack(code)` on inbox_thread_
- `hub::InboxClient` (DEALER): `connect_to(ep, sender_uid, schema, packing)` + `send(timeout)` + ACK wait
- Wire format identical to ZmqQueue data frames
- See `src/include/utils/hub_inbox_queue.hpp` + `src/utils/hub/hub_inbox_queue.cpp`

**Deferred extensions**: ACK mechanism for ZmqQueue PUSH write path (requires PUSH→DEALER socket change; deferred); flexzone over ZMQ (second frame; deferred to HEP-0023).

This HEP covers the **broker protocol** (virtual node registration, service directory).
Full ZmqQueue implementation detail was drafted in `docs/tech_draft/zmq_queue_design.md`
(archived 2026-03-10 to `docs/archive/transient-2026-03-10/`).

---

## 14. Source File Reference

| Component | File |
|-----------|------|
| Channel entry + transport field | `src/utils/ipc/broker_service.cpp` |
| REG_REQ / CONSUMER_REG_ACK protocol | `src/utils/ipc/messenger.cpp` |
| ChannelHandle data_transport() / zmq_node_endpoint() | `src/utils/ipc/channel_handle.cpp/.hpp` |
| Producer ZMQ transport | `src/utils/hub/hub_producer.hpp/cpp` |
| Consumer ZMQ discovery | `src/utils/hub/hub_consumer.hpp/cpp` |
| ProcessorScriptHost (simplified) | `src/processor/processor_script_host.cpp` |
| ProcessorConfig (transport fields) | `src/processor/processor_config.hpp/cpp` |
| L3 protocol tests | `tests/test_layer3_datahub/test_datahub_zmq_virtual_channel.cpp` |
| Demo bridge config | `share/py-demo-dual-processor-bridge/processor-{a,b}/processor.json` |

---

## 15. ZMQ Socket Lifecycle Policy

### 15.1 LINGER=0 — Universal Policy

**Every ZMQ socket in pyLabHub sets `ZMQ_LINGER = 0` immediately after creation.**

```cpp
// Canonical pattern — applied at socket creation, not deferred:
zmq::socket_t sock(ctx, zmq::socket_type::push);
sock.set(zmq::sockopt::linger, 0);  // policy: always LINGER=0
```

This is enforced in all socket creation sites:

| Socket | Location | Type |
|--------|----------|------|
| Broker main | `src/utils/ipc/broker_service.cpp` | ROUTER |
| Federation peer | `src/utils/ipc/broker_service.cpp` | DEALER |
| Messenger broker connection | `src/utils/ipc/messenger.cpp` | DEALER |
| Messenger monitor | `src/utils/ipc/messenger.cpp` | PAIR |
| Messenger P2C ctrl (server) | `src/utils/ipc/messenger.cpp` | ROUTER |
| Messenger P2C ctrl (client) | `src/utils/ipc/messenger.cpp` | DEALER |
| Messenger P2C data | `src/utils/ipc/messenger.cpp` | XPUB / SUB / PUSH / PULL |
| AdminShell | `src/hub_python/admin_shell.cpp` | REP |
| ZmqQueue PUSH | `src/utils/hub/hub_zmq_queue.cpp` | PUSH |
| ZmqQueue PULL | `src/utils/hub/hub_zmq_queue.cpp` | PULL |
| InboxQueue | `src/utils/hub/hub_inbox_queue.cpp` | ROUTER |
| InboxClient | `src/utils/hub/hub_inbox_queue.cpp` | DEALER |

### 15.2 Rationale

**ZMQ's default LINGER is -1** (infinite): `zmq_close()` blocks until all queued
outgoing messages are delivered or the socket's linger timeout expires. This creates
two failure modes in pyLabHub:

1. **Hang on shutdown**: If the remote end has already closed, `zmq_close()` blocks
   indefinitely trying to flush the send queue — particularly dangerous for the
   monitor PAIR socket, which is closed before the main socket it monitors.

2. **Release vs. Debug timing divergence**: In Release builds (no -O2 overhead, no
   `PLH_DEBUG` I/O), the ZMQ I/O thread may not have processed a socket disconnect
   before `zmq_close()` is called, making hangs **deterministic** in Release but a
   **race** in Debug. This was the root cause of the `MessengerTest` hangs observed
   only in Release (2026-03-12).

**pyLabHub does not rely on socket linger for delivery guarantees.** Shutdown is
always coordinated through explicit protocol messages:
- Producers send `CHANNEL_CLOSING_NOTIFY` before closing.
- Processors/consumers receive `FORCE_SHUTDOWN` or handle `CHANNEL_CLOSING_NOTIFY`.
- All parties wait for protocol-level acknowledgment before destroying sockets.

Since message delivery is guaranteed by protocol, not by socket linger, LINGER=0
is correct and safe. `zmq_close()` discards pending sends immediately — the
counterpart has already received a shutdown notification through the control path.

### 15.3 Exceptions

There are **no exceptions** to this policy in pyLabHub. If a future socket requires
reliable delivery after `close()` (e.g., a one-shot persistent-send path with no
protocol-level ACK), document the exception explicitly with the socket creation
comment and provide the reason.

### 15.4 Setting Location

LINGER must be set **at creation**, not deferred to close time. Deferred setting:

```cpp
// WRONG — race condition: the I/O thread may have already queued sends
// before linger is set, so it has no effect.
sock.close();  // already sends with linger=-1
sock.set(zmq::sockopt::linger, 0);  // too late

// CORRECT — set before any use:
zmq::socket_t sock(ctx, zmq::socket_type::push);
sock.set(zmq::sockopt::linger, 0);
sock.bind(endpoint);
```

This also applies when sockets are moved or reassigned — the LINGER=0 is preserved
in the socket option, not the C++ object. Reassignment creates a new socket; always
set LINGER=0 immediately after.

---

## 16. Ephemeral Port Resolution (port-0 binding)

**Status**: Design — 2026-03-26. Not yet implemented.

### 16.1 Problem

When `zmq_node_endpoint` is configured as `tcp://127.0.0.1:0`, the OS assigns an
ephemeral port at bind time. The current flow registers the **config value** (`:0`)
with the broker before the ZmqQueue binds. Consumers discover `:0` from the broker
and cannot connect.

### 16.2 Current Factory Flow

The Producer has three functions involved in creation:

- **`messenger.create_channel(name, opts)`** — sends `CREATE_CHANNEL_REQ` to broker,
  returns a `ChannelHandle` (ctrl/data sockets). This is the broker registration step.
- **`Producer::create(messenger, opts)`** — public entry point (template and non-template
  variants). Validates types/schemas, calls `create_channel`, creates DataBlock,
  then calls `establish_channel`.
- **`Producer::establish_channel(...)`** — assembles
  the Producer: wires callbacks, creates ZmqQueue, creates ShmQueue wrapper, configures
  ctrl_queue and peer timeout.

```
create() / create<F,D>():
  1. Validate types/schemas (template only)
  2. messenger.create_channel() → broker stores ":0"          ← BUG: useless endpoint
  3. Create DataBlock (if SHM)
  4. Call establish_channel()

establish_channel():
  5. Wire Messenger callbacks
  6. ZmqQueue::push_to(":0") → bind → OS assigns :45782
  7. ZmqQueue::start()                                        ← actual_endpoint() now available
  8. Create ShmQueue wrapper
  9. Return Producer
```

At step 2, the broker stores `:0`. At step 7, the actual port is known but never
communicated back to the broker.

### 16.3 Design: ENDPOINT_UPDATE

After the ZmqQueue binds and starts (step 7), `establish_channel` sends an
`ENDPOINT_UPDATE_REQ` to the broker with the resolved endpoint from
`ZmqQueue::actual_endpoint()`.

**New protocol message:**

| Field | Type | Description |
|-------|------|-------------|
| `channel_name` | string | Channel to update |
| `zmq_node_endpoint` | string | Resolved endpoint (e.g., `tcp://127.0.0.1:45782`) |

**Broker response:** `ENDPOINT_UPDATE_ACK` with status `"ok"` or `"error"`.

### 16.4 Broker Readiness Guard

The broker maintains a **readiness flag** on each ZMQ channel entry:

- On `CREATE_CHANNEL` with `transport=zmq`: if the registered `zmq_node_endpoint`
  contains port `0` (parsed), mark the channel as **not ready**.
- On `ENDPOINT_UPDATE_REQ`: validate the new endpoint (non-zero port), update the
  stored endpoint, mark the channel as **ready**.
- On `CONSUMER_REG_REQ` (discovery): if the target channel is ZMQ and **not ready**,
  reject with a clear error (`CHANNEL_NOT_READY`). The consumer can retry after a
  short delay.

For channels registered with a specific port (not `:0`), the channel is immediately
**ready** — no update required.

### 16.5 Updated Factory Flow

```
create() / create<F,D>():
  1. Validate types/schemas (template only)
  2. messenger.create_channel() → broker stores ":0", marks NOT READY
  3. Create DataBlock (if SHM)
  4. Call establish_channel()

establish_channel():
  5. Wire Messenger callbacks
  6. ZmqQueue::push_to(":0") → bind → OS assigns :45782
  7. ZmqQueue::start()
  8. messenger.update_endpoint(channel, actual_endpoint())    ← NEW
     → broker stores ":45782", marks READY
  9. Create ShmQueue wrapper
  10. Return Producer
```

### 16.6 Design Notes

- **No factory reordering**: The create → establish_channel split is unchanged.
  The update happens within `establish_channel` after ZmqQueue is ready.
- **Cleanup on failure**: If `ENDPOINT_UPDATE_REQ` fails, `establish_channel`
  returns nullopt. The ZmqQueue unique_ptr is destroyed automatically.
  The channel remains in the broker as not-ready; the Producer destructor's
  `close()` sends the deregistration.
- **SHM channels unaffected**: The readiness guard only applies when
  `transport=zmq` and the registered endpoint has port 0.
- **Existing deployments**: Channels registered with a specific port are
  immediately ready. No behavior change for non-port-0 configs.
- **Template vs non-template**: Both `create()` variants call `establish_channel`
  which handles the update. No duplication.
- **Rename**: `create_from_parts` → `establish_channel` — done (this commit).

### 16.7 Relation to Producer/Consumer API Layering

This change is internal to `establish_channel` and the broker. No impact on:
- Role host code (calls `Producer::create()` as before)
- Forwarding API (`write_acquire`, `queue_metrics`, etc.)
- Consumer discovery flow (broker returns the correct endpoint once ready)

The only visible change is that consumers connecting to a port-0 channel before the
producer is fully ready receive `CHANNEL_NOT_READY` instead of a bad endpoint.

