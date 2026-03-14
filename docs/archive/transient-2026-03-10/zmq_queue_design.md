# ZmqQueue Internal Design

**Status**: ✅ Fully implemented — send_thread_, OverflowPolicy, InboxQueue, schema enforcement all in place.
ACK mechanism for ZmqQueue PUSH path deferred (requires PUSH→DEALER socket change).
Archived 2026-03-10 → `docs/archive/transient-2026-03-10/`. Canonical reference: HEP-CORE-0021 §13.
**Scope**: Wire format, schema enforcement, read/write paths, send_thread_ design,
InboxQueue, metrics, unified node concept, design gaps.
**See also**: HEP-CORE-0021 (broker protocol, virtual channel node registration).

---

## 1. Overview

`hub::ZmqQueue` implements both `hub::QueueReader` (PULL mode) and `hub::QueueWriter`
(PUSH mode) abstract interfaces. It is the network-capable peer to `hub::ShmQueue`.

Both implement the same acquire/release protocol:
```
write_acquire() → [write data] → write_commit() | write_discard()
read_acquire()  → [read data]  → read_release()
```

The caller (producer loop, processor loop, consumer loop) uses the same code path
regardless of transport. This is the **unified node concept**: SHM and ZMQ are equal
nodes — different facilities, same interface.

| Property | ShmQueue | ZmqQueue |
|----------|----------|----------|
| Backing store | OS shared memory (`mmap`) | ZMQ PUSH/PULL sockets |
| Network reach | Same machine only | Same machine, LAN, WAN |
| Schema enforcement | BLDS hash at broker | msgpack field-by-field per frame |
| Flexzone | Yes | No (future: second ZMQ frame) |
| Back-pressure (write) | `write_acquire()` blocks/times out | `OverflowPolicy` (Drop or Block) on send ring |
| Ring buffer | `slot_count` slots in SHM | Recv: `max_buffer_depth` slots; Send: `send_buffer_depth` slots |
| Overflow | `Block` or `Drop` | Recv: oldest slot overwritten; Send: `OverflowPolicy` |
| Checksum | BLAKE2b-256 per slot | No (deferred, HEP-0023) |
| Multiple readers | Yes (each has own read pointer) | Yes (multiple PULL sockets per PUSH) |
| Cross-hub | No | Yes |

---

## 2. Wire Format

Every ZMQ message is a single-part msgpack frame:

```
fixarray[4]
  [0] magic       : uint32  = 0x51484C50 ('PLHQ') — frame identity guard
  [1] schema_tag  : bin8    = first 8 bytes of BLAKE2b-256(BLDS) — schema identity guard
                              (zero-filled if not configured)
  [2] seq         : uint64  = monotonic send counter; gaps counted by recv_gap_count()
  [3] payload     : array[N] — one element per schema field
```

### 2.1 Payload Encoding Rules

| Field type | count | Encoded as |
|------------|-------|------------|
| scalar (`int32`, `float64`, …) | 1 | native msgpack type (preserves int/float distinction) |
| array (`uint8`, `float32`, …) | > 1 | msgpack bin — `count × sizeof(elem)` raw bytes |
| `string` | — | msgpack bin — `length` raw bytes |
| `bytes` | — | msgpack bin — `length` raw bytes |

**Type safety**: The receiver checks that each payload element has the correct msgpack
type tag for scalar fields (`msgpack::convert()` throws on integer↔float mismatch)
and the correct byte size for bin fields. Mismatches increment `recv_frame_error_count()`
and silently discard the frame.

### 2.2 Frame Size Bound

```
Outer envelope : fixarray(1) + uint32(5) + bin8(10) + uint64(9) = ~25 bytes
Payload header : array header 3 bytes
Per scalar     : ≤ 9 bytes
Per bin        : 5 + byte_size bytes
```

The receive buffer is pre-sized at construction time (`max_frame_sz_`) to avoid
per-frame allocation.

### 2.3 Schema Tag

An optional 8-byte schema tag (first 8 bytes of BLAKE2b-256 of the BLDS schema blob)
is embedded in every sent frame and checked on receive. Tag mismatches count as frame
errors with a rate-limited `LOGGER_WARN` (at most once per second). This is a
lightweight identity guard — it is not a full BLDS hash comparison.

---

## 3. Schema Definition and Layout

### 3.1 ZmqSchemaField

