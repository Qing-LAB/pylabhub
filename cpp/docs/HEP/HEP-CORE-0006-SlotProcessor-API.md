# HEP-CORE-0006: Pluggable Slot-Processor API

| Property         | Value                                                      |
| ---------------- | ---------------------------------------------------------- |
| **HEP**          | `HEP-CORE-0006`                                            |
| **Title**        | Pluggable Slot-Processor API for Producer and Consumer     |
| **Author**       | Quan Qing, AI assistant                                    |
| **Status**       | Design Ready — Pending Implementation                      |
| **Category**     | Core                                                       |
| **Created**      | 2026-02-19                                                 |
| **C++-Standard** | C++20                                                      |
| **Version**      | 1.0                                                        |
| **Depends-on**   | HEP-CORE-0002 (DataHub), Messenger/Broker layer            |

---

## 1. Motivation

The current `hub::Producer` and `hub::Consumer` expose SHM slot access through two
low-level primitives:

- `Producer::post_write(WriteJob)` / `Consumer::read_shm(ReadJob)` — caller-driven,
  type-erased (`SlotWriteHandle&` / `SlotConsumeHandle&`), no FlexZone access, no
  messaging context.
- `DataBlockProducer::with_transaction<FlexZoneT, DataBlockT>()` — typed but requires
  the user to drive the loop themselves, manage thread lifetime, and wire up Messenger
  separately.

Neither provides a self-contained, typed, actor-style processing loop. Users who want
continuous SHM processing must replicate thread management, shutdown logic, exception
isolation, and messaging wiring in every application.

This HEP defines a **Pluggable Slot-Processor API** that:

1. Offers two processing modes: **Queue** (caller-driven) and **Real-time** (framework-driven).
2. Provides a fully typed `WriteProcessorContext<FlexZoneT, DataBlockT>` /
   `ReadProcessorContext<FlexZoneT, DataBlockT>` that bundles slot access, FlexZone
   access, messaging, and shutdown signalling in a single object.
3. Renames existing APIs for clarity (`post_write` → `push`, `write_shm` → `synced_write`,
   `read_shm` → `pull`).
4. Upgrades `Consumer::on_shm_data` to a typed, mode-aware handler.
5. Is compatible with Python bindings via pybind11.

---

## 2. Processing Modes

### 2.1 Mode definitions

| Mode | Enum value | Producer API | Consumer API | Who drives the loop |
|---|---|---|---|---|
| **Queue** | `ShmProcessingMode::Queue` | `push()` / `synced_write()` | `pull()` | Caller (external) |
| **Real-time** | `ShmProcessingMode::RealTime` | `set_write_handler()` | `set_read_handler()` | Framework thread |

```cpp
enum class ShmProcessingMode {
    Queue,    ///< Caller-driven: push()/synced_write() / pull()
    RealTime  ///< Framework-driven: set_write_handler() / set_read_handler()
};
```

### 2.2 Mode selection

Mode is **implicit** — determined by the API the user calls. No explicit config flag is
required. The framework thread switches mode atomically:

- `set_write_handler(fn)` (non-null) → enters Real-time mode; write_thread loops
  continuously calling `fn` until handler is cleared or stop is requested.
- `set_write_handler(nullptr)` → returns to Queue mode; write_thread waits for `push()`.
- Calling `push()` while Real-time is active: the job is enqueued and processed **after**
  the current handler invocation completes. This is not a recommended pattern; document
  as undefined behavior in concurrent handler + push scenarios.

The current active mode is queryable:

```cpp
ShmProcessingMode Producer::shm_processing_mode() const noexcept;
ShmProcessingMode Consumer::shm_processing_mode() const noexcept;
```

### 2.3 Queue mode — idle behavior

When no handler is installed and the `push()` queue is empty, the write_thread sleeps on
a condition variable. CPU usage is zero. The CV is notified on:

- `push()` enqueuing a job.
- `set_write_handler(fn)` installing a handler.
- `stop()` requesting shutdown.

This eliminates any busy-wait in the idle state.

### 2.4 Real-time mode — handler lifecycle

