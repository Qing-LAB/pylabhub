# Implementation Plan: QueueReader/QueueWriter Refactor + Consumer Role Completion

**Status**: Plan (2026-03-09) — design complete in `loop_design_consumer.md` + HEP-0017/0018.
**Scope**: Replace `hub::Queue` with `hub::QueueReader` + `hub::QueueWriter`; complete consumer
role implementation (spinlock, last_seq, verify_checksum, unified loop); close producer gaps.

---

## Confirmed Gaps

### Producer
| Gap | Where | Severity |
|-----|-------|----------|
| `queue_` type is `hub::Queue*` — should be `hub::QueueWriter*` | producer_script_host.hpp | Refactor |
| `set_checksum_options()` called on `unique_ptr<ShmQueue>` pre-move (line 377) — should be on QueueWriter interface | producer_script_host.cpp | Refactor |
| `api.overrun_count()` reads `shm->metrics()` only; ZMQ transport returns 0 even though ZmqQueue::metrics().overrun_count tracks it | producer_api.cpp:189 | Bug |
| `api.out_capacity()` / `api.out_policy()` not in ProducerAPI | producer_api.hpp/cpp | Gap |

### Consumer
| Gap | Where | Severity |
|-----|-------|----------|
| `run_loop_shm_()` + `run_loop_zmq_()` — two separate loops, should be unified via `QueueReader*` | consumer_script_host.cpp | Refactor |
| `Consumer::queue_reader()` does not exist — SHM path calls `shm()->acquire_consume_slot()` directly | hub_consumer.hpp/cpp | Gap |
| Template `Consumer::connect<FlexZoneT, DataBlockT>()` still has pre-Phase-7 auto-derive: `opts.zmq_schema.empty() ? "shm" : "zmq"` | hub_consumer.hpp | Bug |
| `ConsumerAPI::spinlock()` / `spinlock_count()` not exposed (only ProducerAPI has these) | consumer_api.hpp/cpp | Gap |
| `api.last_seq()` not in ConsumerAPI | consumer_api.hpp/cpp | Gap |
| `api.in_capacity()` / `api.in_policy()` not in ConsumerAPI | consumer_api.hpp/cpp | Gap |
| `ConsumerConfig::verify_checksum` field missing | consumer_config.hpp/cpp | Gap |
| `set_verify_checksum()` not on QueueReader abstract class | hub_queue.hpp | Gap |

### Layer 3 (hub_queue.hpp, ShmQueue, ZmqQueue)
| Gap | Where | Severity |
|-----|-------|----------|
| `hub::Queue` combined class — no access control | hub_queue.hpp | Refactor |
| `QueueReader::last_seq()` — not on abstract class | hub_queue.hpp | Gap |
| `QueueReader::set_verify_checksum(slot, fz)` — not on abstract class | hub_queue.hpp | Gap |
| `QueueWriter::set_checksum_options(slot, fz)` — ShmQueue only, not on abstract class | hub_queue.hpp | Gap |
| `QueueReader/Writer::capacity()` + `policy_info()` — not on abstract class | hub_queue.hpp | Gap |
| ShmQueue factories return `unique_ptr<ShmQueue>`, not `unique_ptr<QueueReader/Writer>` | hub_shm_queue.hpp | Refactor |
| ZmqQueue factories return `unique_ptr<ZmqQueue>`, not `unique_ptr<QueueReader/Writer>` | hub_zmq_queue.hpp | Refactor |
| L3 ShmQueue tests: ~9 missing scenarios | test_datahub_hub_queue.cpp | Testing |

---

## Phase 1: hub_queue.hpp — QueueReader / QueueWriter Split

**File**: `src/include/utils/hub_queue.hpp`

Replace `class Queue` with two independent abstract classes. Keep `OverflowPolicy`
and `QueueMetrics` structs (they are transport-agnostic and shared by both).

### QueueReader (read side only)