```cpp
struct ZmqSchemaField {
    std::string type_str;  // "bool","int8","uint8","int16","uint16","int32","uint32",
                           // "int64","uint64","float32","float64","string","bytes"
    uint32_t    count{1};  // 1 = scalar; > 1 = array encoded as bin
    uint32_t    length{0}; // for "string"/"bytes": total byte length
};
```

Valid `type_str` values are the same 13-type set as `FieldDef::type_str` in
`script_host_schema.hpp`. Note: BLDS uses `"char"` instead of `"string"/"bytes"` —
see `schema_def.hpp` for the conversion.

### 3.2 Layout Computation

Field offsets follow **C `ctypes.LittleEndianStructure` alignment rules**:
- `"aligned"` packing: each field is aligned to its own size (e.g. `float32` → 4-byte align).
- `"packed"` packing: no padding between fields.

Both PUSH and PULL sides must use the same packing string. Struct total size is
padded to the largest field alignment (natural mode only).

Scalars: `offset = (offset + align - 1) & ~(align - 1); size = sizeof(type)`.
Arrays:  `size = count × sizeof(elem)`.
string/bytes: `size = length`, always 1-byte aligned.

### 3.3 Schema Requirement

**Every ZmqQueue must be created with a non-empty schema.** Factories return `nullptr`
(with `LOGGER_ERROR`) if the schema is empty or any `type_str` is invalid. This is
enforced at construction time — there is no raw-bytes "schema-less" mode.

The `schema_spec_to_zmq_fields()` helper converts a `scripting::SchemaSpec`
(from `script_host_schema.hpp`) to a `std::vector<ZmqSchemaField>`. Currently
this helper is file-local in `processor_script_host.cpp` and `consumer_script_host.cpp`.
⚠ **Gap**: move to a shared location (e.g., `src/scripting/script_host_helpers.hpp`).

---

## 4. Read Path (PULL Mode)

```
Factory: ZmqQueue::pull_from(endpoint, schema, packing, bind, max_buffer_depth)

Memory layout (pre-allocated at construction):
  recv_ring_[max_depth]  — ring of decoded item_size slots (heap, no per-frame alloc)
  decode_tmp_[item_size] — recv_thread_-private staging buffer
  current_read_buf_[item_size] — returned by read_acquire()

Thread: recv_thread_ (spawned by start())
Loop:
  zmq_recv(socket, frame_buf, max_frame_sz, 0)   // blocks up to 100ms (ZMQ_RCVTIMEO)
  if EAGAIN/EINTR: continue
  unpack msgpack frame
  validate: magic, schema_tag, seq, payload field count and types
  decode payload fields into decode_tmp_
  acquire recv_mu_:
    if ring_count_ >= max_depth: overwrite ring_head_ (oldest), ++recv_overflow_count_
    memcpy decode_tmp_ → recv_ring_[ring_tail_]
    ++ring_count_; ring_tail_ = (ring_tail_+1) % max_depth
  recv_cv_.notify_one()

read_acquire(timeout):
  wait on recv_cv_ for timeout or ring_count_ > 0
  if ring_count_ == 0: return nullptr
  memcpy recv_ring_[ring_head_] → current_read_buf_
  ring_head_ = (ring_head_+1) % max_depth; --ring_count_
  return current_read_buf_.data()

read_release(): no-op (already consumed from ring)
```

**Key properties**:
- recv_thread_ and read_acquire() share state under `recv_mu_` + `recv_cv_`.
- No per-frame heap allocation (ring slots pre-allocated).
- `recv_overflow_count_` counts frames dropped because the consumer is slow.
- `recv_gap_count_` counts `seq` gaps (frames lost in the network between PUSH and PULL).

---

## 5. Write Path (PUSH Mode) — Current Implementation