```
write_thread:
  while not stopping:
    handler = m_write_handler.load()          // atomic<shared_ptr>
    if handler is null:
      wait on CV (until handler set or stop)
      continue
    acquire write slot (with timeout)
    if timeout: continue                      // no slot yet; retry
    construct WriteProcessorContext
    try: handler(ctx)
    catch std::exception: log + continue
    catch ...:            log + continue
    if ctx.slot not committed: abort txn     // handler did not commit → discard slot
```

The handler is replaced atomically. The next iteration picks up the new handler. A
`LOGGER_INFO` is emitted on every install and removal.

---

## 3. Processor Context Types

### 3.1 WriteProcessorContext

```cpp
template <typename FlexZoneT, typename DataBlockT>
struct WriteProcessorContext
{
    // ── DataBlock access ─────────────────────────────────────────────────────
    WriteTransactionContext<FlexZoneT, DataBlockT> &txn;

    /// Direct typed FlexZone access. Type is validated at channel establishment.
    /// Synchronization is managed by the framework; always valid when context exists.
    FlexZoneT &flexzone() noexcept { return txn.flexzone(); }

    /// Typed slot buffer access. Commits on context destruction if not already committed.
    DataBlockT &slot() { return txn.slot(); }

    // ── Shutdown signal ──────────────────────────────────────────────────────
    /// True when the Producer is stopping. Check at natural loop checkpoints.
    /// Relevant primarily in Real-time mode; always false in Queue mode unless
    /// Producer::stop() is called while the job is executing.
    [[nodiscard]] bool is_stopping() const noexcept;

    // ── Peer messaging (direct P2C, bypasses broker) ─────────────────────────
    /// Broadcast raw bytes to all connected consumers on the data socket.
    void broadcast(const void *data, size_t size);

    /// Send raw bytes to a specific consumer identified by ZMQ ROUTER identity.
    void send_to(const std::string &identity, const void *data, size_t size);

    /// Returns ZMQ identities of currently connected consumers.
    [[nodiscard]] std::vector<std::string> connected_consumers() const;

    // ── Broker channel ───────────────────────────────────────────────────────
    /// Report a Cat 2 checksum error to the broker (fire-and-forget).
    void report_checksum_error(int32_t slot_idx, std::string_view desc);

    /// Full Messenger access for advanced use (additional registrations, etc.).
    Messenger &messenger;
};
```

### 3.2 ReadProcessorContext

```cpp
template <typename FlexZoneT, typename DataBlockT>
struct ReadProcessorContext
{
    // ── DataBlock access ─────────────────────────────────────────────────────
    ReadTransactionContext<FlexZoneT, DataBlockT> &txn;

    /// Direct typed FlexZone read access. Producer-written; consumer is read-only.
    /// The FlexZone is a single shared structure per channel (not per-slot).
    /// Consistent if the producer updates it within slot transactions (see §5).
    const FlexZoneT &flexzone() const noexcept { return txn.flexzone(); }

    /// Typed slot buffer read access.
    const DataBlockT &slot() const { return txn.slot(); }

    // ── Shutdown signal ──────────────────────────────────────────────────────
    [[nodiscard]] bool is_stopping() const noexcept;

    // ── Control messaging (to producer) ─────────────────────────────────────
    /// Send a typed ctrl frame upstream to the producer.
    void send_ctrl(std::string_view type, const void *data, size_t size);

    // ── Broker channel ───────────────────────────────────────────────────────
    void report_checksum_error(int32_t slot_idx, std::string_view desc);
    Messenger &messenger;
};
```

### 3.3 Type safety contract

- `FlexZoneT` and `DataBlockT` are fixed at `Producer::create<F,D>()` /
  `Consumer::connect<F,D>()` time and validated against the channel schema hash
  at establishment. By the time any handler or job executes, the types are guaranteed
  consistent across all participants.
- `static_assert(std::is_trivially_copyable_v<FlexZoneT>)` and
  `static_assert(std::is_trivially_copyable_v<DataBlockT>)` are enforced in the factory
  templates (existing constraint, unchanged).
- `flexzone()` and `slot()` are plain non-template methods on the context — no casting,
  no `void*` in the public API.

---

## 4. Producer API Changes

