# Consumer Role Design

**Status**: ✅ Fully implemented 2026-03-10. Consumer inbox_thread_ done; 9 ShmQueue L3 tests added → 996/996. Archived 2026-03-10 → see `docs/archive/transient-2026-03-10/`. Canonical reference: HEP-CORE-0018.
**Scope**: Responsibilities, thread model, QueueReader abstraction, loop design,
slot access, flexzone/spinlock, checksum policy, implementation gaps.

---

## 1. Consumer Responsibilities

The consumer is a **single-input data sink**. It subscribes to one data channel,
reads slots produced by the upstream producer, and drives user script execution
for each received slot.

### 1.1 Single Input Channel
A consumer connects to **exactly one** data node:
- `transport=shm` → a `hub::ShmQueue` (read side) backed by the producer's shared-memory DataBlock
- `transport=zmq` → a `hub::ZmqQueue` (read side, PULL socket) backed by a ZMQ endpoint
  registered in the broker registry (HEP-0021)

The hub broker arbitrates the connection. For SHM, the broker provides the SHM secret so
the consumer can attach. For ZMQ, the broker provides the producer's endpoint; the
consumer connects directly (the hub is not in the data path).

### 1.2 Data Consumption
The consumer runs a **demand-driven loop** (`loop_thread_`):
- Calls `reader->read_acquire(timeout_ms)` → gets a read-only zero-copy slot
- Calls `on_consume(in_slot, fz, msgs, api)` with the slot
- Calls `reader->read_release()` after `on_consume` returns

The GIL is held only during `on_consume()`. The slot remains valid (zero-copy) for
the entire duration of the `on_consume` callback; it is released immediately after.

### 1.3 Flexzone Access
When a flexzone is configured (SHM transport only):
- Consumer gets a **writable** zero-copy view of the producer's shared flexzone memory.
- This is intentional: the flexzone is a user-coordinated shared region. Both producer
  and consumer can read/write it using the spinlock API to coordinate.
- ZmqQueue has no flexzone (always `None` in scripts; not a bug, by design).

### 1.4 Spinlock Access
Spinlocks provide user-level mutual exclusion over named regions of the flexzone.
- Producer exposes `api.spinlock(idx)` and `api.spinlock_count()` — already implemented.
- Consumer does NOT currently expose spinlock — this is a gap (see §8.5).

### 1.5 Control Plane (ctrl_thread_)
A dedicated `ctrl_thread_` manages the broker connection:
- `CONSUMER_REG_REQ` → `DISC_ACK` handshake (SHM secret or ZMQ endpoint)
- Heartbeat (`HEARTBEAT_REQ` periodically)
- Incoming: `CHANNEL_CLOSING_NOTIFY`, `FORCE_SHUTDOWN`, `CHANNEL_BROADCAST_NOTIFY`
- Outgoing: `CONSUMER_DEREG_REQ` on clean exit
- `ctrl_thread_` is always active regardless of data transport (SHM or ZMQ).

---

## 2. Thread Model

The consumer runs **three concurrent threads** (four when inbox is configured):

```
Main thread (consumer_main.cpp)
  ├── loop_thread_    ← demand-driven consumption loop
  │     read_acquire(timeout) → on_consume() → read_release()
  │     GIL held only during on_consume() call
  │     GIL released during read_acquire() / timeout sleep
  │
  ├── ctrl_thread_    ← broker control + messaging
  │     ZmqPollLoop: DEALER socket to BrokerService
  │     DISC_ACK, CHANNEL_CLOSING_NOTIFY, FORCE_SHUTDOWN, heartbeat
  │     Enqueues events for loop_thread_
  │
  └── inbox_thread_   ← per-role inbox (optional, when inbox_schema defined)
        ZMQ ROUTER socket
        Recv: msgpack-validated frame → on_inbox(msg_slot, sender_uid, api)
        GIL held only during on_inbox() call
```

**Key invariant**: The Python GIL is held by at most one thread at a time.
`loop_thread_` and `inbox_thread_` interleave via GIL acquire/release.
`ctrl_thread_` never holds the GIL.