```
Factory: ZmqQueue::push_to(endpoint, schema, packing, bind, schema_tag, sndhwm,
                            send_buffer_depth, overflow_policy, send_retry_interval_ms)

Memory layout (pre-allocated at construction):
  write_buf_[item_size]           — caller-visible write buffer (returned by write_acquire())
  send_ring_[send_buffer_depth]   — send ring (send_buffer_depth × item_size slots)
  send_local_buf_[item_size]      — send_thread_-private staging buffer
  send_sbuf_                      — send_thread_-private msgpack encode buffer (reused)

write_acquire(timeout):
  Drop policy: if send_count_ >= send_depth_: ++overrun_count_; return nullptr
               else: return write_buf_.data()   [non-blocking]
  Block policy: wait on send_cv_ until send_count_ < send_depth_ or stop or timeout
               on timeout/stop: ++overrun_count_; return nullptr
               else: return write_buf_.data_

write_commit():
  copy write_buf_ → send_ring_[send_tail_] under send_mu_
  advance send_tail_ = (send_tail_+1) % send_depth_; ++send_count_
  notify_one() → wakes send_thread_

write_abort(): no-op (write_buf_ not enqueued to send ring)

send_thread_ (spawned by start()):
  loop:
    wait on send_cv_ until send_count_ > 0 or stop
    if send_count_ == 0 && stop: exit  [clean drain complete]
    copy send_ring_[send_head_] → send_local_buf_ under send_mu_
    pack msgpack frame:
      [0] kFrameMagic (0x51484C50)
      [1] schema_tag_ (8 bytes, zero-filled if absent)
      [2] send_seq_.fetch_add(1)
      [3] payload array: pack_field() per schema field from send_local_buf_
    retry loop (zmq_send ZMQ_DONTWAIT):
      rc = zmq_send(socket, ..., ZMQ_DONTWAIT)
      if rc >= 0: success; break
      if EAGAIN and !stop: ++send_retry_count_; sleep(send_retry_interval_ms_); retry
      if EAGAIN and  stop: ++send_drop_count_; break  [stop-drain: one attempt only]
      non-retriable error:  ++send_drop_count_; break
    advance ring head: send_head_ = (send_head_+1) % send_depth_; --send_count_
    notify_all() → wakes blocked write_acquire (Block policy)
```

**Key properties**:
- write_buf_ is write_acquire()'s exclusive buffer. write_commit() memcpy's into send_ring_.
  The caller never writes directly into the ring, so no race between caller and send_thread_.
- send_thread_ copies ring slot under lock before releasing lock for zmq_send. The slot
  stays counted (send_count_ not decremented) until after zmq_send completes. This prevents
  write_acquire() from overwriting a slot being sent.
- `notify_one()` in write_commit() and `notify_all()` in ring-head-advance are both safe:
  send_thread_ (waiting for items) and write_acquire(Block) (waiting for space) cannot
  both be waiting simultaneously (one waits when full, the other when empty; depth > 0).
- `sndhwm` controls ZMQ's internal send HWM (default 1000 frames). For latency-sensitive
  pipelines, set to 1–4 to limit ZMQ buffering.

**Limitations**:
- No ACK/delivery guarantee. Successful `zmq_send()` means the frame entered ZMQ's
  kernel buffer, not that the peer received it. See §6 for the planned ACK extension.
- `send_drop_count_` counts frames dropped at EAGAIN-on-stop or non-retriable error,
  **not** frames the peer never processed (no ACK mechanism).

---

## 6. Write Path Extension — ACK Mechanism ⚠ Planned

The current write path has no delivery confirmation. The planned extension adds
an ACK reply socket so the sender knows if the peer accepted the frame:

```
send_thread_ extended:
  after zmq_send success:
    wait for ACK frame (ZMQ_RCVTIMEO = send_retry_interval_ms_)
    ACK: msgpack fixarray[2]: [ack_magic: uint32='PLHA', error_code: uint8]
      0=OK → advance ring head
      1=queue_overflow → ++send_nack_count_; retry slot (don't advance head)
      2=schema_error / 3=handler_error → ++send_drop_count_; advance head
    on ACK timeout: retry (send_thread_ re-sends same slot from head)
```

This requires a separate reply socket (PUSH/PULL is unidirectional). For InboxQueue
(ROUTER/DEALER), ACK is built-in. See §8.

**Offline detection** (⚠ Planned): after repeated ACK timeouts, send_thread_ sends
`ROLE_PRESENCE_REQ{target_uid}` to broker to check if peer is alive.

---

## 7. Metrics — Current

| Metric | Status | Meaning |
|--------|--------|---------|
| `recv_overflow_count()` | ✅ | PULL ring buffer full; oldest item dropped |
| `recv_frame_error_count()` | ✅ | Invalid frame (bad magic, schema mismatch, type/size error) |
| `recv_gap_count()` | ✅ | Seq number gaps; frames lost in network between PUSH and PULL |
| `send_drop_count()` | ✅ | Frame dropped at EAGAIN-on-stop or non-retriable zmq_send error |
| `send_retry_count()` | ✅ | Genuine EAGAIN retries by send_thread_ (normal operation only) |
| `overrun_count()` | ✅ | write_acquire() returned nullptr (send buffer full / Block timeout) |
| `send_nack_count()` | ⚠ Planned | ACK with non-zero error code from peer |