```cpp
class PYLABHUB_UTILS_EXPORT QueueReader
{
public:
    virtual ~QueueReader() = default;

    // ── Reading ───────────────────────────────────────────────────────────────
    virtual const void* read_acquire(std::chrono::milliseconds timeout) noexcept = 0;
    virtual void        read_release() noexcept = 0;
    virtual const void* read_flexzone() const noexcept { return nullptr; }

    // ── Slot sequence number ──────────────────────────────────────────────────
    /** Monotonic sequence number of the last slot returned by read_acquire().
     *  ShmQueue: SlotConsumeHandle::slot_id() (commit_index).
     *  ZmqQueue: wire frame seq field.
     *  Returns 0 until the first successful read_acquire(). */
    virtual uint64_t last_seq() const noexcept { return 0; }

    // ── Checksum verification (framework-level, not script-callable) ──────────
    /** Configure BLAKE2b verification on read_acquire().
     *  ShmQueue: verifies slot and/or flexzone checksum; read_acquire returns nullptr on failure.
     *  ZmqQueue: no-op (TCP ensures transport integrity; different threat model).
     *  Call once at initialization before the first read_acquire(). */
    virtual void set_verify_checksum(bool /*slot*/, bool /*fz*/) noexcept {}

    // ── Ring buffer status ────────────────────────────────────────────────────
    /** Ring/recv buffer slot count. Throws std::runtime_error if no ring buffer. */
    virtual size_t      capacity()    const = 0;
    /** Overflow policy description for diagnostics. */
    virtual std::string policy_info() const = 0;

    // ── Metadata ──────────────────────────────────────────────────────────────
    virtual size_t      item_size()     const noexcept = 0;
    virtual size_t      flexzone_size() const noexcept { return 0; }
    virtual std::string name()          const = 0;
    virtual QueueMetrics metrics()      const noexcept { return {}; }

    // ── Lifecycle ─────────────────────────────────────────────────────────────
    virtual bool start() { return true; }
    virtual void stop()  {}
    virtual bool is_running() const noexcept { return true; }
};
```

### QueueWriter (write side only)

```cpp
class PYLABHUB_UTILS_EXPORT QueueWriter
{
public:
    virtual ~QueueWriter() = default;

    // ── Writing ───────────────────────────────────────────────────────────────
    virtual void* write_acquire(std::chrono::milliseconds timeout) noexcept = 0;
    virtual void  write_commit()  noexcept = 0;
    virtual void  write_discard() noexcept = 0;
    virtual void* write_flexzone() noexcept { return nullptr; }

    // ── Checksum (framework-level, not script-callable) ───────────────────────
    /** Configure BLAKE2b checksum update on write_commit().
     *  ShmQueue: updates slot and/or flexzone checksum automatically.
     *  ZmqQueue: no-op.
     *  Call once at initialization before the first write_acquire(). */
    virtual void set_checksum_options(bool /*slot*/, bool /*fz*/) noexcept {}

    // ── Ring buffer status ────────────────────────────────────────────────────
    virtual size_t      capacity()    const = 0;
    virtual std::string policy_info() const = 0;

    // ── Metadata ──────────────────────────────────────────────────────────────
    virtual size_t      item_size()     const noexcept = 0;
    virtual size_t      flexzone_size() const noexcept { return 0; }
    virtual std::string name()          const = 0;
    virtual QueueMetrics metrics()      const noexcept { return {}; }

    // ── Lifecycle ─────────────────────────────────────────────────────────────
    virtual bool start() { return true; }
    virtual void stop()  {}
    virtual bool is_running() const noexcept { return true; }
};
```

**Remove `class Queue` entirely.** Any remaining references to `hub::Queue` in tests
or processors must be updated (see Phase 7).

---

## Phase 2: ShmQueue — Update to Inherit QueueReader + QueueWriter

**Files**: `src/include/utils/hub_shm_queue.hpp`, `src/utils/hub/hub_shm_queue.cpp`

### 2a. Inheritance change

```cpp
class ShmQueue : public QueueReader, public QueueWriter
{
    // Implementation unchanged — all methods already exist.
    // Duplicate declarations (item_size, flexzone_size, name, metrics, start, stop, is_running)
    // are implemented once; QueueReader and QueueWriter vtable entries both point to the same impl.
};
```

### 2b. Factory return type changes

```cpp
// Read-side factories → unique_ptr<QueueReader>
static std::unique_ptr<QueueReader> from_consumer(/* ... */);
static std::unique_ptr<QueueReader> from_consumer_ref(/* ... */);

// Write-side factories → unique_ptr<QueueWriter>
static std::unique_ptr<QueueWriter> from_producer(/* ... */);
static std::unique_ptr<QueueWriter> from_producer_ref(/* ... */);
```

### 2c. New methods to add to ShmQueue implementation

**`last_seq()`** — store slot_id after each `read_acquire()`:
```cpp
// In ShmQueueImpl: add `uint64_t last_seq_{0};`
// In read_acquire(): after acquiring SlotConsumeHandle, before returning:
//   impl_->last_seq_ = handle.slot_id();
uint64_t ShmQueue::last_seq() const noexcept { return impl_->last_seq_; }
```

