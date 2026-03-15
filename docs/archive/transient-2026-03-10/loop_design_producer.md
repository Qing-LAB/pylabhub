# Producer Role Design

**Status**: ✅ Fully implemented 2026-03-09. Archived 2026-03-10 → see `docs/archive/transient-2026-03-10/`.
**Canonical reference**: HEP-CORE-0018 §0/§5/§6/§7 (all content merged there).
**Scope**: Responsibilities, thread model, unified node concept, inbox facility,
loop design, prohibited combinations, directory structure.

---

## 1. Producer Responsibilities

The producer is a **single-output data source**. It owns one data channel and
drives the data plane for all consumers of that channel.

### 1.1 Single Output Channel
A producer connects to **exactly one** data node:
- `transport=shm` → a `hub::ShmQueue` backed by a shared-memory DataBlock
- `transport=zmq` → a `hub::ZmqQueue` backed by a ZMQ PUSH socket

The hub registers the node (SHM secret or ZMQ endpoint) in the channel registry.
Consumers discover the node via `CONSUMER_REG_ACK`. Cross-transport bridging
requires a processor; the producer itself never bridges.

### 1.2 Data Production
The producer runs a **time-driven loop** (`loop_thread_`):
- Calls `queue->write_acquire(timeout_ms)` → gets a typed write slot
- Calls `on_produce(out_slot, fz, msgs, api)` to fill the slot
- Calls `queue->write_commit()` to publish

The role **never manages queue internals** — ring buffer slots (SHM) and
send buffer entries (ZmqQueue) are the queue's concern, not the role's.

### 1.3 Messaging — Inbox Facility
Each role optionally declares an **inbox**: a ZMQ ROUTER socket that accepts
typed, schema-validated messages from any DEALER sender (any other role).

- The inbox schema is declared in `producer.json` as `inbox_schema`.
- The inbox endpoint + schema are registered with the broker at `REG_REQ`.
- Any role can discover and send to the producer's inbox via `open_inbox(uid)`.
- Messages are dispatched via `on_inbox(msg_slot, sender_uid, api)` on `inbox_thread_`.

This replaces the old direct P2P ROUTER↔DEALER ctrl channel between producers
and consumers. **All messaging now goes through broker-registered inbox endpoints.**

### 1.4 Control Plane (ctrl_thread_)
A dedicated `ctrl_thread_` manages:
- Broker connection (DEALER socket to BrokerService)
- Heartbeat (`HEARTBEAT_REQ` periodically)
- Incoming broker events: `CONSUMER_JOINED`, `CONSUMER_LEFT`, `CONSUMER_DIED`,
  `CHANNEL_CLOSING_NOTIFY`, `FORCE_SHUTDOWN`, `CHANNEL_BROADCAST_NOTIFY`
- Outgoing control: `CHANNEL_NOTIFY_REQ` (broadcast to channel), metrics reports
- Inbox endpoint registration (carried in `REG_REQ` at startup)

`ctrl_thread_` is the rename of the former `run_zmq_thread_()`. The name
`ctrl_thread_` clarifies this thread is for broker communication and control
messaging — not data transfer.

---

## 2. Thread Model

The producer runs **three concurrent threads**:

```
Main thread (producer_main.cpp)
  ├── loop_thread_    ← data production loop
  │     write_acquire() → on_produce() → write_commit()
  │     Time-driven: target_period_ms + LoopTimingPolicy
  │     GIL held only during on_produce() call
  │     GIL released during write_acquire() / sleep
  │
  ├── ctrl_thread_    ← broker control + messaging
  │     ZmqPollLoop: DEALER socket to BrokerService
  │     Heartbeat, CONSUMER_JOINED/LEFT/DIED, CHANNEL_CLOSING_NOTIFY, FORCE_SHUTDOWN
  │     Enqueues events for loop_thread_
  │
  └── inbox_thread_   ← per-role inbox (optional, when inbox_schema defined)
        ZMQ ROUTER socket (binds to inbox_endpoint)
        Recv: msgpack-validated frame → on_inbox(msg_slot, sender_uid, api)
        Send: ACK with error code back to sender's DEALER identity
        GIL held only during on_inbox() call
```

**Key invariant**: The Python GIL is held by at most one thread at a time.
`loop_thread_` and `inbox_thread_` interleave via GIL acquire/release.
`ctrl_thread_` never holds the GIL.

---

## 3. Unified Node Concept

SHM and ZmqQueue are **equal queue implementations** under `hub::QueueWriter`
(the write-side abstract class that replaces the old combined `hub::Queue`):