All counters are `std::atomic<uint64_t>`. `metrics()` reads each independently —
fields may not reflect the exact same instant under concurrent I/O (documented in
`QueueMetrics`). For a consistent snapshot, `stop()` the queue first.

---

## 8. InboxQueue (ROUTER/DEALER Pair) — ✅ Implemented 2026-03-08

### 8.1 Concept

The **inbox** is a per-role facility for schema-validated point-to-point messaging
between any two roles. It is distinct from the data channel:

| | Data Channel (ZmqQueue) | Inbox (InboxQueue) |
|--|-------------------------|--------------------|
| Pattern | PUSH/PULL (fan-out) | ROUTER/DEALER (point-to-point) |
| Direction | Producer → Consumer(s) | Any role → any role |
| Schema owner | Producer (declares in REG_REQ) | Receiver (declares in own config) |
| ACK | ⚠ Planned (reply socket) | ✅ Built-in (ROUTER knows DEALER identity) |
| Broker registration | Yes (data transport) | Yes (inbox_endpoint + inbox_schema) |
| Script callback | `on_produce` / `on_consume` | `on_inbox(msg_slot, sender_uid, api)` |

### 8.2 Ownership

The **receiver** owns the inbox config — declared in its own role config
(`inbox_schema`, `inbox_buffer_depth`, `inbox_overflow_policy`). This is symmetric
with SHM: the producer owns SHM config (slot_count, secret). Senders discover
inbox config from the broker — they do not set it.

### 8.3 Registration Protocol (planned)

```
REG_REQ extension:
  inbox_endpoint  string   bound ROUTER endpoint; "" = auto-assign
  inbox_schema    array    ZmqSchemaField list (same JSON format as slot_schema)
  inbox_buffer_depth   uint32  recv ring buffer depth (default 64)
  inbox_overflow_policy string  "drop" | "block"

ROLE_INFO_REQ (any role → broker):
  target_uid  string

ROLE_INFO_ACK (broker → requester):
  target_uid      string
  inbox_endpoint  string
  inbox_schema    array (ZmqSchemaField list)

ROLE_PRESENCE_REQ (role → broker):
  uid  string

ROLE_PRESENCE_ACK (broker → requester):
  uid    string
  alive  bool
```

### 8.4 InboxQueue Internal Design

```
Receiver side (ROUTER socket — binds to inbox_endpoint):
  inbox_thread_:
    zmq_recv(router_socket, ...) → [identity frame][empty][data frame]
    validate msgpack frame (same wire format as ZmqQueue data)
    decode into inbox_ring_[tail]
    call on_inbox(msg_slot, sender_uid, api)   // under GIL
    zmq_send(router_socket, [identity][empty][ack_frame])  // ACK using DEALER identity

Sender side (DEALER socket — connects to inbox_endpoint):
  open_inbox(target_uid) → send ROLE_INFO_REQ → get inbox_endpoint + schema
  create InboxClient<InboxT> with internal send buffer (same as §5 design)
  send_thread_: pack frame → zmq_send → wait for ACK → retry or drop
```

---

## 9. Unified Node Concept

SHM and ZmqQueue are **equal** under `hub::QueueReader`/`hub::QueueWriter`. The role loop is transport-agnostic:

```cpp
// Producer loop — identical for SHM and ZmqQueue
void run_loop_() {
    while (!stop_requested_) {
        void* slot = queue_->write_acquire(config_.timeout_ms);
        if (!slot) { on_timeout_(); apply_timing_policy_(); continue; }
        call_on_produce_(slot);
        queue_->write_commit();
        sleep_until_next_deadline_();
    }
}
```

| Queue property | ShmQueue | ZmqQueue |
|----------------|----------|----------|
| `write_acquire()` | Blocks until slot free | Drop: nullptr if buffer full; Block: waits up to timeout |
| `write_commit()` | Releases spinlock, advances ring | Enqueues to send ring; send_thread_ delivers |
| `read_acquire()` | Waits for new slot | Waits for recv ring item |
| `read_release()` | Releases read lock | No-op (copy-on-consume) |
| Back-pressure | Yes (spinlock blocks writer) | Yes (OverflowPolicy on send ring) |
| Delivery guarantee | In-process memory | Fire-and-forget (ACK extension planned, §6) |

---

## 10. Flexzone

ZmqQueue has **no flexzone** in the current design. `read_flexzone()` and
`write_flexzone()` return `nullptr` (inherited from the Queue base class default).

