# Channel Expansion Design

**Status:** Draft — approved for implementation (2026-02-18)
**Author:** design session
**Supersedes:** none (extends HEP-CORE-0002 §6)
**Related TODO:** `docs/todo/MESSAGEHUB_TODO.md`

---

## 1. Overview

The current broker is a discovery-only registry: producers register shared memory
segments; consumers discover them. The expanded design adds:

- **Direct ZMQ channels** between producer and consumer (not through the broker in the
  data path)
- **Optional shared memory** — still the right choice for bulk/high-throughput data, but
  no longer the only option
- **Broker as setup facilitator** — owns channel lifetime, mediates initial connection,
  detects dead producers via heartbeat
- **Universal ZMQ framing** — one ASCII type byte as Frame 0 on every ZMQ socket in the
  project

---

## 2. Universal ZMQ Framing

Every ZMQ multi-part message in pylabhub (broker ↔ Messenger, P2C ctrl, P2C data) begins
with Frame 0 = **one ASCII byte** identifying the payload type.

| Byte | Meaning | Subsequent frames |
|------|---------|-------------------|
| `'C'` (0x43) | Control | Frame 1: type string (e.g. `"REG_REQ"`), Frame 2: JSON body |
| `'A'` (0x41) | ASCII / JSON data | Frame 1: JSON payload string |
| `'M'` (0x4D) | MessagePack data | Frame 1: MessagePack bytes |

**Why ZMQ frames solve the length question:** ZMQ multi-part messages deliver each frame
as a separate buffer with its own implicit length. No separator character or length prefix
is needed. `recv_multipart` gives you exactly the frames the sender put in.

### 2.1 Control message structure (broker protocol, P2C ctrl socket)

```
Frame 0: 'C'
Frame 1: "REG_REQ"               ← message type string
Frame 2: {"channel_name": "…"}   ← JSON body
```

The broker ROUTER socket prepends an identity frame on the receive side (ZMQ mechanism,
not application-level):

```
Frame 0: <ZMQ identity>   ← ROUTER mechanism only, invisible to DEALER sender
Frame 1: 'C'
Frame 2: "REG_REQ"
Frame 3: {"channel_name": "…"}
```

### 2.2 JSON data message (P2C data socket)

```
Frame 0: 'A'
Frame 1: {"sensor_id": 42, "value": 3.14, "ts_ns": 1234567890}
```

### 2.3 MessagePack data message (P2C data socket)

```
Frame 0: 'M'
Frame 1: <MessagePack encoded bytes>
```

### 2.4 Extensibility

New type bytes can be added without affecting existing receivers; unknown type bytes
are logged and dropped.

---

## 3. Channel Transport Model

**ZMQ is always present** — it is the channel backbone for control, heartbeat, and
optionally data. SharedMemory (DataBlock) is an optional add-on for high-throughput
bulk data.

```
Every channel has:
  ZMQ ctrl socket   (ROUTER/DEALER)    — heartbeat, control, Bidir data
  ZMQ data socket   (XPUB/SUB or       — streaming data (PubSub / Pipeline)
                     PUSH/PULL)           absent for Bidir (uses ctrl socket)

Optionally:
  DataBlock segment (shared memory)    — bulk zero-copy data
```

The producer declares `has_shared_memory` in REG_REQ. Consumers learn this from
DISC_ACK and attach accordingly.

---

## 4. Channel Patterns

```cpp
enum class ChannelPattern {
    PubSub,    // producer XPUB (binds), consumers SUB  (connect) — streaming, one-to-many
    Pipeline,  // producer PUSH  (binds), consumers PULL (connect) — load-balanced pipeline
    Bidir,     // producer ROUTER(binds), consumers DEALER(connect)— full bidirectional
};
```

| Pattern | ZMQ ctrl socket | ZMQ data socket | Shared memory |
|---------|----------------|----------------|---------------|
| PubSub  | ROUTER / DEALER | XPUB / SUB     | optional      |
| Pipeline| ROUTER / DEALER | PUSH / PULL    | optional      |
| Bidir   | ROUTER / DEALER (data+ctrl combined) | — | optional |