| Property           | SHM (ShmQueue)                        | ZMQ (ZmqQueue)                        |
|--------------------|---------------------------------------|---------------------------------------|
| Backing store      | Shared memory DataBlock               | ZMQ PUSH socket + send buffer         |
| Schema enforcement | BLDS hash at `CONSUMER_REG_REQ`       | msgpack frame validation per message  |
| Flexzone           | Yes (zero-copy R/W live view)         | No (fz=None in scripts; by design)    |
| Ring buffer        | Yes (slot_count slots)                | Internal send buffer (zmq_buffer_depth)|
| Back-pressure      | `write_acquire()` blocks/times out    | `write_acquire()` immediate; overflow by policy |
| Spinlocks          | Yes (data slots + control zone)       | No                                    |
| Checksum (write)   | BLAKE2b per-slot via `set_checksum_options()` | No-op (TCP handles integrity)  |
| Metrics            | SHM overrun/underrun + consumer lag   | send_drop_count, send_nack_count, recv_overflow_count, recv_frame_error_count, recv_gap_count |
| Discovery          | Broker stores secret; consumer attaches| Broker stores endpoint; consumer connects |

**QueueWriter API** (write side only — producer cannot call read methods):
```cpp
queue_->write_acquire(timeout_ms)   // get write slot (blocks or returns null by policy)
queue_->write_commit()              // publish
queue_->write_discard()             // abandon slot without publishing
queue_->metrics()                   // QueueMetrics snapshot
queue_->capacity()                  // ring/send buffer depth
queue_->policy_info()               // overflow policy description
```

The role loop is identical regardless of transport:
```cpp
void run_loop_() {
    while (!stop_requested_) {
        auto slot = queue_->write_acquire(config_.timeout_ms);
        if (!slot) { handle_timeout_(); continue; }
        call_on_produce_(*slot, fz_, msgs_);
        queue_->write_commit();
        sleep_until_next_deadline_();
    }
}
```

**Note on hub::Queue elimination**: The old combined `hub::Queue` class (with both read
and write methods) is replaced by two independent abstract classes: `hub::QueueReader`
(consumer / processor input) and `hub::QueueWriter` (producer / processor output).
`ShmQueue` and `ZmqQueue` inherit from both but are internal only. Public API exposes
only `unique_ptr<QueueWriter>` (producer) or `unique_ptr<QueueReader>` (consumer).
See `docs/tech_draft/loop_design_consumer.md §3` for the full design.

---

## 4. Loop Design (loop_thread_)

### 4.1 Time-Driven Loop
```
loop:
    deadline = now + target_period_ms        // set at start of cycle
    slot = queue->write_acquire(timeout_ms)
    if slot is null:
        on_produce_timeout(api)
        update_deadline(LoopTimingPolicy)
        continue
    call on_produce(out_slot, fz, msgs, api)
    queue->write_commit()
    sleep until deadline
    update_deadline(LoopTimingPolicy)
```

When `target_period_ms == 0` (free-run): no sleep, produce as fast as queue allows.

### 4.2 LoopTimingPolicy

| Policy       | Deadline advance on overrun                                    |
|--------------|----------------------------------------------------------------|
| FixedPace    | `next = now + target_period_ms` — no catch-up, safe default   |
| Compensating | `next = prev_deadline + target_period_ms` — maintains average rate |

Applied to both SHM and ZmqQueue transports (same loop code).

### 4.3 Queue Write Path (SHM)
`write_acquire()` → DataBlock shared-memory spinlock → returns slot pointer.
`write_commit()` → release spinlock + advance write head.

### 4.4 Queue Write Path (ZmqQueue PUSH)
`write_acquire()` → always returns immediately (internal buffer slot).
`write_commit()` → enqueues to `send_buffer_` (bounded FIFO, depth=`zmq_buffer_depth`).
`send_thread_` → dequeues head → `zmq_send()` → waits for ACK.

**ACK and retry**:
- Receiver's ROUTER sends ACK (error code 0 = OK, non-zero = queue overflow etc.)
  back to sender's DEALER identity on the same socket pair.
- On ACK fail: retain head in buffer, retry after `zmq_retry_interval_ms`
  (default: `target_period_ms`, inherits from role config).
- Retry count: unlimited by default (configurable).
- On `send_buffer_` full: `OverflowPolicy::Drop` (drop new) or `Block` (stall `write_commit()`).

**send_thread_ does not block `loop_thread_`**. Role sees only `write_acquire()`/`write_commit()`.

---

## 5. Inbox Design (inbox_thread_)