### 4.1 Renamed methods

| Old name | New name | Semantics (unchanged) |
|---|---|---|
| `post_write(WriteJob)` | `push<FlexZoneT, DataBlockT>(fn)` | Async queue; write_thread acquires slot, calls fn, commits |
| `write_shm(WriteJob, timeout)` | `synced_write<FlexZoneT, DataBlockT>(fn, timeout)` | Sync; calling thread acquires slot, calls fn, commits |

### 4.2 New methods

```cpp
// Real-time mode: install a persistent handler invoked by write_thread.
// Pass nullptr to deregister (returns to Queue mode).
// Thread-safe; hot-swap is atomic (next iteration picks up new handler).
template <typename FlexZoneT, typename DataBlockT>
void set_write_handler(
    std::function<void(WriteProcessorContext<FlexZoneT, DataBlockT> &)> fn);

// Query current mode.
[[nodiscard]] ShmProcessingMode shm_processing_mode() const noexcept;
```

### 4.3 Updated method signatures

```cpp
// Queue mode — async (non-blocking for caller).
template <typename FlexZoneT, typename DataBlockT>
bool push(std::function<void(WriteProcessorContext<FlexZoneT, DataBlockT> &)> job);

// Queue mode — sync (blocks caller until slot acquired and job completes).
template <typename FlexZoneT, typename DataBlockT>
bool synced_write(std::function<void(WriteProcessorContext<FlexZoneT, DataBlockT> &)> job,
                  int timeout_ms = 5000);
```

### 4.4 Handler function type alias (documentation convenience)

```cpp
template <typename FlexZoneT, typename DataBlockT>
using WriteHandlerFn = std::function<void(WriteProcessorContext<FlexZoneT, DataBlockT> &)>;
```

---

## 5. Consumer API Changes

### 5.1 Renamed methods

| Old name | New name | Semantics (unchanged) |
|---|---|---|
| `read_shm(ReadJob, timeout)` | `pull<FlexZoneT, DataBlockT>(fn, timeout)` | Sync; calling thread acquires slot, calls fn, releases |

### 5.2 Upgraded on_shm_data → set_read_handler

The existing `on_shm_data(ShmCallback)` uses a raw `SlotConsumeHandle&` (type-erased,
no FlexZone, no messaging context). It is **replaced** by:

```cpp
// Real-time mode: install a persistent handler invoked by shm_thread
// when a new slot is available.
// Pass nullptr to deregister.
template <typename FlexZoneT, typename DataBlockT>
void set_read_handler(
    std::function<void(ReadProcessorContext<FlexZoneT, DataBlockT> &)> fn);

// Query current mode.
[[nodiscard]] ShmProcessingMode shm_processing_mode() const noexcept;
```

The `shm_thread` already drives the read loop; this upgrade provides a typed context
instead of a raw handle. Behavior: shm_thread polls for new slots, constructs
`ReadProcessorContext`, calls handler, releases slot after handler returns. Exception
isolation: same as producer (std::exception + ... caught, logged, thread continues).

### 5.3 Updated pull signature

```cpp
// Queue mode — sync (blocks caller until next slot available).
template <typename FlexZoneT, typename DataBlockT>
bool pull(std::function<void(ReadProcessorContext<FlexZoneT, DataBlockT> &)> job,
          int timeout_ms = 5000);
```

### 5.4 Handler function type alias

```cpp
template <typename FlexZoneT, typename DataBlockT>
using ReadHandlerFn = std::function<void(ReadProcessorContext<FlexZoneT, DataBlockT> &)>;
```

---

## 6. FlexZone Semantics

### 6.1 Ownership and access

- **Producer owns writes.** FlexZone is updated by the producer, read-only for consumers.
- **Consumer access is `const`.** `ReadProcessorContext::flexzone()` returns `const FlexZoneT&`.
- **Not per-slot.** FlexZone is a single structure in the SHM header, shared across all
  ring buffer slots. It reflects the state set during the most recent producer transaction.

### 6.2 Consistency guarantee

FlexZone reads on the consumer side are consistent **if and only if** the producer updates
the FlexZone within the same slot transaction that commits the slot. The slot-commit atomic
state transition (`COMMITTED`) acts as the happens-before barrier.