For `PubSub` and `Pipeline`, the ctrl socket carries control messages and heartbeat
only; bulk data goes on the data socket. For `Bidir`, a single ROUTER/DEALER pair
carries both.

---

## 5. Updated Wire Protocol

All new fields in REG_REQ and DISC_ACK are additive. Omitting `has_shared_memory`
defaults to `false`; omitting `channel_pattern` defaults to `"PubSub"`. Existing
clients that do not send these fields remain compatible.

### 5.1 REG_REQ (producer → broker) — updated

```json
{
  "channel_name":      "sensor.temp",
  "schema_hash":       "…",
  "schema_version":    1,
  "producer_pid":      12345,

  "has_shared_memory": true,
  "shm_name":          "/pylabhub_sensor_temp",

  "channel_pattern":   "PubSub",
  "zmq_ctrl_endpoint": "tcp://192.168.1.100:5556",
  "zmq_data_endpoint": "tcp://192.168.1.100:5557",
  "zmq_pubkey":        "<40-char Z85 producer public key>"
}
```

- `zmq_ctrl_endpoint`: producer pre-binds this ROUTER socket before calling REG_REQ.
- `zmq_data_endpoint`: pre-bound XPUB or PUSH socket; empty for Bidir.
- `zmq_pubkey`: CurveZMQ public key for the producer's P2C sockets (distinct from
  the broker's own keypair).

### 5.2 REG_ACK (broker → producer)

Unchanged fields; broker now sets channel status to `pending_ready` internally.
Producer must send its first HEARTBEAT_REQ before consumers can discover the channel.

### 5.3 DISC_ACK (broker → consumer) — updated

```json
{
  "status":            "success",
  "shm_name":          "/pylabhub_sensor_temp",
  "schema_hash":       "…",
  "schema_version":    1,
  "consumer_count":    2,

  "has_shared_memory": true,
  "channel_pattern":   "PubSub",
  "zmq_ctrl_endpoint": "tcp://192.168.1.100:5556",
  "zmq_data_endpoint": "tcp://192.168.1.100:5557",
  "zmq_pubkey":        "<40-char Z85 producer public key>"
}
```

### 5.4 HEARTBEAT_REQ (producer → broker) — new, fire-and-forget, no ACK

```json
{ "channel_name": "sensor.temp", "producer_pid": 12345 }
```

Sent periodically (default: every 2 s) by the Messenger worker thread after
`create_channel` returns. Broker updates `last_heartbeat` and transitions channel
from `pending_ready` → `ready` on first receipt.

### 5.5 CHANNEL_NOT_READY error (broker → consumer, during DISC_REQ)

```json
{ "status": "error", "error_code": "CHANNEL_NOT_READY",
  "message": "Producer has not sent first heartbeat yet" }
```

`connect_channel` retries DISC_REQ until it receives a non-error response or times out.

### 5.6 CHANNEL_CLOSING_NOTIFY (broker → consumers) — new, best-effort

```json
{ "channel_name": "sensor.temp", "reason": "heartbeat_timeout" }
```

Sent by broker to all registered consumer Messenger connections when a channel is
forcibly closed (heartbeat timeout, broker shutdown). Uses the existing DEALER identity
the broker holds from each consumer's prior REG/DISC interaction.

Type byte: `'C'`, type string: `"CHANNEL_CLOSING_NOTIFY"`.

---

## 6. ChannelEntry Extensions