---

## 3. QueueReader / QueueWriter Abstraction

### 3.1 Design Rationale

The current `hub::Queue` exposes **both** read and write APIs in a single abstract class.
This means `hub::ShmQueue` and `hub::ZmqQueue` each implement all 9 virtual methods
(read_acquire, read_release, read_flexzone, write_acquire, write_commit, write_discard,
write_flexzone, plus metadata). There is no enforcement that a consumer cannot accidentally
call write methods on its queue, or that a producer cannot call read methods.

The redesign splits `hub::Queue` into two independent abstract classes:

```
hub::QueueReader  — read_acquire, read_release, read_flexzone, last_seq,
                    capacity, policy_info, set_verify_checksum,
                    item_size, flexzone_size, name, metrics, start, stop, is_running

hub::QueueWriter  — write_acquire, write_commit, write_discard, write_flexzone,
                    capacity, policy_info, set_checksum_options,
                    item_size, flexzone_size, name, metrics, start, stop, is_running
```

Concrete classes use C++ multiple inheritance:
```cpp
// Internal only — not exposed in public headers
class ShmQueue : public QueueReader, public QueueWriter { ... };
class ZmqQueue : public QueueReader, public QueueWriter { ... };
```

The metadata methods (item_size, flexzone_size, name, metrics, start, stop, is_running,
capacity, policy_info) have identical signatures in both abstract classes. In the
concrete classes they are implemented once. No runtime cost: the vtable entries for
`QueueReader::item_size()` and `QueueWriter::item_size()` on a ShmQueue object both
resolve to the same ShmQueue implementation.

**Hub::Queue is eliminated.** The old combined abstract class is removed entirely.

Public API factories return `unique_ptr<QueueReader>` or `unique_ptr<QueueWriter>`.
`ShmQueue` and `ZmqQueue` are internal implementation details; callers see only the
access-mode-appropriate interface.

### 3.2 Why Multiple Inheritance (not a QueueBase helper class)?

A QueueBase would require the metadata methods to be pure virtual there and overridden
once in ShmQueue/ZmqQueue — identical to what multiple inheritance achieves, but with
an extra level of indirection in the hierarchy and an additional name that pollutes
the public API. Multiple inheritance of two independent interfaces with the same method
signatures is a standard C++ idiom for this pattern. There is no diamond problem because
neither QueueReader nor QueueWriter inherits from a common base.

### 3.3 Consumer Sees Only QueueReader

The consumer script host holds:
```cpp
std::unique_ptr<hub::QueueReader> reader_;
```

Created after `DISC_ACK`:
```cpp
// SHM transport:
reader_ = hub::ShmQueue::from_consumer_ref(*in_consumer_->shm(), item_size, fz_size, channel);

// ZMQ transport:
reader_ = hub::ZmqQueue::pull_from(endpoint, schema, packing, /*bind=*/false, buffer_depth);
```

`hub::Consumer::queue_reader()` returns `hub::QueueReader*` (non-owning) — same pattern
as `hub::Producer::queue_writer()` returning `hub::QueueWriter*` (future).

### 3.4 No Runtime Cost

A call to `reader_->read_acquire(timeout)` resolves directly to ShmQueue's or ZmqQueue's
implementation — one virtual dispatch at the same cost as the current `Queue*` calls.
There is no extra wrapper, no extra indirection, no heap allocation beyond the initial
factory call.

---

## 4. Loop Design (loop_thread_)

### 4.1 Demand-Driven Loop (Unified)

After the refactoring, both SHM and ZMQ transports use the same loop:

```
loop:
    slot = reader->read_acquire(timeout_ms)   // blocks until data or timeout
    acquire GIL
    if slot is null:                          // timeout
        on_consume(None, fz, msgs, api)
    else:
        on_consume(in_slot_view, fz, msgs, api)
    release GIL
    reader->read_release()                    // release slot (no-op if slot was null)
```