**`set_verify_checksum(slot, fz)`** — store flags, apply in `read_acquire()`:
```cpp
// In ShmQueueImpl: add `bool verify_slot_{false}; bool verify_fz_{false};`
void ShmQueue::set_verify_checksum(bool slot, bool fz) noexcept {
    impl_->verify_slot_ = slot;
    impl_->verify_fz_   = fz;
}
// In read_acquire() after acquiring handle:
//   if (verify_slot_ && !handle.verify_checksum_slot()) {
//       LOGGER_ERROR("[ShmQueue] checksum mismatch on slot {} channel '{}'",
//                    handle.slot_id(), impl_->channel_name_);
//       handle.release(); return nullptr;
//   }
//   if (verify_fz_ && !handle.verify_checksum_flexible_zone()) { same pattern }
```

**`set_checksum_options(slot, fz)`** — move existing method to QueueWriter interface.
The impl already exists; this just makes it part of the abstract contract.

**`capacity()`** — return ring buffer slot count from DataBlock:
```cpp
size_t ShmQueue::capacity() const {
    // For consumer side:
    if (impl_->consumer_) return impl_->consumer_->ring_buffer_capacity();
    // For producer side:
    if (impl_->producer_) return impl_->producer_->ring_buffer_capacity();
    throw std::runtime_error("ShmQueue::capacity(): not connected");
}
```
Note: `DataBlockConsumer`/`DataBlockProducer` need a `ring_buffer_capacity()` accessor
(or use `DataBlockConfig::ring_buffer_capacity` via `config()` method — verify what's available).

**`policy_info()`** — describe the ConsumerSyncPolicy:
```cpp
std::string ShmQueue::policy_info() const {
    // Return string like "latest_only" / "ring_8" based on configured policy.
}
```

---

## Phase 3: ZmqQueue — Update to Inherit QueueReader + QueueWriter

**Files**: `src/include/utils/hub_zmq_queue.hpp`, `src/utils/hub/hub_zmq_queue.cpp`

### 3a. Inheritance change

```cpp
class ZmqQueue : public QueueReader, public QueueWriter { ... };
```

### 3b. Factory return type changes

```cpp
// PULL = read side → unique_ptr<QueueReader>
static std::unique_ptr<QueueReader> pull_from(/* existing params */);

// PUSH = write side → unique_ptr<QueueWriter>
static std::unique_ptr<QueueWriter> push_to(/* existing params */);
```

### 3c. New methods (ZmqQueue-specific impl)

**`last_seq()`** — already tracked internally (`recv_gap_count` uses seq). Store last received seq:
```cpp
// In ZmqQueueImpl: add `uint64_t last_seq_{0};`
// In recv_thread after decoding frame: impl_->last_seq_ = frame_seq;
uint64_t ZmqQueue::last_seq() const noexcept { return impl_->last_seq_; }
```

**`set_verify_checksum(slot, fz)`** — no-op (TCP handles transport integrity):
```cpp
void ZmqQueue::set_verify_checksum(bool, bool) noexcept {}  // TCP sufficient
```

**`set_checksum_options(slot, fz)`** — no-op:
```cpp
void ZmqQueue::set_checksum_options(bool, bool) noexcept {}
```

**`capacity()`** — return recv buffer depth:
```cpp
size_t ZmqQueue::capacity() const { return impl_->max_buffer_depth_; }
```

**`policy_info()`**:
```cpp
// PUSH side: "zmq_push_drop" or "zmq_push_block"
// PULL side: "zmq_pull_ring_N"
std::string ZmqQueue::policy_info() const { return impl_->policy_info_str_; }
```

---

## Phase 4: ProducerScriptHost + ProducerAPI — Close Producer Gaps

**Files**: `src/producer/producer_script_host.hpp/cpp`, `src/producer/producer_api.hpp/cpp`

### 4a. queue_ member type change

```cpp
// producer_script_host.hpp:
// Before: std::unique_ptr<hub::Queue> queue_;
// After:
std::unique_ptr<hub::QueueWriter> queue_;
```

### 4b. set_checksum_options() — call unconditionally after queue creation

```cpp
// producer_script_host.cpp start_role() — AFTER creating queue_ (SHM or ZMQ branch):
if (queue_)
    queue_->set_checksum_options(config_.update_checksum, core_.has_fz);
// Remove the ShmQueue-specific call (shm_q->set_checksum_options) from the SHM branch.
// ZmqQueue::set_checksum_options() is a no-op — safe to call unconditionally.
```

### 4c. api.overrun_count() — use queue metrics for both transports

```cpp
// producer_api.cpp — replace SHM-only path:
uint64_t ProducerAPI::overrun_count() const noexcept {
    if (queue_)
        return queue_->metrics().overrun_count;  // works for SHM and ZMQ
    return 0;
}
// Note: ProducerAPI needs to hold a raw ptr to queue_ (non-owning).
// Add: void set_queue(hub::QueueWriter* q) { queue_ = q; }
// Called in start_role() after queue_ is created.
```

### 4d. New ProducerAPI methods

```cpp
// producer_api.hpp — add:
size_t      out_capacity() const noexcept;
std::string out_policy()   const noexcept;

// producer_api.cpp — implement:
size_t ProducerAPI::out_capacity() const noexcept {
    if (!queue_) return 0;
    try { return queue_->capacity(); }
    catch (...) { return 0; }
}
std::string ProducerAPI::out_policy() const noexcept {
    if (!queue_) return "";
    try { return queue_->policy_info(); }
    catch (...) { return ""; }
}
```

### 4e. pybind11 bindings (producer_api.cpp embedded module)

```cpp
.def("out_capacity", &ProducerAPI::out_capacity,
     "Ring/send buffer slot count for the output queue.")
.def("out_policy",   &ProducerAPI::out_policy,
     "Output queue overflow policy description.")
```

---

## Phase 5: hub::Consumer — Add queue_reader() Accessor

**Files**: `src/include/utils/hub_consumer.hpp`, `src/utils/hub/hub_consumer.cpp`

### 5a. Fix template Phase 7 regression

```cpp
// hub_consumer.hpp — Consumer::connect<FlexZoneT, DataBlockT>():
// Remove the auto-derive line:
//   const std::string loop_driver_str = opts.zmq_schema.empty() ? "shm" : "zmq";  // WRONG
// Use explicit value from opts:
const std::string loop_driver_str = opts.loop_driver;  // set by caller (ConsumerScriptHost)
```

### 5b. Add queue_reader() accessor

After `DISC_ACK`, `hub::Consumer` creates the appropriate QueueReader and stores it:

```cpp
// hub_consumer.hpp:
hub::QueueReader* queue_reader() const noexcept;

// hub_consumer.cpp — in connect() / connect_embedded() after DISC_ACK:
// For SHM transport:
if (opts.loop_driver == "shm" && has_shm) {
    reader_ = hub::ShmQueue::from_consumer_ref(*shm_, item_sz, fz_sz, channel_name);
}
// For ZMQ transport (endpoint from DISC_ACK):
else if (opts.loop_driver == "zmq") {
    reader_ = hub::ZmqQueue::pull_from(zmq_endpoint, schema_fields, packing, false, buffer_depth);
    reader_->start();
}
// In Consumer::Impl: std::unique_ptr<hub::QueueReader> reader_;
```

The existing `shm()` accessor is kept for backward compatibility (spinlock access via ConsumerAPI).
The existing `queue()` (ZmqQueue*) can be deprecated once all callers use `queue_reader()`.

---

## Phase 6: ConsumerScriptHost + ConsumerAPI + ConsumerConfig

**Files**: `src/consumer/consumer_script_host.hpp/cpp`,
           `src/consumer/consumer_api.hpp/cpp`,
           `src/consumer/consumer_config.hpp/cpp`

### 6a. ConsumerConfig — add verify_checksum

```cpp
// consumer_config.hpp:
bool verify_checksum{false};

// consumer_config.cpp from_json_file():
cfg.verify_checksum = j.value("verify_checksum", false);
```

### 6b. ConsumerScriptHost — reader_ member + unified run_loop_()

```cpp
// consumer_script_host.hpp:
// Remove: run_loop_shm_(), run_loop_zmq_()
// Add:
std::unique_ptr<hub::QueueReader> reader_;  // owned; or non-owning ptr to Consumer::queue_reader()
uint64_t last_seq_{0};
```

After `DISC_ACK` (in `start_role()`):
```cpp
// Option A: consumer_ returns QueueReader* from queue_reader(); ScriptHost stores non-owning ptr.
// Option B: ScriptHost creates QueueReader directly (ShmQueue or ZmqQueue) and owns it.
// Recommend Option A — mirrors producer's pattern (out_producer_ owns SHM; queue_ is the interface).
reader_ = in_consumer_->queue_reader();  // non-owning; Consumer owns lifetime

// Apply checksum policy:
if (reader_)
    reader_->set_verify_checksum(config_.verify_checksum, core_.has_fz);
```

Unified `run_loop_()`:
```cpp
void ConsumerScriptHost::run_loop_() {
    while (!core_.shutdown_requested.load()) {
        const void* slot = reader_->read_acquire(std::chrono::milliseconds{config_.timeout_ms});
        last_seq_ = reader_->last_seq();   // update regardless of null (stays 0 on timeout)
        {
            py::gil_scoped_acquire gil;
            drain_messages_();
            call_on_consume_(slot, fz_inst_, messages_);
        }
        reader_->read_release();
    }
}
```

Replace `run_loop_shm_()` and `run_loop_zmq_()` with the above. The `run_ctrl_thread_()`
path that creates the ZMQ loop is kept (ctrl plane still needed for both transports).

### 6c. ConsumerAPI — new methods

```cpp
// consumer_api.hpp — add:
uint64_t    last_seq()      const noexcept;
size_t      in_capacity()   const noexcept;
std::string in_policy()     const noexcept;
py::object  spinlock(std::size_t index);
uint32_t    spinlock_count() const noexcept;

// consumer_api.hpp — storage:
const hub::QueueReader*    reader_{nullptr};      // set in start_role after consumer_ connects
const hub::Consumer*       consumer_{nullptr};    // for spinlock access
std::atomic<uint64_t>      last_seq_snapshot_{0}; // updated by loop_thread_, read by Python

// consumer_api.cpp — implement:
uint64_t ConsumerAPI::last_seq() const noexcept {
    return last_seq_snapshot_.load(std::memory_order_relaxed);
}

size_t ConsumerAPI::in_capacity() const noexcept {
    if (!reader_) return 0;
    try { return reader_->capacity(); } catch (...) { return 0; }
}

std::string ConsumerAPI::in_policy() const noexcept {
    if (!reader_) return "";
    try { return reader_->policy_info(); } catch (...) { return ""; }
}

py::object ConsumerAPI::spinlock(std::size_t index) {
    const hub::DataBlockConsumer *shm = consumer_ ? consumer_->shm() : nullptr;
    if (!shm)
        throw py::value_error("spinlock: SHM input channel not connected");
    return py::cast(ConsumerSpinLockPy{shm->get_spinlock(index)},
                    py::return_value_policy::move);
}

uint32_t ConsumerAPI::spinlock_count() const noexcept {
    const hub::DataBlockConsumer *shm = consumer_ ? consumer_->shm() : nullptr;
    return shm ? shm->spinlock_count() : 0u;
}
```

**Note on last_seq_ thread safety**: `loop_thread_` writes `last_seq_snapshot_` after
each `read_acquire()`; Python (GIL, loop_thread_) reads it via `api.last_seq()`.
Since both run under GIL in the loop thread, a regular `uint64_t` is also safe — but
atomic is cleaner and documents intent.

### 6d. ConsumerAPI pybind11 bindings (consumer_api.cpp embedded module)

```cpp
.def("last_seq",       &ConsumerAPI::last_seq,       "Monotonic slot sequence number of last slot read.")
.def("in_capacity",    &ConsumerAPI::in_capacity,    "Ring/recv buffer slot count.")
.def("in_policy",      &ConsumerAPI::in_policy,      "Input queue overflow policy description.")
.def("spinlock",       &ConsumerAPI::spinlock,       py::arg("index"))
.def("spinlock_count", &ConsumerAPI::spinlock_count)
```

---

## Phase 7: hub::Processor — Update Signature

**Files**: `src/include/utils/hub_processor.hpp`, `src/utils/hub/hub_processor.cpp`,
           `src/processor/processor_script_host.cpp`

### 7a. Processor::create() signature

```cpp
// Before: create(Queue& in_q, Queue& out_q, ProcessorOptions opts)
// After:
static std::optional<Processor> create(
    QueueReader& in_q, QueueWriter& out_q, ProcessorOptions opts);
```

The `Processor` internal `process_thread_` calls `in_q.read_acquire()` and
`out_q.write_acquire()` — these still work; only the parameter types change.

### 7b. ProcessorScriptHost callers

```cpp
// processor_script_host.cpp — in start_role() where Processor::create is called:
// in_queue_ is now unique_ptr<QueueReader>  (ShmQueue or ZmqQueue read side)
// out_queue_ is now unique_ptr<QueueWriter> (ShmQueue or ZmqQueue write side)
// Processor::create(*in_queue_, *out_queue_, proc_opts) — unchanged semantics
```

### 7c. ProcessorScriptHost member types

```cpp
// processor_script_host.hpp:
// Before: std::unique_ptr<hub::Queue> in_queue_, out_queue_;
// After:
std::unique_ptr<hub::QueueReader> in_queue_;
std::unique_ptr<hub::QueueWriter> out_queue_;
```

---

## Phase 8: New ShmQueue L3 Tests

**Files**: `tests/test_layer3_datahub/test_datahub_hub_queue.cpp`,
           `tests/test_layer3_datahub/workers/datahub_hub_queue_workers.cpp/h`

Secret/port range: 71001–71020 (avoid conflict with existing 70001–70009).

| Test | Description |
|------|-------------|
| `ShmQueue.MultiConsumer` | from_consumer_ref ×2 on same DataBlock; both read correctly |
| `ShmQueue.FlexzoneRoundTrip` | producer writes fz, consumer reads fz (zero-copy) |
| `ShmQueue.FromRefFactories` | from_producer_ref + from_consumer_ref pair; verify non-owning |
| `ShmQueue.LatestOnly` | N writes; consumer always gets newest (not oldest) |
| `ShmQueue.RingWrap` | slot_count=2, 10 writes; capacity()=2; metrics.overrun_count > 0 |
| `ShmQueue.LastSeq` | last_seq() increments monotonically after each read_acquire() |
| `ShmQueue.Capacity` | capacity() == slot_count from DataBlock config |
| `ShmQueue.PolicyInfo` | policy_info() returns non-empty string matching policy |
| `ShmQueue.VerifyChecksum` | set_verify_checksum(true, false); corrupt slot → read_acquire returns nullptr |

---

## Phase 9: Update All Remaining hub::Queue References

After Phases 1–8, do a project-wide search for remaining `hub::Queue` references
and update each to `QueueReader` or `QueueWriter` as appropriate:

```bash
grep -r "hub::Queue\b" src/ tests/ --include="*.hpp" --include="*.cpp" -l
```

Expected locations:
- `test_datahub_hub_queue.cpp` — update local variable types
- `test_datahub_hub_processor.cpp` — update Queue* to QueueReader*/QueueWriter*
- `datahub_hub_queue_workers.cpp` — update worker helper types
- Any remaining `hub::Queue*` in processor/consumer/producer script hosts

---

## Implementation Order (Recommended)

```
Phase 1  →  Phase 2  →  Phase 3                     (Layer 3: abstract interfaces + concrete impls)
                ↓
Phase 9 (search + replace hub::Queue refs in tests)
                ↓
Phase 7  (Processor signature — depends on QueueReader/QueueWriter)
                ↓
Phase 4  (Producer gaps — depends on QueueWriter)
Phase 5  (Consumer::queue_reader() — depends on QueueReader)
                ↓
Phase 6  (ConsumerScriptHost — depends on Phase 5)
                ↓
Phase 8  (New L3 tests — depends on Phases 1–3)
```

Phases 4, 5, 6 can proceed in parallel after Phase 3 is complete.

---

## Verification

```bash
# After each phase — build and run full suite:
cmake --build build -j2 2>&1 | tee /tmp/build.txt
grep -c "error:" /tmp/build.txt  # must be 0

timeout 120 ctest --test-dir build -j2 --output-on-failure 2>&1 | tee /tmp/test.txt
tail -5 /tmp/test.txt

# After all phases — grep for any remaining hub::Queue references:
grep -r "hub::Queue\b" src/ tests/ --include="*.hpp" --include="*.cpp"  # must be empty

# Verify new methods exposed to Python:
# (via test_layer4 integration or demo run):
# api.last_seq(), api.in_capacity(), api.in_policy()  — consumer
# api.out_capacity(), api.out_policy()                 — producer
# api.spinlock()                                       — consumer (SHM transport)
```

---

## Test Count Estimate

| Phase | New tests |
|-------|-----------|
| Phase 2 (ShmQueue methods) | ~3 (last_seq, verify_checksum, capacity) |
| Phase 4 (ProducerAPI) | ~2 (out_capacity, out_policy config tests) |
| Phase 6 (ConsumerConfig) | ~2 (verify_checksum field parsing) |
| Phase 8 (ShmQueue L3) | ~9 (see table above) |
| **Total new** | **~16** |
| Current total | 975 |
| **Expected total** | **~991** |