### 5.1 Ownership
The **receiver** (the role declaring the inbox) owns all inbox config:
- `inbox_schema` — field list (same JSON format as `slot_schema`)
- `inbox_buffer_depth` — recv buffer size (default 64)
- `inbox_overflow_policy` — Drop or Block (default Drop)
- `inbox_endpoint` — bound ZMQ ROUTER endpoint (auto-assigned or configured)

This is symmetric with SHM: the producer owns SHM config (slot_count, secret).
Senders discover inbox config from the broker — they do not set it.

### 5.2 Registration
At `REG_REQ`, the producer registers:
```
inbox_endpoint  "tcp://127.0.0.1:5592"   // ROUTER binds here
inbox_schema    <ZmqSchemaField list>    // validated by broker store
```
The broker stores these per-role alongside the data node info.

### 5.3 Sender Discovery
Any role calls `api.open_inbox(target_uid)` → framework sends
`ROLE_INFO_REQ{target_uid}` to broker → receives `ROLE_INFO_ACK{inbox_endpoint, inbox_schema}`.
Framework creates a `ZmqQueue::push_to(inbox_endpoint, schema)` DEALER connection.
Returns a handle typed to the inbox schema.

### 5.4 ACK Protocol
The ROUTER/DEALER pair is inherently bidirectional. After the receiver processes
a message:
- Send ACK frame back to sender's ZMQ identity: `[error_code: uint8]`
  (0=OK, 1=queue_overflow, 2=schema_error, 3=handler_error)
- This happens at the framework level; the script sees only `on_inbox(msg_slot, sender_uid, api)`.

### 5.5 Offline Detection
No push notification from broker when a sender goes offline. The `send_thread_`
uses timeout-driven `ROLE_PRESENCE_REQ` to broker on repeated ACK failures:
```
[ACK timeout] → send ROLE_PRESENCE_REQ{sender_uid} → broker replies yes/no
              → no: close DEALER, evict from send cache
```

### 5.6 Script API

**Receiver side** (the role that declared the inbox):
```python
def on_inbox(msg_slot, sender_uid, api):
    # msg_slot is typed ctypes struct; schema enforced by framework before callback
    api.log(f"inbox from {sender_uid}: value={msg_slot.value}")
```