Scripts receive `fz=None` when reading from a ZmqQueue-backed channel.

Future option: encode flexzone as a second ZMQ frame (HEP-0023). The PUSH socket
would send a two-part message; the PULL side would recv both frames and expose the
second as flexzone. This requires protocol versioning (magic byte bump).

---

## 11. Thread Safety

`ZmqQueue` is **not thread-safe** at its public API. Internally:

| Thread | State accessed |
|--------|---------------|
| `recv_thread_` | `recv_ring_`, `ring_head_/tail_/count_` (under `recv_mu_`) |
| `send_thread_` | `send_ring_`, `send_head_/tail_/count_`, `send_local_buf_`, `send_sbuf_` (under `send_mu_`) |
| Caller thread | `read_acquire()` / `read_release()` (under `recv_mu_`) |
| Caller thread | `write_acquire()` / `write_commit()` / `write_abort()` (write_buf_ exclusive; send_mu_ for ring enqueue) |

The public write API (`write_acquire`, `write_commit`) must be called from one thread only.

---

## 12. Lifecycle

```
Construction (factory):
  compute field layout → item_sz
  pre-allocate recv_ring_ + decode_tmp_ + current_read_buf_ (Read mode)
  pre-allocate write_buf_ + send_ring_ + send_local_buf_ (Write mode)
  validate schema → nullptr if invalid
  validate send_buffer_depth > 0 → nullptr if zero

start():
  zmq_ctx_new(); zmq_ctx_set(ZMQ_BLOCKY, 0)
  zmq_socket(PULL or PUSH)
  zmq_setsockopt(ZMQ_LINGER=0)
  zmq_setsockopt(ZMQ_SNDHWM) if sndhwm > 0  [PUSH only]
  zmq_bind() or zmq_connect()
  resolve actual_endpoint (for port-0 bind; connect-mode: equals configured endpoint)
  spawn recv_thread_ (Read mode) or send_thread_ (Write mode)
  returns true on success; false + LOGGER_ERROR on any failure

stop():
  recv_stop_ = true; send_stop_ = true
  recv_cv_.notify_all(); send_cv_.notify_all()
  join recv_thread_ (Read mode)
  join send_thread_ after drain — remaining send ring items sent once; EAGAIN → drop (Write mode)
  zmq_close(socket); zmq_ctx_term(ctx)
  safe to call multiple times (no-op if not running)

Destruction:
  ~ZmqQueue() calls stop()
```

---

## 13. Design Gaps Summary

| Gap | Severity | Status |
|-----|----------|--------|
| ACK mechanism for PUSH write path | Major | ⚠ Deferred (PUSH→DEALER socket change required) |
| `send_nack_count()` metric | Moderate | ⚠ Deferred with ACK extension |
| InboxQueue (ROUTER/DEALER) | Major | ✅ Implemented 2026-03-08 (`hub_inbox_queue.hpp/cpp`) |
| `ROLE_INFO_REQ/ACK` protocol | Major | ✅ Implemented 2026-03-09 (`messenger.hpp`, `broker_service.cpp`) |
| `ROLE_PRESENCE_REQ/ACK` protocol | Moderate | ✅ Implemented 2026-03-09 |
| `schema_spec_to_zmq_fields()` shared | Minor | ✅ Moved to `script_host_helpers.hpp` 2026-03-09 |
| Flexzone over ZMQ | Minor | ⚠ Deferred to HEP-0023 |
| Checksum over ZMQ | Minor | ⚠ Deferred to HEP-0023 |

---

## 14. Source File Reference

| Component | File |
|-----------|------|
| `ZmqQueue` interface | `src/include/utils/hub_zmq_queue.hpp` |
| `ZmqQueue` implementation | `src/utils/hub/hub_zmq_queue.cpp` |
| `Queue` abstract base | `src/include/utils/hub_queue.hpp` |
| `ShmQueue` (for comparison) | `src/include/utils/hub_shm_queue.hpp`, `src/utils/hub/hub_shm_queue.cpp` |
| `schema_spec_to_zmq_fields()` | `src/processor/processor_script_host.cpp` (⚠ to be moved) |
| ZMQ Virtual Channel Node protocol | `docs/HEP/HEP-CORE-0021-ZMQ-Virtual-Channel-Node.md` |
| L3 ZmqQueue tests | `tests/test_layer3_datahub/test_datahub_hub_zmq_queue.cpp` |

---

*Next: `loop_design_consumer.md` · `loop_design_processor.md`*
