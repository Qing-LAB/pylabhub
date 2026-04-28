# HEP-CORE-0027: Inbox Messaging

**Status**: Implemented (documenting existing system)
**Created**: 2026-03-27
**Scope**: InboxQueue, InboxClient, peer-to-peer messaging side channel

---

## 1. Motivation

The main data queue (SHM or ZMQ) is a one-to-many broadcast channel: one producer writes,
multiple consumers read. It is optimized for high-throughput, unidirectional streaming.

However, roles often need to exchange small, targeted messages with specific peers:
- A consumer sending calibration parameters to a producer
- A processor requesting a configuration change from another processor
- An orchestrator coordinating startup sequence across roles

These use cases require:
- **Point-to-point** addressing (to a specific role, not broadcast)
- **Bidirectional** communication (request + acknowledgement)
- **Schema-validated** payloads (same typed slot model as the data queue)
- **Independence** from the data plane (inbox messages don't interfere with data flow)

The Inbox provides this as an optional side channel, available to all roles.

---

## 2. Architecture

### 2.1 Component Model

```
Role A (sender)                          Role B (receiver)
┌────────────────┐                       ┌────────────────────┐
│                │                       │                    │
│  InboxClient   │  ZMQ DEALER ────────► │  InboxQueue        │
│  (DEALER)      │  ◄──── ACK ───────── │  (ROUTER)          │
│                │                       │                    │
│  Acquired via  │                       │  Bound at startup  │
│  api.open_     │                       │  inbox_thread_     │
│  inbox(uid)    │                       │  receives + ACKs   │
└────────────────┘                       └────────────────────┘
        │                                         │
        │ ROLE_INFO_REQ                          │ REG_REQ
        │ (discover endpoint)                    │ (advertise endpoint)
        ▼                                         ▼
    ┌──────────┐                             ┌──────────┐
    │  Broker  │ ◄─── endpoint metadata ───► │  Broker  │
    └──────────┘                             └──────────┘
```

- **InboxQueue** (ROUTER): Binds a ZMQ ROUTER socket. Receives typed messages from
  any connected DEALER. Sends ACK after processing. One per role (optional).
- **InboxClient** (DEALER): Connects to a remote InboxQueue. Fills a typed buffer,
  sends, waits for ACK. Created on demand via `api.open_inbox(target_uid)`.
- **Broker**: Stores inbox metadata (endpoint, schema, packing) from REG_REQ.
  Serves it via ROLE_INFO_REQ. Not involved in actual message flow.

### 2.2 Key Design Decisions

| Decision | Rationale |
|----------|-----------|
| Direct P2P (no broker relay) | Low latency; broker is not a bottleneck for inbox traffic |
| ROUTER/DEALER (not PUB/SUB) | Need per-sender identity for ACK routing |
| Request/ACK (not fire-and-forget) | Sender knows if message was processed |
| Same wire format as ZmqQueue | Reuse msgpack encoding, schema validation, checksum |
| Independent from data queue | Inbox availability doesn't depend on data transport (SHM/ZMQ) |
| Optional per role | Not all roles need peer messaging; zero overhead if unconfigured |

### 2.3 Relationship to Other Components

| Component | Relationship |
|-----------|-------------|
| Data queue (SHM/ZMQ) | Orthogonal. Inbox is a separate ZMQ channel. Both can be active simultaneously. |
| Messenger | Discovery only. Messenger routes ROLE_INFO_REQ to broker; not involved in data flow. |
| Broker | Metadata storage only. Stores inbox_endpoint + schema from REG_REQ; serves via ROLE_INFO_REQ. |
| RoleHostCore | Inbox metrics will be exposed through RoleContext (see §8). |

---

## 3. Wire Protocol

Inbox uses the same msgpack fixarray[5] wire format as ZmqQueue:

```
[magic:uint32, schema_tag:bin8, seq:uint64, payload:array(N), checksum:bin32]
```

| Element | Type | Description |
|---------|------|-------------|
| magic | uint32 | `0x51484C50` — frame validation |
| schema_tag | bin8 | 8-byte schema hash — receiver validates match |
| seq | uint64 | Monotonic per-sender sequence number |
| payload | array(N) | Schema fields packed per `ZmqSchemaField` definitions |
| checksum | bin32 | BLAKE2b-256 of decoded payload data |

**Schema validation** is mandatory and always-on (same as ZmqQueue):
- Both InboxQueue and InboxClient require a non-empty `ZmqSchemaField` list at construction
- Field types, sizes, and counts are validated at factory time (non-empty, valid type strings,
  numeric counts >= 1, string/bytes lengths > 0)
- The schema tag (8-byte BLAKE2b hash of canonical field definitions) is computed at factory
  time from the `ZmqSchemaField` list and embedded in every frame. The receiver validates the
  tag matches its own computed tag; mismatched tags are rejected (increments `recv_frame_error_count`).
- Payload array size must match the receiver's schema field count; rejected if different

**Checksum verification** is mandatory and always-on (same as ZmqQueue):
- Sender computes BLAKE2b-256 over the raw payload data before msgpack packing
- Receiver verifies BLAKE2b-256 after msgpack decoding; frames with mismatched checksums
  are dropped and logged (increments `checksum_error_count`)
- Unlike SHM's `ChecksumPolicy` (None/Manual/Enforced), inbox has no policy toggle —
  checksum is always computed and verified

**ACK**: Single byte sent from ROUTER to DEALER after processing:
- `0` = OK
- `1` = queue overflow
- `2` = schema error
- `3` = handler error

The DEALER/ROUTER envelope uses ZMQ's built-in identity routing:
- DEALER sets `ZMQ_IDENTITY` to the sender's pylabhub UID before connecting
- ROUTER receives `[identity, empty_delimiter, payload]`
- ROUTER sends ACK as `[identity, empty_delimiter, ack_byte]`

---

## 4. Data Flow

### 4.1 Receiver Setup (role host startup)

```
1. Role host reads inbox config (schema, endpoint, buffer_depth, packing)
2. InboxQueue::bind_at(endpoint, schema_fields, packing, rcvhwm) → unique_ptr
3. inbox_queue_->start()                    — bind ROUTER socket
4. inbox_queue_->set_checksum_policy(config_.checksum().policy)
5. inbox_queue_->actual_endpoint()          — resolve port-0 if used
6. Inbox info included in registration:
   - Producer/Processor: ProducerOptions → REG_REQ
   - Consumer: ConsumerOptions → CONSUMER_REG_REQ
   Fields: inbox_endpoint, inbox_schema_json, inbox_packing, inbox_checksum
7. Broker stores inbox info on ChannelEntry (producer) or ConsumerEntry (consumer)
8. inbox_thread_ started: loop { recv_one() → invoke_on_inbox() → send_ack() }
```

### 4.2 Sender Connection (on demand)

```
1. Script calls api.open_inbox("TARGET-UID-1234")
2. ScriptEngine → RoleHostCore::open_inbox() (cached, thread-safe)
3. Messenger sends ROLE_INFO_REQ to broker
4. Broker searches producer entries (by producer_role_uid), then consumer entries
   (by role_uid). Returns first match.
5. ROLE_INFO_ACK: inbox_endpoint, inbox_schema, inbox_packing, inbox_checksum
6. InboxClient::connect_to(endpoint, my_uid, schema, packing) → shared_ptr
7. client->start()                          — connect DEALER socket
8. client->set_checksum_policy(owner's policy from inbox_checksum)
   The inbox OWNER dictates the checksum policy. The sender adopts it.
9. InboxHandle wraps client for script use
```

### 4.3 Message Exchange

```
Sender (InboxClient):                    Receiver (InboxQueue):
  buf = client->acquire()                  item = inbox_queue_->recv_one(timeout)
  // fill buf with typed fields            // item->data = decoded payload
  ack = client->send(timeout)              // item->sender_id = sender UID
  // ack == 0 means OK                     // item->seq = sequence number
                                           // engine->invoke_on_inbox(...)
                                           inbox_queue_->send_ack(0)
```

### 4.4 Shutdown

```
1. Role host signals inbox_thread_ to stop
2. inbox_queue_->stop()                    — closes ROUTER socket
3. Connected InboxClients get ZMQ disconnect; send() returns 255
4. InboxClient::stop() called in RoleHostCore cache cleanup
```

---

## 5. Threading Model

```
                          Role Host Process
┌─────────────────────────────────────────────────────────────┐
│                                                             │
│  main_thread_ (data loop)                                   │
│  ├─ drain_inbox_sync(inbox_queue_)                          │
│  │   ├─ inbox_queue_->recv_one(0ms)  [non-blocking poll]    │
│  │   ├─ engine->invoke_on_inbox(data, size, sender_id)      │
│  │   └─ inbox_queue_->send_ack(code)                        │
│  └─ [continue with on_produce/on_consume/on_process]        │
│                                                             │
│  ctrl_thread_ (broker protocol)                             │
│  └─ [heartbeat, ctrl messages — no inbox interaction]       │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

**Threading contract:**
- `recv_one()` and `send_ack()` MUST be called from the same thread (ZMQ ROUTER
  socket is not thread-safe)
- Currently called from the main data loop via `drain_inbox_sync()` (non-blocking
  poll before each data callback)
- InboxClient is single-threaded per instance (one client per target, used from
  the script thread via InboxHandle)

---

## 6. Configuration

Inbox configuration is specified as flat top-level keys in the role's JSON config:

```json
{
    "inbox_schema": {
        "fields": [
            {"name": "cmd", "type": "int32"},
            {"name": "value", "type": "float64"}
        ]
    },
    "inbox_endpoint": "tcp://0.0.0.0:0",
    "inbox_buffer_depth": 64,
    "inbox_overflow_policy": "drop",
    "inbox_zmq_packing": "aligned"
}
```

| Field | Required | Default | Description |
|-------|----------|---------|-------------|
| `inbox_schema` | yes (to enable inbox) | — | Slot layout for inbox messages |
| `inbox_endpoint` | yes (if inbox) | — | ZMQ ROUTER bind endpoint. Port 0 = OS-assigned. |
| `inbox_buffer_depth` | no | `64` | ZMQ RCVHWM. 0 = unlimited. |
| `inbox_overflow_policy` | no | `"drop"` | `"drop"` (finite HWM) or `"block"` (unlimited HWM) |
| `inbox_zmq_packing` | no | `"aligned"` | `"aligned"` (C-struct natural) or `"packed"` (no padding) |

When `inbox_schema` is absent or empty, no inbox is created. The role operates
without peer messaging capability.

---

## 7. Script API

### 7.1 Receiving (on_inbox callback)

```python
def on_inbox(slot, sender, api):
    """Called once per inbox message, before the main data callback.

    Args:
        slot:   ctypes struct view of the decoded payload (schema-typed)
        sender: str — pylabhub UID of the sender
        api:    role API object (ProducerAPI/ConsumerAPI/ProcessorAPI)
    """
    if slot.cmd == 1:
        api.log(f"Received command from {sender}: value={slot.value}")
```

### 7.2 Sending (InboxHandle)

```python
handle = api.open_inbox("PROD-SENSOR-A1B2C3D4")
if handle is None:
    api.log("Target offline or has no inbox")
    return

handle.acquire()           # populate buffer from schema
handle.slot.cmd = 1
handle.slot.value = 3.14
ack = handle.send(1000)    # send with 1s ACK timeout
if ack == 0:
    api.log("Message delivered")
```

### 7.3 InboxHandle Methods

| Method | Returns | Description |
|--------|---------|-------------|
| `acquire()` | ctypes slot | Get write buffer (zeroed); fill fields before send |
| `send(timeout_ms)` | int | Send + wait for ACK. 0=OK, 255=timeout/error |
| `discard()` | None | Discard buffer without sending |
| `is_ready()` | bool | True if client is connected |
| `close()` | None | Disconnect client |

### 7.4 InboxHandle Caching

`api.open_inbox(uid)` is cached per (role, target_uid):
- First call: broker round-trip (ROLE_INFO_REQ) → create InboxClient → cache
- Subsequent calls: return cached handle
- Cache is per-RoleHostCore (shared across script reloads)
- `api.clear_inbox_cache()` forces fresh broker lookups

---

## 8. Metrics

InboxQueue exposes four diagnostic counters:

| Metric | Description |
|--------|-------------|
| `recv_frame_error_count` | Frames rejected: bad magic, schema tag mismatch, field type/size error |
| `ack_send_error_count` | ZMQ send errors when sending ACK response |
| `recv_gap_count` | Sequence number gaps (per-sender tracking; indicates dropped frames) |
| `checksum_error_count` | BLAKE2b verification failures after successful decode |

These are available via:
- `InboxQueue::inbox_metrics()` → `InboxMetricsSnapshot` (C++ struct)
- `PYLABHUB_INBOX_METRICS_FIELDS` X-macro for serialization (see HEP-0008 §6.1)
- Hierarchical metrics output under `"inbox"` key (see HEP-0008 §6.1, HEP-0019 §5.4)

InboxQueue is exposed to the script engine via `RoleContext::inbox_queue` pointer
(non-owning; role host retains ownership). The pointer is set during role setup
and is nullptr when no inbox is configured.

---

## 9. Common Use Cases

### 9.1 Parameter Update

A control role sends updated calibration parameters to a running producer:

```python
# Controller (consumer with inbox client):
handle = api.open_inbox("PROD-SENSOR-A1")
handle.acquire()
handle.slot.param_id = 42
handle.slot.new_value = 1.234
handle.send(1000)

# Producer (on_inbox handler):
def on_inbox(slot, sender, api):
    update_calibration(slot.param_id, slot.new_value)
    api.log(f"Param {slot.param_id} updated by {sender}")
```

### 9.2 Coordination Signal

A processor signals readiness to a downstream consumer:

```python
# Processor:
handle = api.open_inbox("CONS-DISPLAY-B2")
handle.acquire()
handle.slot.signal = READY_SIGNAL
handle.send(500)

# Consumer (on_inbox handler):
def on_inbox(slot, sender, api):
    if slot.signal == READY_SIGNAL:
        api.log(f"Upstream {sender} ready")
```

### 9.3 Request/Response

A monitoring role queries status from another role and reads the ACK:

```python
handle = api.open_inbox("PROC-FILTER-C3")
handle.acquire()
handle.slot.request_type = STATUS_QUERY
ack = handle.send(2000)
if ack == 0:
    api.log("Status query acknowledged")
elif ack == 255:
    api.log("Target did not respond in time")
```

---

## 10. Source File Reference

| Component | File | Description |
|-----------|------|-------------|
| InboxQueue | `src/include/utils/hub_inbox_queue.hpp` | ROUTER receiver API |
| InboxClient | `src/include/utils/hub_inbox_queue.hpp` | DEALER sender API |
| Implementation | `src/utils/hub/hub_inbox_queue.cpp` | ZMQ + msgpack + checksum |
| Wire helpers | `src/utils/hub/zmq_wire_helpers.hpp` | Shared msgpack pack/unpack |
| Script handle | `src/scripting/python_helpers.hpp` | `InboxHandle` wrapper |
| Drain helper | `src/scripting/role_host_helpers.hpp` | `drain_inbox_sync()` |
| Discovery | `src/include/utils/messenger.hpp` | `query_role_info()` for ROLE_INFO_REQ |
| Protocol | HEP-CORE-0007 §12.4 | ROLE_INFO_REQ/ACK message format |
| Metrics X-macro | `src/include/utils/hub_inbox_queue.hpp` | `PYLABHUB_INBOX_METRICS_FIELDS` |
| Metrics adapters | `metrics_json.hpp`, `metrics_pydict.hpp`, `metrics_lua.hpp` | Serialization helpers |
| Tests | `tests/test_layer3_datahub/test_datahub_hub_inbox_queue.cpp` | 11 L3 tests |

---

## 11. Cross-References

- **HEP-CORE-0007 §12.4**: ROLE_INFO_REQ/ACK protocol for inbox endpoint discovery
- **HEP-CORE-0008 §6.1**: Hierarchical metrics schema (inbox group)
- **HEP-CORE-0015 §4, §6.4**: Processor inbox config fields + InboxHandle API
- **HEP-CORE-0018 §15.6**: Inbox plane overview (superseded by this document for details)
- **HEP-CORE-0019 §5.4**: Metrics serialization architecture
- **HEP-CORE-0034 §11.4**: Inbox schemas integrate into the hub's owner-authoritative
  schema registry as `(receiver_uid, "inbox")` records. Receiver-as-authority model
  (this HEP §4.1 step 7-8) maps directly onto HEP-0034 ownership rules; existing wire
  fields (`inbox_endpoint`, `inbox_schema_json`, `inbox_packing`, `inbox_checksum`) are
  retained, with broker-side storage unified into `HubState.schemas`. The broker
  computes the SchemaRecord fingerprint using the same canonical bytes as
  `compute_inbox_schema_tag` so `SchemaRecord.hash[0..7] == wire schema_tag`. Inbox
  records cascade-evict via `_on_channel_closed` when the producer's data channel
  closes (DEREG_REQ or heartbeat timeout) — see HEP-0034 §7.2.