If the producer updates FlexZone independently of slot transactions, the consumer may
observe a partially-written FlexZone. This is the user's responsibility — the framework
cannot enforce the ordering constraint.

**Pattern: per-slot FlexZone context.**
If a consumer needs the FlexZone state as it was when a specific slot was written, embed
a snapshot inside `DataBlockT`:

```cpp
struct MySlotData {
    MyFlexZone fz_snapshot;  // producer copies fz into slot before commit
    uint64_t   sequence;
    uint8_t    payload[...];
};
```

### 6.3 Raw access escape hatch

`std::span`-based raw FlexZone access exists on `WriteTransactionContext` and
`ReadTransactionContext` for special cases. Users who need it are aware of the
type-unsafety tradeoff. This is not surfaced in `WriteProcessorContext` /
`ReadProcessorContext` — use `ctx.txn` to reach the underlying transaction context
for raw access.

---

## 7. Internal Implementation Notes

### 7.1 Type erasure boundary

Type erasure is an **internal** implementation detail. The public API is fully typed.

```
Public:  push<F,D>(fn)           — user sees WriteProcessorContext<F,D>&
Internal: std::function<void()>  — fully-applied closure stored in write_queue
```

For the real-time handler, the internal storage is:

```cpp
// ProducerImpl private:
using InternalWriteHandler =
    std::function<void(SlotWriteHandle &, void *fz_ptr, size_t fz_sz)>;
std::atomic<std::shared_ptr<InternalWriteHandler>> m_write_handler;
```

`set_write_handler<F,D>(fn)` wraps `fn` in a lambda that constructs
`WriteProcessorContext<F,D>` from raw pointers and calls `fn`. The `void*` is never
visible to the user.

For `push<F,D>(fn)`, the closure captures the typed function and calls
`with_transaction<F,D>()` internally:

```cpp
write_queue.push([this, job = std::move(fn)]() {
    shm->with_transaction<F, D>(slot_timeout_ms,
        [&](WriteTransactionContext<F, D> &txn) {
            WriteProcessorContext<F, D> ctx{txn, *this};
            job(ctx);
        });
});
```

### 7.2 Shutdown ordering

Producer `stop()`:
1. Sets `write_stop = true`.
2. Notifies write_thread CV.
3. Joins write_thread.
4. After join: clears handler and queue (no concurrent access).

Handler respects `ctx.is_stopping()` — reads the same `write_stop` atomic. Users should
check this at natural loop checkpoints, not in tight inner loops.

The framework waits for write_thread to exit naturally. There is no forced kill.
Handlers that block indefinitely will block `stop()` indefinitely — document this.

---

## 8. Usage Examples

### 8.1 Real-time producer (sensor acquisition loop)

```cpp
auto producer = Producer::create<SensorFlexZone, SensorFrame>(messenger, opts);
producer->start();

producer->set_write_handler<SensorFlexZone, SensorFrame>(
    [&sensor](WriteProcessorContext<SensorFlexZone, SensorFrame> &ctx) {
        if (ctx.is_stopping()) return;

        auto frame = sensor.read_latest();        // blocking hardware read
        if (!frame) return;                       // no new data — skip slot

        ctx.flexzone().timestamp_ns = frame.ts;  // update channel-wide metadata
        ctx.slot().value            = frame.value;
        ctx.slot().sequence         = ++seq;
        ctx.txn.commit();                         // explicit commit

        if (ctx.connected_consumers().empty()) return;
        // optional: broadcast a lightweight notification
        ctx.broadcast(&seq, sizeof(seq));
    });

// ... later, hot-swap to a different handler:
producer->set_write_handler<SensorFlexZone, SensorFrame>(new_handler);

// ... or return to queue mode:
producer->set_write_handler<SensorFlexZone, SensorFrame>(nullptr);
```

### 8.2 Queue mode producer (event-driven)