```cpp
enum class ChannelStatus  { PendingReady, Ready, Closing };
enum class ChannelPattern { PubSub, Pipeline, Bidir };

struct ChannelEntry {
    // --- existing ---
    std::string    shm_name;
    std::string    schema_hash;
    uint32_t       schema_version{0};
    uint64_t       producer_pid{0};
    std::string    producer_hostname;
    nlohmann::json metadata;
    std::vector<ConsumerEntry> consumers;

    // --- new ---
    ChannelStatus  status{ChannelStatus::PendingReady};
    std::chrono::steady_clock::time_point last_heartbeat{};
    bool           has_shared_memory{false};
    ChannelPattern pattern{ChannelPattern::PubSub};
    std::string    zmq_ctrl_endpoint;
    std::string    zmq_data_endpoint;   // empty for Bidir
    std::string    zmq_pubkey;
};
```

---

## 7. ProducerInfo / ConsumerInfo Extensions

```cpp
struct ProducerInfo {
    // --- existing ---
    std::string shm_name;
    uint64_t    producer_pid;
    std::string schema_hash;
    uint32_t    schema_version;
    // --- new ---
    bool           has_shared_memory{false};
    ChannelPattern pattern{ChannelPattern::PubSub};
    std::string    zmq_ctrl_endpoint;
    std::string    zmq_data_endpoint;
    std::string    zmq_pubkey;
};

struct ConsumerInfo {
    // --- existing ---
    std::string shm_name;
    std::string schema_hash;
    uint32_t    schema_version;
    // --- new (filled from DISC_ACK) ---
    bool           has_shared_memory{false};
    ChannelPattern pattern{ChannelPattern::PubSub};
    std::string    zmq_ctrl_endpoint;
    std::string    zmq_data_endpoint;
    std::string    zmq_pubkey;
};
```

---

## 8. ChannelHandle (new class)

Lives in `src/include/utils/channel_handle.hpp` + `src/utils/channel_handle.cpp`.
Pimpl idiom for ABI stability. Part of `pylabhub::hub` namespace.

```cpp
class ChannelHandle {
public:
    ~ChannelHandle();  // closes ZMQ sockets, resets DataBlock handles, stops heartbeat

    // --- ZMQ data send (producer) ---
    // PubSub/Pipeline: broadcasts to all subscribers.
    // Bidir: routes to specific consumer by identity (empty = broadcast attempt).
    bool send(const void* data, size_t size,
              const std::string& identity = {});

    // --- ZMQ data receive ---
    // Consumer (PubSub/Pipeline): receives from producer data socket.
    // Bidir producer: receives from any consumer; out_identity filled with sender ID.
    bool recv(std::vector<std::byte>& buf,
              int timeout_ms = 5000,
              std::string* out_identity = nullptr);

    // --- ZMQ control (PubSub/Pipeline only; Bidir uses send/recv directly) ---
    // Consumer sends a control message up to producer via ctrl socket.
    bool send_ctrl(const void* data, size_t size);
    // Producer receives control message from any consumer.
    bool recv_ctrl(std::vector<std::byte>& buf,
                   int timeout_ms = 5000,
                   std::string* out_identity = nullptr);

    // --- SharedMemory ---
    DataBlockProducer* producer();   // nullptr if !has_shared_memory or consumer side
    DataBlockConsumer* consumer();   // nullptr if !has_shared_memory or producer side

    // --- Introspection ---
    ChannelPattern     pattern()      const;
    bool               has_shm()      const;
    const std::string& channel_name() const;
    bool               is_valid()     const;  // false after CHANNEL_CLOSING_NOTIFY
};
```

ZMQ sockets in `ChannelHandle` share the `zmq::context_t` from `ZMQContext` lifecycle
module — no second context.

---

## 9. Messenger API Extensions

```cpp
class Messenger {
    // --- existing methods unchanged ---

    // Producer: synchronous (waits for REG_ACK with timeout).
    // Binds ZMQ sockets, creates DataBlock if has_shared_memory, sends REG_REQ,
    // starts heartbeat timer on success.
    [[nodiscard]] std::optional<ChannelHandle>
    create_channel(const std::string& channel_name,
                   ChannelPattern pattern,
                   bool has_shared_memory = false,
                   const DataBlockConfig& config = {},
                   int timeout_ms = 5000);

    // Consumer: synchronous (retries DISC_REQ until channel ready or timeout).
    // Connects ZMQ sockets, attaches DataBlock if has_shared_memory,
    // sends CONSUMER_REG_REQ on success.
    [[nodiscard]] std::optional<ChannelHandle>
    connect_channel(const std::string& channel_name,
                    int timeout_ms = 5000,
                    const DataBlockConfig& config = {});

    // Register callback invoked when broker sends CHANNEL_CLOSING_NOTIFY.
    void on_channel_closing(std::function<void(const std::string& channel)> cb);
};
```