There is no sleep in the loop: `read_acquire()` is the blocking primitive. The loop
is demand-driven (not time-driven). This means no LoopTimingPolicy and no overrun counter
on the consumer side — the producer controls the rate.

**Current implementation** has two separate paths:
- `run_loop_shm_()` — calls `in_consumer_->shm()->acquire_consume_slot()` directly
- `run_loop_zmq_()` — calls `in_consumer_->queue()->read_acquire()` via `Queue*`

After refactoring to QueueReader, these are unified into a single `run_loop_()` that
calls `reader_->read_acquire()` regardless of transport (same as producer's unified loop).

### 4.2 Timeout Behaviour

When `read_acquire()` times out (returns `nullptr`):
- `on_consume(None, fz, msgs, api)` is called — gives script a chance to log or act
- `read_release()` is a no-op on null
- Loop continues immediately

`timeout_ms` from consumer.json controls how long to wait per cycle.
Set to a large value (e.g., 30000) to fire a timeout callback only on stale channels.

### 4.3 Queue Read Path (SHM)

`read_acquire(timeout_ms)` → `DataBlockConsumer::acquire_consume_slot(timeout_ms)` →
`SlotConsumeHandle`. Returns `buf.data()` (raw slot bytes).

`read_release()` → `SlotConsumeHandle::release()` — releases slot back to ring buffer.
The slot_id (commit_index) is captured at acquire time and stored for `last_seq()`.

**Multiple consumers**: SHM ring buffer natively supports N consumers simultaneously.
Each consumer has its own `DataBlockConsumer` instance tracking its own read position.
`ConsumerSyncPolicy` controls slot reuse timing.

### 4.4 Queue Read Path (ZmqQueue PULL)

`read_acquire(timeout_ms)` → waits on internal recv ring (filled by `recv_thread_`).
Returns pointer to decoded frame payload (msgpack-decoded slot bytes).

`read_release()` → frees the ring slot so `recv_thread_` can reuse it.

The seq field from the ZMQ frame header is stored for `last_seq()`.

---

## 5. Slot Access Design

### 5.1 Zero-Copy via from_buffer()

The slot pointer returned by `read_acquire()` points directly into:
- SHM transport: the SHM-mapped ring slot (shared with the producer's address space)
- ZMQ transport: the internal recv-ring buffer (owned by ZmqQueue's `recv_thread_`)

The Python slot view is created using `memoryview(ptr, size, readonly=False)` +
`from_buffer()` on the ctypes struct type. This is zero-copy — no bytes are duplicated.

The slot view is valid for the duration of `on_consume()`. After the callback
returns, `read_release()` is called and the slot memory may be reused. Scripts
must NOT store the slot object across callbacks (the slot is a live view, not a snapshot).

### 5.2 Read-Only Guard (__setattr__ override)

Consumer `in_slot` must not be writable by script. The guard is applied at the Python
level via `wrap_as_readonly_ctypes()` in `script_host_helpers.hpp`:

```python
# Creates a new ctypes type "SlotFrameReadonly" that inherits from the real type
# and overrides __setattr__ to raise AttributeError on any field write.
in_slot.ts              # OK — reads directly from SHM bytes (zero-copy)
in_slot.value = 42      # raises AttributeError: read-only slot: field 'value' cannot be written
```

This is enforced at construction time in `build_role_types()` via `build_schema_type_(..., readonly=True)`.
The ctypes struct instance itself is created with `from_buffer()` (zero-copy writable mmap),
so the read-only guard is purely at the Python `__setattr__` layer — C++ framework
still manages the memory directly.

**Known limitation**: Array sub-elements (`in_slot.arr[0] = x`) bypass the struct-level
guard — this is a ctypes limitation (the assignment is on the `ctypes.Array` subobject,
not on the struct). This is documented as a known limitation, not a design defect.

### 5.3 Slot ID (last_seq)

A monotonic sequence number identifying each slot read, exposed as:
```python
api.last_seq()  # → int: monotonic slot sequence number after last read_acquire
```

Implementation:
- ShmQueue: `SlotConsumeHandle::slot_id()` (DataBlock commit_index — strictly monotonic)
- ZmqQueue: `seq` field from wire frame header (emitted by ZmqQueue PUSH sender)

`last_seq()` is updated by `QueueReader` after each successful `read_acquire()`. It returns
the value from the most recent slot; returns 0 if no slot has been acquired yet.

Gap detection (missing slots = producer skipped) is possible by comparing consecutive
`last_seq()` values:
```python
def on_consume(in_slot, fz, msgs, api):
    if in_slot is None: return
    seq = api.last_seq()
    global _prev_seq
    if _prev_seq and seq != _prev_seq + 1:
        api.log(f"gap: {_prev_seq} → {seq} ({seq - _prev_seq - 1} slots lost)", level="warn")
    _prev_seq = seq
```

### 5.4 Ring Buffer Status

`QueueReader` exposes ring buffer metadata:
```python
api.in_capacity()     # → int: ring buffer slot count (ShmQueue: ring depth; ZmqQueue: recv buffer depth)
api.in_policy()       # → str: overflow policy description ("latest_only", "drop", "ring_N", etc.)
```

These map to `reader_->capacity()` and `reader_->policy_info()`. For ShmQueue, `capacity()`
returns the DataBlock ring slot count. For ZmqQueue, `capacity()` returns `max_buffer_depth`.

If a DataBlock is configured without a ring buffer (flexzone-only configuration), `ShmQueue`
construction throws at the factory call — there is nothing to read.

---

## 6. Flexzone and Spinlock Access

### 6.1 Flexzone (SHM Transport Only)

When flexzone is configured:
- Consumer gets a **writable** zero-copy view via `reader_->read_flexzone()` →
  `from_buffer()` on the ctypes struct type.
- No `readonly` restriction: consumer is granted full R/W access to the flexzone.
- User code is responsible for coordination (spinlock API).
- ZmqQueue: `read_flexzone()` returns `nullptr`; `fz` is `None` in script.

The flexzone lives in SHM for the entire session — it is NOT slot-scoped.
Scripts may cache the fz reference across `on_consume()` calls.

### 6.2 Spinlock Access (✅ Implemented 2026-03-09)

```python
api.spinlock(idx)         # → context manager; GIL released during lock wait
api.spinlock_count()      # → int: number of available spinlocks
# ZMQ transport: spinlock_count() returns 0; spinlock(i) raises RuntimeError
```

---

## 7. Checksum Policy

### 7.1 Symmetric Design

Checksum is a **framework-level policy** — not callable from script.

```
Write side (producer):  set_checksum_options(bool slot, bool fz)
                        → write_commit() auto-applies BLAKE2b
Read side (consumer):   set_verify_checksum(bool slot, bool fz)
                        → read_acquire() auto-verifies; returns nullptr on failure
```

Both sides are configured from the role's JSON config, not from script.

### 7.2 ShmQueue Checksum (BLAKE2b)

**Purpose**: Detect SHM memory corruption — hardware errors, bugs in shared
mappings, out-of-bounds writes from other processes.

- Write side: `update_checksum_slot()` / `update_checksum_flexible_zone()` on commit.
- Read side: `verify_checksum_slot()` / `verify_checksum_flexible_zone()` on acquire.
- Failure → `read_acquire()` returns `nullptr` (treated as timeout by the loop).
- A `LOGGER_ERROR` is emitted on failure with the channel name and slot_id.

### 7.3 ZmqQueue Checksum (No-Op)

TCP ensures byte integrity for ZMQ transport. The threat model for ZmqQueue
(network transmission errors) is handled by the transport layer, not by BLAKE2b.

`set_verify_checksum()` on ZmqQueue is accepted (no error) but is a no-op.
This allows the same `ConsumerConfig::verify_checksum` field to be present
regardless of transport — the framework applies it correctly.

### 7.4 ConsumerConfig Field

New field to add:
```json
"verify_checksum": true    // default: false; applies to slot only; fz always included when configured
```

Parsed in `ConsumerConfig::from_json_file()`. Passed to `ShmQueue` or `ZmqQueue`
after construction via `reader_->set_verify_checksum(config_.verify_checksum, core_.has_fz)`.

---

## 8. Design Gaps

### 8.1 QueueReader / QueueWriter Split (✅ Implemented 2026-03-09)

The `hub::Queue` single class must be replaced by separate `QueueReader` and `QueueWriter`
abstract classes. `ShmQueue` and `ZmqQueue` inherit from both.

Files affected:
- `src/include/utils/hub_queue.hpp` — replace `Queue` with `QueueReader` + `QueueWriter`
- `src/include/utils/hub_shm_queue.hpp` — `ShmQueue : QueueReader, QueueWriter`
- `src/include/utils/hub_zmq_queue.hpp` — `ZmqQueue : QueueReader, QueueWriter`
- All callers of `Queue*` — switch to `QueueReader*` or `QueueWriter*`
- `hub::Processor` uses both sides: `QueueReader& in_q, QueueWriter& out_q` (passed separately)

New methods to add:
- `QueueReader::last_seq()` → `uint64_t`
- `QueueReader::set_verify_checksum(bool slot, bool fz)`
- `QueueWriter::set_checksum_options(bool slot, bool fz)` (move from ShmQueue)
- `QueueReader::capacity()` → `size_t`
- `QueueWriter::capacity()` → `size_t`
- `QueueReader::policy_info()` → `std::string`
- `QueueWriter::policy_info()` → `std::string`

### 8.2 Unified run_loop_() in ConsumerScriptHost (✅ Implemented 2026-03-09)

Currently two separate loops:
- `run_loop_shm_()` — calls `shm()->acquire_consume_slot()` directly
- `run_loop_zmq_()` — calls `queue()->read_acquire()` via `Queue*`

After QueueReader is available, unify into a single `run_loop_()`:
```cpp
void run_loop_() {
    while (!stop_requested_) {
        const void* slot = reader_->read_acquire(timeout_ms_);
        {
            py::gil_scoped_acquire gil;
            call_on_consume_(slot, fz_inst_, msgs_);
        }
        reader_->read_release();
    }
}
```

### 8.3 Consumer::queue_reader() Accessor (✅ Implemented 2026-03-09)

`hub::Consumer` needs:
```cpp
hub::QueueReader* queue_reader() const noexcept;
```

For SHM transport: returns a `ShmQueue` created via `ShmQueue::from_consumer_ref()`.
For ZMQ transport: returns the existing `ZmqQueue*` (cast from `QueueReader*`).

The existing `shm()` and `queue()` accessors may be retained for backward compatibility
internally, or deprecated once all callers are migrated.

### 8.4 Template Consumer::connect<>() Phase 7 Regression (✅ Fixed 2026-03-09)

`hub_consumer.hpp` template `Consumer::connect<FlexZoneT, DataBlockT>()` still
has the old auto-derive logic from before Phase 7:
```cpp
// WRONG (pre-Phase-7 code still present in template):
const std::string loop_driver_str = opts.zmq_schema.empty() ? "shm" : "zmq";
```

Phase 7 fixed `hub_consumer.cpp` (non-template) but the template was missed.
This means the template path sends the wrong `loop_driver` in `CONSUMER_REG_REQ`,
potentially causing `TRANSPORT_MISMATCH` on SHM channels when `zmq_schema` is set.

Fix: remove the auto-derive line; use `opts.loop_driver` directly (which is set
explicitly by `ConsumerScriptHost` from `ConsumerConfig::loop_trigger`).

### 8.5 Consumer Spinlock API (✅ Implemented 2026-03-09)

Add to `ConsumerAPI`:
```cpp
py::object spinlock(std::size_t index);
std::size_t spinlock_count() const;
```

For ZMQ transport: `spinlock()` raises `RuntimeError("no shared memory for ZMQ consumer")`.

### 8.6 ConsumerConfig: verify_checksum Field (✅ Implemented 2026-03-09)

Add to `ConsumerConfig`:
```cpp
bool verify_checksum{false};
```

Parsed from `consumer.json["verify_checksum"]`. Passed to `set_verify_checksum()` after
queue construction.

### 8.7 ShmQueue New L3 Tests (✅ Implemented 2026-03-10)

9 test scenarios added to `test_datahub_hub_queue.cpp` (secrets 70010–70018):
1. `ShmQueueMultipleConsumers` — `from_consumer_ref` × 2 on same DataBlock
2. `ShmQueueFlexzoneRoundTrip` — producer writes fz, consumer reads fz
3. `ShmQueueRefFactories` — `from_producer_ref` + `from_consumer_ref` factory pair
4. `ShmQueueLatestOnly` — stale slot skipped, always returns newest
5. `ShmQueueRingWrap` — ring wrap with slot_count=2
6. `ShmQueueDestructorSafety` — release outstanding handle before destroy
7. `ShmQueueCapacityPolicy` — QueueMetrics overrun_count increments correctly
8. `ShmQueueLastSeq` — successive last_seq() comparisons advance monotonically
9. `ShmQueueVerifyChecksumMismatch` — verify_checksum=true catches corrupted slot

---

## 9. What Is Complete

| Feature | Status |
|---------|--------|
| SHM data consumption (run_loop_shm_) | Done |
| ZMQ data consumption (run_loop_zmq_) | Done (HEP-0021) |
| Broker registration (CONSUMER_REG_REQ / DISC_ACK) | Done |
| Heartbeat + channel lifecycle | Done (HEP-0007) |
| ctrl_thread_ (renamed from zmq_thread_) | Done (2026-03-09) |
| Transport arbitration (loop_driver + TRANSPORT_MISMATCH) | Done (Phase 6/7, 2026-03-09) |
| Zero-copy slot access (from_buffer) | Done |
| Read-only __setattr__ guard on in_slot | Done |
| Flexzone writable zero-copy view | Done (SHM only) |
| Timeout callback (on_consume with None) | Done |
| Metrics plane | Done (HEP-0019) |
| api.open_inbox() / api.wait_for_role() | Done (2026-03-09) |
| QueueReader / QueueWriter split | Done ✅ 2026-03-09 |
| Unified run_loop_() (eliminated shm/zmq variants) | Done ✅ 2026-03-09 |
| Consumer::queue_reader() accessor | Done ✅ 2026-03-09 |
| Template Consumer::connect<>() Phase 7 regression fix | Done ✅ 2026-03-09 (uses opts.loop_driver directly) |
| ConsumerAPI spinlock() / spinlock_count() | Done ✅ 2026-03-09 |
| QueueReader::last_seq() / api.last_seq() | Done ✅ 2026-03-09 |
| capacity() / policy_info() on QueueReader/QueueWriter | Done ✅ 2026-03-09 |
| ConsumerConfig::verify_checksum + set_verify_checksum | Done ✅ 2026-03-09 |
| Consumer inbox_thread_ (ROUTER — receiving side) | Done ✅ 2026-03-10 |
| Additional ShmQueue L3 tests (9 scenarios) | Done ✅ 2026-03-10 |

---

## 10. Prohibited Combinations (Consumer-Enforced)

| Combination | Severity | Reason |
|-------------|----------|--------|
| `loop_trigger=zmq` + producer `transport=shm` | Hard error | Broker rejects: TRANSPORT_MISMATCH |
| `loop_trigger=shm` + producer `transport=zmq` | Hard error | Broker rejects: TRANSPORT_MISMATCH |
| Schema hash mismatch (consumer vs producer) | Hard error | Broker rejects: schema incompatibility |
| `api.spinlock()` with ZMQ transport | RuntimeError | No shared memory |
| `api.in_slot.field = value` | AttributeError | Read-only slot via __setattr__ guard |
| Consumer attach without matching SHM secret | Hard error | DataBlock attach fails at connect time |

---

## 11. Config Reference

### consumer.json (current + proposed additions)

```json
{
  "consumer": {
    "uid":       "CONS-TEMPLOGGER-B7E3A142",
    "name":      "TempLogger",
    "log_level": "info",
    "auth": { "keyfile": "" }
  },

  "hub_dir": "/opt/pylabhub/hubs/lab",

  "channel":      "lab.sensors.temperature",
  "loop_trigger": "shm",      // "shm" | "zmq" — data transport; determines QueueReader type

  "timeout_ms":   5000,        // passed to read_acquire(); on_consume(None,...) fires on expiry

  "slot_schema":     { "fields": [{"name": "ts", "type": "float64"}, {"name": "value", "type": "float32"}] },
  "flexzone_schema": null,     // only for loop_trigger=shm; None for ZMQ

  "verify_checksum": false,    // [NEW] set_verify_checksum(slot=true, fz=has_fz); ShmQueue only

  "shm": { "enabled": true, "secret": 0 },

  "script": { "path": ".", "type": "python" },

  "validation": { "stop_on_script_error": false }
}
```

### ZMQ consumer variant

For `loop_trigger: "zmq"`, the consumer connects to the producer's ZMQ endpoint
(discovered from broker's `DISC_ACK`). No `zmq_in_endpoint` needed in consumer.json
— the hub is the service directory.

```json
{
  "consumer": { "uid": "CONS-NETCLIENT-AABBCCDD", "name": "NetClient" },
  "hub_dir":     "/opt/pylabhub/hubs/lab",
  "channel":     "lab.sensors.temperature",
  "loop_trigger": "zmq",      // connect to producer's ZMQ PUSH endpoint
  "timeout_ms":   5000,
  "slot_schema": { "packing": "aligned", "fields": [...] },
  "shm": { "enabled": false }
}
```

---

## 12. on_consume API Reference

```python
def on_consume(in_slot, flexzone, messages, api) -> None:
    """
    in_slot:  zero-copy read-only ctypes struct (slot_schema layout).
              None on timeout (timeout_ms elapsed with no data).
              __setattr__ raises AttributeError on writes.
              Array sub-elements: use np.ctypeslib.as_array() for zero-copy read.
              Valid only for the duration of this callback.

    flexzone: zero-copy writable ctypes struct (flexzone_schema layout).
              None if not configured or transport=zmq.
              Use api.spinlock(idx) for coordinated access with producer.
              Persists across callbacks (lifetime = session).

    messages: list of (sender_uid: str, data: bytes) received via Messenger.

    api:      ConsumerAPI — see below.
    """
    if in_slot is None:
        api.log("timeout — no slot received", level="warn")
        return

    api.log(f"seq={api.last_seq()} ts={in_slot.ts:.3f} value={in_slot.value:.4f}")

# ConsumerAPI methods (planned / current):
#
# api.name()                → str: "TempLogger"
# api.uid()                 → str: "CONS-TEMPLOGGER-B7E3A142"
# api.channel()             → str: channel name
# api.log(msg, level)       → None
# api.last_seq()            → int: slot_id of last read_acquire (ring index for SHM; wire seq for ZMQ)
# api.in_capacity()         → int: ring buffer depth (SHM) or recv buffer depth (ZMQ)
# api.in_policy()           → str: overflow policy info
# api.overrun_count()       → int: slots dropped due to ring overflow
# api.in_slots_received()   → int: total slots read
# api.spinlock(idx)         → context manager; GIL released during lock wait (SHM only)
# api.spinlock_count()      → int (0 for ZMQ transport)
# api.open_inbox(uid)       → InboxHandle or None
# api.wait_for_role(uid,ms) → bool
# api.shm_blocks()          → list of dicts with SHM block metadata
# api.send(target, data)    → None
# api.broadcast(data)       → None
# api.script_error_count()  → int
# api.report_metric(k, v)   → None
# api.stop()                → None
# api.set_critical_error(m) → None
```

---

*Preceded by: `loop_design_producer.md` · `loop_design_hub.md`*