```cpp
// Non-blocking: caller returns immediately; write_thread processes later.
producer->push<SensorFlexZone, SensorFrame>(
    [&data](WriteProcessorContext<SensorFlexZone, SensorFrame> &ctx) {
        ctx.slot() = data;
        ctx.txn.commit();
    });

// Blocking: caller waits until slot acquired and written.
producer->synced_write<SensorFlexZone, SensorFrame>(
    [&data](WriteProcessorContext<SensorFlexZone, SensorFrame> &ctx) {
        ctx.slot() = data;
        ctx.txn.commit();
    }, /*timeout_ms=*/1000);
```

### 8.3 Real-time consumer (continuous processing)

```cpp
consumer->set_read_handler<SensorFlexZone, SensorFrame>(
    [](ReadProcessorContext<SensorFlexZone, SensorFrame> &ctx) {
        if (ctx.is_stopping()) return;

        const auto &meta  = ctx.flexzone();        // channel-wide metadata
        const auto &frame = ctx.slot();            // current slot data

        process(meta.timestamp_ns, frame.value, frame.sequence);

        if (frame.sequence % 1000 == 0)
            ctx.send_ctrl("STATUS", &frame.sequence, sizeof(frame.sequence));
    });
```

### 8.4 Sync consumer (pull-on-demand)

```cpp
SensorFrame out{};
consumer->pull<SensorFlexZone, SensorFrame>(
    [&out](ReadProcessorContext<SensorFlexZone, SensorFrame> &ctx) {
        out = ctx.slot();
    }, /*timeout_ms=*/500);
```

---

## 9. Broker Extension (Future / Deferred)

A symmetric `register_message_handler` for `BrokerService` is conceptually similar:
a user-installed handler receives broker protocol messages and can inject custom
responses. This is deferred pending a separate HEP, as broker extension has
significantly different security and concurrency constraints.

---

## 10. Python Compatibility

The design is compatible with pybind11 Python bindings:

- `FlexZoneT` / `DataBlockT` must be trivially copyable — satisfied by fixed-layout
  ctypes-compatible structs exposed via pybind11.
- `std::function<void(WriteProcessorContext<F,D>&)>` accepts Python callables wrapped
  by pybind11's implicit `std::function` overload.
- `ctx.is_stopping()` is a plain bool property — no special handling needed.
- The handler executes on the C++ write_thread; the pybind11 binding layer must acquire
  the GIL (`py::gil_scoped_acquire`) before calling into Python. This is a binding
  implementation responsibility, not user responsibility.
- Users must not hold the GIL for extended periods inside handlers (same rule as all
  pybind11 callbacks from C++ threads).

---

## 11. API Migration Summary

| Old API | New API | Notes |
|---|---|---|
| `WriteJob = std::function<void(SlotWriteHandle&)>` | `WriteHandlerFn<F,D> = std::function<void(WriteProcessorContext<F,D>&)>` | Typed; FlexZone + messaging included |
| `post_write(WriteJob)` | `push<F,D>(WriteHandlerFn<F,D>)` | Renamed; context upgraded |
| `write_shm(WriteJob, timeout)` | `synced_write<F,D>(WriteHandlerFn<F,D>, timeout)` | Renamed; context upgraded |
| `on_shm_data(ShmCallback)` | `set_read_handler<F,D>(ReadHandlerFn<F,D>)` | Upgraded from raw handle to typed context |
| `ReadJob = std::function<void(SlotConsumeHandle&)>` | `ReadHandlerFn<F,D> = std::function<void(ReadProcessorContext<F,D>&)>` | Typed; FlexZone + messaging included |
| `read_shm(ReadJob, timeout)` | `pull<F,D>(ReadHandlerFn<F,D>, timeout)` | Renamed; context upgraded |
| *(none)* | `set_write_handler<F,D>(WriteHandlerFn<F,D>)` | New: real-time producer mode |
| *(none)* | `shm_processing_mode()` | New: mode introspection |

---

## 12. Deferred Items

| Item | Reason |
|---|---|
| Broker `register_message_handler` | Separate HEP; different security model |
| Async consumer pull queue | Real-time handler covers the use case; adding an async queue adds complexity for marginal benefit |
| Handler priority / chaining | Out of scope; user can compose handlers in a lambda |
| Per-slot FlexZone snapshots | Framework convention (§6.2); not framework infrastructure |