**Sender side** (any other role sending to a target's inbox):
```python
# api.open_inbox(target_uid) → InboxHandle or None
#   - Returns None if the target is not currently online
#   - Returns a cached InboxHandle on subsequent calls (lazy discovery)
#   - Cache is cleared on role restart (clear_inbox_cache() called automatically)
#
# InboxHandle methods:
#   handle.acquire()              → writable ctypes slot view (fill before send)
#   handle.send(timeout_ms=5000) → int: 0=OK, non-zero=error code (GIL released during ACK wait)
#   handle.discard()              → abandon current acquired slot without sending
#   handle.is_ready()             → bool: True if connected and running
#   handle.close()                → disconnect and invalidate

# api.wait_for_role(uid, timeout_ms=5000) → bool
#   Polls broker for role presence until online or deadline.
#   GIL released between polls. Returns True if online, False if timed out.

def on_produce(out_slot, fz, msgs, api):
    handle = api.open_inbox("CONS-LOGGER-12345678")  # cached after first call
    if handle is None:
        return False    # target not online — discard this slot, loop continues

    slot = handle.acquire()
    slot.value = out_slot.value * 2
    err = handle.send(timeout_ms=1000)
    if err != 0:
        api.log(f"inbox send failed: {err}", level="warn")

    out_slot.ts = time.monotonic()
    out_slot.value = compute_value()
    return True   # or None — commits out_slot to ring
```

**`on_produce` return value contract** (critical — determines slot fate):
```
Return True or None  → write_commit(): slot is published to the ring
Return False         → write_discard(): slot is discarded, loop_thread_ continues normally
Wrong type           → write_discard() + LOGGER_ERROR: framework logs type error, loop continues
```
"Discard" means the write slot is abandoned for this cycle. It does NOT stop the loop.
The most common use of `return False` is when a prerequisite is not ready (e.g., target
inbox not online) and you want to skip producing this cycle.

**`check_readiness()` pattern** (recommended for complex dependency chains):
```python
# Module-level state — persists across on_produce() calls
_inbox_handle = None
_target_uid = "CONS-LOGGER-12345678"

def check_readiness(api):
    """Idempotent: try to open inbox if not already open. Returns True if ready."""
    global _inbox_handle
    if _inbox_handle is not None and _inbox_handle.is_ready():
        return True
    _inbox_handle = api.open_inbox(_target_uid)
    if _inbox_handle is None:
        api.log(f"target {_target_uid} not online yet", level="debug")
    return _inbox_handle is not None

def on_produce(out_slot, fz, msgs, api):
    if not check_readiness(api):
        return False   # not ready — discard this cycle, try again next iteration

    # ... fill out_slot and send inbox message ...
    return True
```

Notes on `api.state`:
- `api` does not support arbitrary attribute assignment (pybind11 classes are not Python dicts).
- Use **Python module-level variables** for persistent state across callbacks (as shown above).

### 5.7 Inbox Discovery Sequence

```
Sender calls api.open_inbox(target_uid)
  → Messenger sends ROLE_INFO_REQ{target_uid} to broker
  → Broker replies ROLE_INFO_ACK{inbox_endpoint, inbox_schema_json, inbox_packing}
  → Framework parses schema_json → SchemaSpec → ctypes type
  → InboxClient.connect_to(inbox_endpoint, sender_uid, schema, packing)
  → InboxClient.start()
  → Returns cached InboxHandle

If target is not online: broker replies with error → open_inbox() returns None

api.wait_for_role(uid, timeout_ms):
  → Polls ROLE_PRESENCE_REQ in a loop until online or deadline
  → GIL released between polls (200 ms poll interval)
  → Returns True if online, False if timed out
  → Use in on_init() when startup order is known; use lazy open_inbox() for dynamic topologies
```

---

## 6. ctrl_thread_ Design

Renamed from `run_zmq_thread_()`. Uses `scripting::ZmqPollLoop` + `HeartbeatTracker`.

```
loop:
    ZmqPollLoop.poll([
        peer_ctrl_socket  → handle_peer_ctrl_events()
    ])
    HeartbeatTracker.maybe_send_heartbeat()
    drain event queue → dispatch callbacks to loop_thread_
```

Handles:
- `CONSUMER_JOINED` → `api.on_consumer_joined(uid)`
- `CONSUMER_LEFT` / `CONSUMER_DIED` → `api.on_consumer_left(uid)` / `api.on_consumer_died(uid)`
- `CHANNEL_CLOSING_NOTIFY` → set `closing_` flag, loop_thread_ exits gracefully
- `FORCE_SHUTDOWN` → set `stop_requested_` immediately
- `CHANNEL_BROADCAST_NOTIFY` → enqueue as IncomingMessage for on_produce msgs list
- Metrics: `METRICS_REPORT_REQ` → broker stores snapshot

---

## 7. Design Gaps

### 7.1 Unified run_loop_() ✅ Done 2026-03-08
`ProducerScriptHost` now has a transport-agnostic `run_loop_()` using
`queue->write_acquire()`/`write_commit()`. SHM and ZMQ paths both use it.
Once `hub::QueueWriter` replaces `hub::Queue`, the `queue_` member type changes
from `hub::Queue*` to `hub::QueueWriter*` — no logic changes required.

### 7.2 InboxQueue Class ✅ Done 2026-03-08
`hub::InboxQueue` (ROUTER) + `hub::InboxClient` (DEALER) implemented in
`src/include/utils/hub_inbox_queue.hpp` + `src/utils/hub/hub_inbox_queue.cpp`.
`inbox_endpoint` and `inbox_schema` carried in `REG_REQ`. `inbox_thread_` runs
in all three script hosts.

### 7.3 Transport Field in ProducerConfig ✅ Done 2026-03-08
`Transport{Shm,Zmq}` enum + `zmq_out_endpoint`/`zmq_out_bind`/`zmq_buffer_depth`
fields added to `ProducerConfig`. Parsed and validated in `from_json_file()`.

### 7.4 ZmqQueue send_thread_ + ACK (Deferred)
Current ZmqQueue push path is fire-and-forget (no ACK, no retry, no send buffer).
Implementing send_thread_ + ACK + retry requires changing PUSH→DEALER socket type
(breaking wire protocol change). Deferred to a separate design discussion.

### 7.5 ROLE_PRESENCE_REQ / ROLE_INFO_REQ ✅ Done 2026-03-09
`query_role_presence(uid, timeout_ms)` and `query_role_info(uid, timeout_ms)` added
to `Messenger`. Broker handles both request types. Scripts use `api.wait_for_role()`
and `api.open_inbox()` (see §5.6).

### 7.6 schema_spec_to_zmq_fields() Location ✅ Done 2026-03-09
`schema_spec_to_zmq_fields()` extracted to shared inline in
`src/include/utils/script_host_helpers.hpp`. Local copies removed from all three
script hosts.

### 7.7 ctrl_thread_ Rename ✅ Done 2026-03-09
`zmq_thread_` → `ctrl_thread_`, `run_zmq_thread_()` → `run_ctrl_thread_()`
renamed in all three script hosts (producer, consumer, processor).

---

## 8. What Is Complete

| Feature | Status |
|---------|--------|
| SHM-backed data production | Done |
| ZmqQueue PUSH production (fire-and-forget) | Done (HEP-0021) |
| Broker registration (REG_REQ) | Done |
| Heartbeat + channel lifecycle | Done (HEP-0007) |
| LoopTimingPolicy (FixedPace, Compensating) | Done |
| Metrics plane | Done (HEP-0019) |
| Broadcast (CHANNEL_NOTIFY_REQ) | Done (HEP-0007) |
| Consumer messaging via CHANNEL_BROADCAST_NOTIFY | Done |
| Unified queue interface (hub::QueueReader/QueueWriter split) | Done ✅ 2026-03-09 |
| Unified run_loop_() in script host | Done ✅ 2026-03-08 |
| transport field in ProducerConfig | Done ✅ 2026-03-08 |
| ZmqQueue send_thread_ + ACK + retry | Deferred (PUSH→DEALER breaking change) |
| Per-role inbox (InboxQueue) | Done ✅ 2026-03-08 |
| ROLE_PRESENCE_REQ / ROLE_INFO_REQ protocol | Done ✅ 2026-03-09 |
| ctrl_thread_ rename | Done ✅ 2026-03-09 |
| script_spec_to_zmq_fields() shared | Done ✅ 2026-03-09 |
| api.open_inbox() / api.wait_for_role() | Done ✅ 2026-03-09 |
| write_discard() rename (abort→discard) | Done ✅ 2026-03-09 |
| Broker transport arbitration (TRANSPORT_MISMATCH) | Done ✅ 2026-03-09 |

---

## 9. Prohibited Combinations

| Combination | Severity | Reason |
|-------------|----------|--------|
| `transport=zmq` + `loop_driver=shm` on consumer | Hard error | No SHM block exists |
| `transport=shm` + `loop_driver=zmq` on consumer | Hard error | No ZMQ queue exists |
| Two producers on same channel | Hard error | 1-writer invariant |
| Schema hash mismatch (consumer vs producer) | Hard error | Data corruption |
| `inbox_schema` mismatch (sender vs receiver declaration) | Hard error | Frame rejection at connect time |
| `target_period_ms=0` (free-run) + ZmqQueue + slow consumer | Warning | Sender floods send buffer, high drop rate |
| `consumer.target_period_ms` << `producer.target_period_ms` | Warning | Consumer starved, CPU waste |

Hub enforces the transport/loop_driver check at `CONSUMER_REG_REQ` time
(see §5.1 in `loop_design_hub.md` — gap, not yet implemented).

---

## 10. Config Reference

### producer.json (current + proposed)
```json
{
  "producer": {
    "uid":       "PROD-TEMPSENS-12345678",
    "name":      "TempSensor",
    "log_level": "info",
    "auth": { "keyfile": "" }
  },

  "hub_dir": "/opt/pylabhub/hubs/lab",

  "channel":          "lab.sensors.temperature",
  "transport":        "shm",              // "shm" | "zmq" — implemented
  "zmq_out_endpoint": "tcp://0.0.0.0:5581", // required when transport=zmq — implemented
  "zmq_out_bind":     true,               // PUSH default=bind — implemented
  "zmq_buffer_depth": 64,                 // send buffer depth (>0) — implemented

  "target_period_ms": 100,
  "loop_timing":      "fixed_pace",       // "fixed_pace" | "compensating"
  "timeout_ms":       -1,

  "slot_schema":     { "fields": [{"name": "value", "type": "float32"}] },
  "flexzone_schema": null,                // only meaningful for transport=shm

  "inbox_schema":    { "fields": [{"name": "cmd", "type": "uint8"}, {"name": "arg", "type": "float32"}] },
  "inbox_endpoint":  "tcp://0.0.0.0:5592", // ROUTER binds — NEW (default: auto)
  "inbox_buffer_depth":     64,           // NEW
  "inbox_overflow_policy":  "drop",       // "drop" | "block" — NEW

  "shm": { "enabled": true, "secret": 0, "slot_count": 8 },

  "script": { "path": ".", "type": "python" },

  "validation": { "stop_on_script_error": false }
}
```

All `inbox_*` fields are implemented (2026-03-08/09).
`flexzone_schema` is ignored when `transport=zmq` (LOGGER_WARN emitted at startup).

---

*Next: `loop_design_consumer.md`*