---

## 10. Bidir Identity Handshake

After `connect_channel` returns (Bidir pattern), a lightweight application-level
handshake runs on the P2C ctrl (= Bidir) socket:

```
Consumer → Producer:  'C', "HELLO", {"consumer_pid": 12345, "consumer_hostname": "ws-01"}
Producer → Consumer:  'C', "HELLO_ACK", {"assigned_id": "consumer_0"}
```

Producer assigns identity strings (`"consumer_0"`, `"consumer_1"`, …). The consumer
stores its assigned ID; the producer uses it in `send(data, size, "consumer_0")` to
address that specific consumer.

---

## 11. CurveZMQ on P2C Sockets

The producer generates a **separate** CurveZMQ keypair for its ChannelHandle ZMQ
sockets (distinct from the Messenger's broker keypair). The public key travels:

```
Producer → REG_REQ (zmq_pubkey) → Broker stores it → DISC_ACK → Consumer
```

The consumer sets `zmq_pubkey` as the server key on its ZMQ sockets before connecting.
The broker never uses or verifies this key — it simply stores and forwards it.

---

## 12. Channel Lifecycle

```
Producer                    Broker                      Consumer
─────────────────────────────────────────────────────────────────
bind ZMQ sockets
→ REG_REQ             →     store channel (pending_ready)
← REG_ACK             ←
start heartbeat (2s)
→ HEARTBEAT_REQ       →     mark channel ready
                                                ← DISC_REQ
                            check status=ready →
                       ←    DISC_ACK (+ zmq_pubkey)  →
                                                connect ZMQ sockets
                                                → CONSUMER_REG_REQ →
                                                ← CONSUMER_REG_ACK ←
                            [heartbeat timeout]
                       ←    CHANNEL_CLOSING_NOTIFY    →
                                                is_valid() = false
→ DEREG_REQ           →     remove channel
← DEREG_ACK           ←
```

Broker-initiated close (heartbeat timeout / shutdown): broker sends
`CHANNEL_CLOSING_NOTIFY` to all registered consumer DEALER identities it holds,
then removes the channel.

Normal close: producer calls `deregister_producer()` (or ChannelHandle destructs),
stops heartbeat, sends DEREG_REQ. Consumers detect via `is_valid()` or callback.

---

## 13. Heartbeat Parameters (configurable)

| Parameter | Default | Location |
|-----------|---------|----------|
| Heartbeat interval | 2 s | `BrokerService::Config::heartbeat_interval` |
| Dead channel timeout | 10 s (5 missed) | `BrokerService::Config::channel_timeout` |

---

## 14. Implementation Phases

| Phase | Scope | Status |
|-------|-------|--------|
| 1. Universal framing | Prepend `'C'` to all broker messages; update `raw_req` test helpers | 🟡 In Progress |
| 2. Wire protocol extensions | New fields in REG_REQ/DISC_ACK; HEARTBEAT_REQ; CHANNEL_NOT_READY; ChannelStatus | ⬜ Pending |
| 3. ChannelHandle class | New file; Pimpl; ZMQ socket ownership; send/recv; DataBlock integration | ⬜ Pending |
| 4. Messenger create/connect | `create_channel`, `connect_channel`, heartbeat timer, `on_channel_closing` | ⬜ Pending |
| 5. Bidir identity handshake | HELLO/HELLO_ACK on P2C ctrl socket | ⬜ Pending |
| 6. Tests | Unit + integration tests for each pattern and transport combination | ⬜ Pending |
