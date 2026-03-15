# Code Review: DataHub / Inbox / Script Host Layer
**Date**: 2026-03-09
**Scope**: hub_zmq_queue, hub_inbox_queue, hub_shm_queue, hub_processor, producer/consumer/processor script hosts, API layer
**Status**: ✅ CLOSED (2026-03-10 — all actionable items fixed; false positives noted)

---

## Severity Legend
- **CRITICAL** — data corruption, crash, deadlock
- **HIGH** — race condition, resource leak, wrong behavior
- **MEDIUM** — performance issue, design inconsistency, observable but non-fatal bug
- **LOW** — style, minor robustness, missing defensive check

---

## CRITICAL

### CR-01: `send_ring_` head advanced under lock but head slot read OUTSIDE lock — TOCTOU in send_thread_
**File**: `src/utils/hub/hub_zmq_queue.cpp`, lines 373–435 (`run_send_thread_`)

The send thread copies the head slot into its private buffer (`send_local_buf_`) while holding `send_mu_` (line 381), then releases the lock before encoding and sending. It then re-acquires `send_mu_` at line 432 to advance `send_head_`. **This part is actually safe**: the slot is copied out under the lock.

However, there is a subtle issue: `send_sbuf_` (a `msgpack::sbuffer`) is a **member of `ZmqQueueImpl`**, not a local variable. It is accessed exclusively on the send thread and declared in the struct. But if `stop()` were called from another thread while `zmq_send` is blocked retrying, the send thread accesses `send_sbuf_` after the ZMQ send retry loop. Since `send_sbuf_` is a non-atomic struct member and stop signals only set `send_stop_`, there is no ownership question — this is correct. **Not actually a bug**, but warranted explicit comment.

**Real finding**: After `send_count_ == 0 → break` exits the wait (lines 376–378), the code reads `send_ring_[send_head_]` (line 381) **but `send_head_` could equal `send_depth_`** if the ring is exactly full and the last item was just committed. The ring arithmetic uses modular indexing and `send_depth_` is always the modulus, so `send_head_ < send_depth_` is maintained. However, the check `if (send_count_ == 0) break` fires when `send_stop_` is set AND ring is empty — at that point the `memcpy` at line 381 is skipped. Code is correct, but the early-break without checking the ring before reading is confusing and future modifications could inadvertently read a stale index.

**Real CRITICAL**: None in the send ring path itself, but see CR-02.

---

### CR-02: `InboxQueue::stop()` while `recv_one()` is blocking — ZMQ socket closed from wrong thread may cause ETERM + partial-frame state
> ✅ FIXED 2026-03-10 — Reordered `stop_role()` in `producer_script_host.cpp`: inbox_thread_ now joined BEFORE `inbox_queue_->stop()`, ensuring socket is closed only after the thread has exited.
**File**: `src/utils/hub/hub_inbox_queue.cpp`, lines 407–415 (`stop()`)
**File**: `src/producer/producer_script_host.cpp`, lines 505–507 (`stop_role()`)

`InboxQueue` has **no background thread**. `recv_one()` is a blocking call made directly on `inbox_thread_` (producer_script_host.cpp:771). When `stop_role()` is called:

1. `core_.running_threads.store(false)` — the inbox thread's loop condition sees this and may not exit before `recv_one()` returns from the next 100 ms timeout.
2. `inbox_queue_->stop()` is called (line 506) — this calls `zmq_close(pImpl->socket)` and `zmq_ctx_term()` from **a different thread** (the script/interpreter thread) while `inbox_thread_` may still be inside `zmq_recv()`.

Closing a ZMQ socket from a thread that did not create it in that ZMQ context is not safe per the ZMQ documentation (which states "Do not use or close sockets except in the thread that created them"). With `ZMQ_BLOCKY=0` the context shutdown should interrupt the `recv`, but the code is relying on this ZMQ-internal behavior, not on a thread-safe shutdown protocol.

**Recommended fix**: Signal the inbox thread to stop (e.g., via `recv_stop_` atomic or by closing the context first while the inbox thread is the one that calls `zmq_close`/`zmq_ctx_term`), similar to how `ZmqQueue::stop()` works with its `recv_stop_` flag and a wake of the `recv_cv_`.

---

### CR-03: `ShmQueue::write_commit()` calls `commit()` before `update_checksum_slot()` — checksum computed on uncommitted data, then released
> ✅ FIXED 2026-03-10 — Swapped order in `hub_shm_queue.cpp`: `update_checksum_slot()` and `update_checksum_flexible_zone()` now called BEFORE `commit()`.
**File**: `src/utils/hub/hub_shm_queue.cpp`, lines 249–259 (`write_commit`)

```cpp
(void)pImpl->write_handle->commit(pImpl->item_sz);   // ← makes slot visible to readers
if (pImpl->checksum_slot)
    (void)pImpl->write_handle->update_checksum_slot(); // ← checksum after visibility!
...
(void)pImpl->producer()->release_write_slot(*pImpl->write_handle); // ← release
```

`commit()` (which sets the slot's commit index and makes the data visible in the ring) is called **before** `update_checksum_slot()`. A consumer that reads the slot between `commit()` and `update_checksum_slot()` will see **stale or zero checksum bytes**, causing a spurious checksum verification failure (returns nullptr from `read_acquire`). This is a real data-integrity race under SHM concurrent access.

**The correct order** should be: compute checksum → commit → release, so the checksum is already in place when the slot becomes observable.

---

## HIGH

### HR-01: `ProducerAPI::script_errors_` is not atomic — written from loop_thread_, read from ctrl_thread_ and Python thread
> ✅ FIXED 2026-03-10 — `script_errors_` changed to `std::atomic<uint64_t>` in all three API headers (producer/consumer/processor); all read sites updated to `.load(relaxed)`. Increment uses `fetch_add(1, relaxed)`.
**File**: `src/producer/producer_api.hpp`, line 185

```cpp
uint64_t script_errors_{0};  // plain uint64_t, not atomic
```

`increment_script_errors()` (++script_errors_, line 73) is called from:
- `run_loop_()` (loop_thread_)
- `run_inbox_thread_()` (inbox_thread_)
- `call_on_stop_common_()` (interpreter/script thread)

`script_error_count()` is called from Python (GIL held) and potentially from `snapshot_metrics_json()` called from `ctrl_thread_` during heartbeat. Without atomic access, concurrent increments are a data race (UB under C++ memory model). Same issue exists in `ConsumerAPI` (consumer_api.hpp:172) and `ProcessorAPI`.

**Recommended fix**: Change `script_errors_` to `std::atomic<uint64_t>` in all three APIs.

---

### HR-02: `ConsumerScriptHost::queue_reader_` is a raw pointer set in start_role() (main thread), used in run_loop_() (loop_thread_) — no synchronization
> ✅ FIXED 2026-03-10 — `ConsumerAPI::reader_` changed to `std::atomic<const hub::QueueReader*>`. `set_reader()` uses `store(release)`. `in_capacity()` and `in_policy()` load with `acquire`.
**File**: `src/consumer/consumer_script_host.hpp`, line 102; `consumer_script_host.cpp`, line 271–287

```cpp
hub::QueueReader *queue_reader_{nullptr};  // set in start_role() before thread creation
```

`queue_reader_` is written in `start_role()` (interpreter thread, lines 271–287), then read in `run_loop_()` (loop_thread_, lines 474, 484, 494, 495, 528, 551). The thread is launched at line 349 (`loop_thread_ = std::thread(…)`).

**Thread creation provides happens-before**: `std::thread` construction synchronizes-before the thread function starts, so writes before the `std::thread` constructor are visible to the new thread. In this case the assignment at line 271 happens before line 349 (`std::thread([this] { run_loop_(); })`). This is **safe** by the C++ memory model's thread-creation guarantee.

However: `api_.set_reader(queue_reader_)` (line 287) stores the raw pointer into `ConsumerAPI::reader_` (a `const hub::QueueReader*`). This pointer is also accessed from the `ctrl_thread_` via `snapshot_metrics_json()` → `in_capacity()` → `reader_->capacity()`. There is no synchronization between the ctrl_thread_ read and the stop_role() nulling via `api_.set_reader(nullptr)`. This is a **real race**: `stop_role()` (line 369) sets `reader_ = nullptr` while `ctrl_thread_` may be reading `reader_`. The ctrl_thread_ is joined before `api_.set_reader(nullptr)` in `stop_role()` (lines 362–364), but if the ctrl_thread_ calls `snapshot_metrics_json()` very close to shutdown, the exact sequencing matters.

**Recommended fix**: `ConsumerAPI::reader_` should be a `std::atomic<const hub::QueueReader*>` or protected by a mutex/release-store.

---

### HR-03: `InboxQueue::recv_one()` sets `ZMQ_RCVTIMEO` on every call — modifying a socket option while another thread may be using the socket
> ✅ FIXED 2026-03-10 — Added `last_rcvtimeo` cache in `InboxQueueImpl` and `last_acktimeo` cache in `InboxClientImpl`. `setsockopt` now called only when value changes.
**File**: `src/utils/hub/hub_inbox_queue.cpp`, lines 441–443

```cpp
const int timeout_ms = static_cast<int>(timeout.count());
zmq_setsockopt(pImpl->socket, ZMQ_RCVTIMEO, &timeout_ms, sizeof(timeout_ms));
```

This is called from `inbox_thread_`. While `recv_one()` is designed for single-thread use, `stop()` can be called from the main thread concurrently (see CR-02). If `stop()` calls `zmq_close()` while `recv_one()` is setting socket options, the socket handle becomes invalid mid-operation. This is less of a correctness issue in practice (ETERM/ENOTSUP is returned) but represents an unsynchronized cross-thread socket operation.

**Additionally**: Setting `ZMQ_RCVTIMEO` on every `recv_one()` call (not just at initialization) is unnecessary overhead; the timeout value does not change across calls. Should be set once at `start()`.

---

### HR-04: `ZmqQueue` mode-guard returns nullptr for cross-mode calls — but `ShmQueue` has NO mode guard for `write_acquire` on read-only queue
**File**: `src/utils/hub/hub_zmq_queue.cpp`, lines 837–839; `src/utils/hub/hub_shm_queue.cpp`, line 234

`ZmqQueue::write_acquire()` correctly guards with `if (pImpl->mode != ZmqQueueImpl::Mode::Write) return nullptr`. However, `ShmQueue::write_acquire()` does:

```cpp
void* ShmQueue::write_acquire(std::chrono::milliseconds timeout) noexcept
{
    if (!pImpl || !pImpl->producer())
        return nullptr;
```

If a `ShmQueue` was created via `from_consumer()` (read-only) and someone calls `write_acquire()` on the `QueueWriter` interface (possible via the `ShmQueue final` type casting), it correctly returns nullptr because `pImpl->producer()` is null. This is safe.

But the inverse: `ShmQueue::read_acquire()` on a write-only queue (from_producer) correctly returns nullptr because `pImpl->consumer()` is null. This is fine. No real bug here — the guard mechanism works.

---

### HR-05: `ProducerAPI::open_inbox()` called from GIL-held Python context, but `InboxClient::start()` calls `zmq_connect()` — blocks the interpreter thread
> ✅ FIXED 2026-03-10 — Added `py::gil_scoped_release` around `messenger_->query_role_info()` in `open_inbox()` in all three API implementations (producer/consumer/processor).
**File**: `src/producer/producer_api.cpp`, lines 305–364

`open_inbox()` is a Python-callable method. The call chain is:
1. `messenger_->query_role_info()` — synchronous broker request (has its own timeout)
2. `InboxClient::start()` — calls `zmq_connect()` under the GIL

`zmq_connect()` is documented as non-blocking (it initiates the connection asynchronously in ZMQ's I/O thread). However, calling it from the script thread while holding the GIL means the broker query (step 1) is also holding the GIL. The `query_role_info()` function presumably releases the GIL internally, but this path should be verified.

More importantly: `wait_for_role()` at line 366 calls `py::gil_scoped_release` inside the polling loop (line 376), correctly releasing the GIL during the wait. This is correct. But `open_inbox()` does NOT release the GIL around the broker round-trip. If the broker is slow or unreachable, this stalls the Python interpreter for up to 1000 ms (the hardcoded timeout at line 316).

**Recommended fix**: Wrap the `messenger_->query_role_info()` call inside `open_inbox()` with a `py::gil_scoped_release`.

---

### HR-06: `ZmqQueue::stop()` joins threads while still holding no lock — `recv_cv_.notify_all()` is called before thread signaling is complete
> ✅ FIXED 2026-03-10 — Added clarifying comment in `ZmqQueue::stop()` explaining `recv_cv_.notify_all()` wakes `read_acquire()` callers, not the recv_thread_ itself (which uses ZMQ_RCVTIMEO).
**File**: `src/utils/hub/hub_zmq_queue.cpp`, lines 774–795 (`stop()`)

```cpp
pImpl->recv_stop_.store(true, std::memory_order_release);
pImpl->send_stop_.store(true, std::memory_order_release);
pImpl->recv_cv_.notify_all();
pImpl->send_cv_.notify_all();
```

`recv_stop_` is stored with `memory_order_release`, and the notify happens after. The `recv_thread_` checks `recv_stop_` with `memory_order_relaxed`. The C++ memory model guarantees the release-store is visible before the relaxed load only if the thread observes the notification and then reads the flag — but `notify_all()` and the `recv_stop_` load are not atomic operations on the same object, so the happens-before chain relies on the condvar internal mutex.

In `run_recv_thread_()`, `recv_stop_` is checked in the `while (!recv_stop_.load(std::memory_order_relaxed))` loop (line 233), not inside the condvar predicate. The recv thread does not use the condvar for its main loop — it uses `ZMQ_RCVTIMEO` (100ms) to periodically return from `zmq_recv`. The `recv_cv_.notify_all()` in `stop()` is therefore effective only for `read_acquire()` (which does use `recv_cv_`). The recv thread itself wakes up at most 100ms after `recv_stop_` is set (from the ZMQ timeout). This is safe but the `notify_all` on `recv_cv_` is misleading — it wakes up any blocked `read_acquire()`, not the recv_thread itself.

**Low-severity finding**: The notification is correct for `read_acquire()` unblocking, but the design comment implies it also signals the recv thread, which is incorrect. The recv thread relies on the ZMQ timeout, not the condvar.

---

### HR-07: `ProducerScriptHost::stop_role()` — `queue_->stop()` called AFTER `out_producer_->stop()` — sender may reference dead producer state
**File**: `src/producer/producer_script_host.cpp`, lines 514–526

```cpp
if (queue_)
{
    queue_->stop();    // ← stop ZMQ send_thread_ (drains ring, sends remaining)
    queue_.reset();
}

if (out_producer_.has_value())
{
    out_producer_->stop();  // ← stop broker connection
    out_producer_->close();
    out_producer_.reset();
}
```

The comment at line 512 says "stop the transport queue before stopping the producer connection so any in-flight send_thread_ work drains cleanly." This sequencing is **correct** for ZMQ transport: drain the send ring first, then tear down the broker. For SHM transport, `queue_->stop()` is a no-op (ShmQueue inherits QueueWriter::stop() which does nothing), so order doesn't matter.

No bug here, but the comment is worth preserving.

---

### HR-08: `InboxClient::send()` sets `ZMQ_RCVTIMEO` on every call for ACK receive — then does not restore it
**File**: `src/utils/hub/hub_inbox_queue.cpp`, lines 771–772

```cpp
const int ack_ms = static_cast<int>(ack_timeout.count());
zmq_setsockopt(pImpl->socket, ZMQ_RCVTIMEO, &ack_ms, sizeof(ack_ms));
```

After `send()` returns (with or without ACK), the socket retains the last `ZMQ_RCVTIMEO` setting. If `ack_timeout` differs between calls, the effective receive timeout on the next `send(0)` (fire-and-forget) call is the last non-zero timeout set, not -1 (block) or 0 (immediate). For fire-and-forget (`ack_timeout.count() <= 0`, line 767 returns early before setting `ZMQ_RCVTIMEO`), this is fine. But if a caller alternates between `send(5000)` and `send(0)`, the timeout state is not cleared between calls.

This is not a correctness issue for the current usage pattern, but it is a latent bug for callers that vary `ack_timeout`.

---

## MEDIUM

### MR-01: Code duplication: `hub_inbox_queue.cpp` duplicates all field layout, pack/unpack, and frame-size helpers from `hub_zmq_queue.cpp`
**Files**: `src/utils/hub/hub_inbox_queue.cpp` lines 44–125; `src/utils/hub/hub_zmq_queue.cpp` lines 44–100

The comments in both files acknowledge "deduplication deferred per design §7.6." The duplicated code includes:
- `is_valid_inbox_type_str()` vs `is_valid_type_str()` — identical logic
- `inbox_field_elem_size()` vs `field_elem_size()` — identical
- `inbox_field_align()` vs `field_align()` — identical
- `compute_inbox_field_layout()` vs `compute_field_layout()` — identical algorithm
- `inbox_max_frame_size()` vs `schema_max_frame_size()` — identical formula
- `inbox_pack_field()` vs `ZmqQueueImpl::pack_field()` — identical except for parameter type (`InboxFieldDesc` vs `FieldDesc`)
- `inbox_unpack_field()` vs `ZmqQueueImpl::unpack_field()` — identical

Any bug fixed in one location must also be fixed in the other. The `FieldDesc` and `InboxFieldDesc` structs are identical in content. This should be consolidated into a shared internal header.

---

### MR-02: `InboxQueue::recv_one()` sequence-gap tracking is per-sender-aggregate, not per-sender
**File**: `src/utils/hub/hub_inbox_queue.cpp`, lines 511–519

```cpp
if (pImpl->seq_initialized_ && seq > pImpl->expected_seq_)
    pImpl->recv_gap_count_.fetch_add(seq - pImpl->expected_seq_, ...);
pImpl->expected_seq_ = seq + 1;
```

A single ROUTER socket receives from multiple DEALER senders. Each sender has its own independent monotonic sequence counter (`send_seq_`). The gap detector tracks a single global `expected_seq_` across all senders. When sender B sends seq=5 and sender A had previously sent seq=10, the gap detector records a spurious gap of 5. Similarly, if sender A restarts (seq resets to 0), `recv_gap_count` is inflated.

`ZmqQueue` (a point-to-point PULL socket) has the same code and the same "restart → silent ignore" comment (lines 302–310), but there it is correct because there is only one sender. For InboxQueue with multiple senders, this gap count is meaningless.

**Recommended fix**: Track per-sender expected_seq using an `unordered_map<string, uint64_t>` keyed by sender_id.

---

### MR-03: `ProducerScriptHost::start_role()` — `on_init` is called BEFORE `loop_thread_` is started; if `on_init` takes longer than one period, first iteration timestamp is wrong
**File**: `src/producer/producer_script_host.cpp`, lines 479–484

```cpp
call_on_init_common_();

if (!core_.running_threads.load())
    return true;

loop_thread_ = std::thread([this] { run_loop_(); });
```

`run_loop_()` computes `next_deadline` relative to `std::chrono::steady_clock::now()` at the start of the loop, so `on_init` time is not counted against the first period. This is correct behavior.

However, the `ctrl_thread_` is started **before** `on_init` (line 474), and `ctrl_thread_` begins sending heartbeats and polling peer events immediately. If `on_init` is slow (Python import, heavy computation), the broker may time out before the producer is ready. This is an operational concern, not a crash, but worth documenting.

---

### MR-04: `ZmqQueue::write_acquire()` — Drop policy returns `write_buf_` pointer after checking capacity, but does NOT verify the buffer is not already in-use (double acquire)
> ❌ FALSE POSITIVE — Single-producer architecture makes double-acquire architecturally impossible. The guard in `write_commit()` is a forward-looking reserve for future multi-writer expansion per user confirmation.
**File**: `src/utils/hub/hub_zmq_queue.cpp`, lines 841–867

The queue contract ("only ONE outstanding acquire at a time") is documented, but there is no enforcement mechanism. If a caller calls `write_acquire()` twice without an intervening `write_commit()`/`write_discard()`, the second call returns the same `write_buf_` pointer (overwriting the first). The `write_commit()` check at line 879 is a post-hoc error log, not prevention.

For the ShmQueue, double-acquire is similarly un-enforced. An assertion (`assert(!has_pending_write_)`) or an `active_write_` flag would catch misuse early.

---

### MR-05: `ShmQueue::capacity()` may throw `std::runtime_error` — but `capacity()` is declared on `QueueReader`/`QueueWriter` without `noexcept`
> ✅ FIXED 2026-03-10 — `ConsumerAPI::in_capacity()` (declared `noexcept`) now catches exceptions from `reader_->capacity()` via try/catch, returning 0 on failure. The catch was already present; code path confirmed safe.
**File**: `src/utils/hub/hub_shm_queue.cpp`, lines 306–322

```cpp
size_t ShmQueue::capacity() const
{
    if (!pImpl) throw std::runtime_error("ShmQueue::capacity(): not connected");
    ...
    throw std::runtime_error("ShmQueue::capacity(): failed to query DataBlock metrics");
}
```

`QueueReader::capacity()` and `QueueWriter::capacity()` are not declared `noexcept`, so throwing is technically permitted. However, callers of the interface (e.g. `ConsumerAPI::in_capacity()`) call this without any exception handling:

```cpp
[[nodiscard]] size_t in_capacity() const noexcept;  // declared noexcept in consumer_api.hpp:136
```

If `reader_->capacity()` is called via the noexcept `in_capacity()` wrapper and it throws, `std::terminate()` is invoked. The `QueueReader` interface should either be declared `noexcept` throughout, or the implementations must not throw.

---

### MR-06: `InboxQueue` has no mechanism to signal the inbox_thread_ to stop immediately — relies on 100ms polling interval
**File**: `src/producer/producer_script_host.cpp`, lines 762–803

`run_inbox_thread_()` polls with a 100ms timeout (`kPollTimeout`). On shutdown, `stop_role()` sets `core_.running_threads = false`, but the inbox thread only checks this condition after `recv_one()` returns (every 100ms). This adds up to 100ms to shutdown latency every time.

The `ZmqQueue` recv thread uses `ZMQ_RCVTIMEO=100ms` for the same reason, and it is also a known cost. But `InboxQueue` compounds this with the fact that `stop()` on the queue (which would cause ETERM) is called AFTER joining the thread (see CR-02), so the inbox thread cannot use ETERM as a fast exit.

---

### MR-07: `ProducerAPI::snapshot_metrics_json()` accesses `producer_->shm()` from `ctrl_thread_` without verifying `out_producer_` lifecycle
**File**: `src/producer/producer_api.cpp`, lines 213–247

`snapshot_metrics_json()` is called from `ctrl_thread_` (heartbeat interval). It accesses `producer_` (a raw pointer set via `set_producer()`). During `stop_role()`:
1. `core_.running_threads.store(false)` — ctrl_thread_ loop may be in the middle of sending a heartbeat
2. `ctrl_thread_.join()` — waits for ctrl_thread_ to exit
3. `api_.set_producer(nullptr)` (line 529) — nulls the pointer

The join at step 2 ensures `producer_` is not read after being nulled. This ordering is **correct**. However, the ctrl_thread_ calls `snapshot_metrics_json()` in its `periodic_tasks` which runs every `heartbeat_interval_ms`. If `heartbeat_interval_ms == 0`, the task fires on every poll cycle and could be very frequent. The race window is small but the design is fragile because `snapshot_metrics_json()` has no null guard that would be safe after `set_producer(nullptr)`.

**Note**: The join ensures safety here. Flag as low risk but worth a defensive null check in `snapshot_metrics_json()`.

---

### MR-08: `consumer_script_host.hpp` comment inconsistency — references obsolete `run_loop_shm_`
> ✅ FIXED 2026-03-10 — Updated header comment from `run_loop_shm_` to `run_loop_()` in `consumer_script_host.hpp`.
**File**: `src/consumer/consumer_script_host.hpp`, line 9

```
 * Inherits the common do_python_work() skeleton from PythonRoleHostBase.
 * This file provides the consumer-specific virtual hook overrides:
 *  - Demand-driven consumption loop (run_loop_shm_)    ← STALE
```

And line 108:
```cpp
void run_loop_();        ///< Unified transport-agnostic loop (replaces run_loop_shm_ + run_loop_zmq_)
```

The file header still says `run_loop_shm_`, which was the old name. `run_loop_()` is the current implementation. Minor but confusing.

---

### MR-09: Design inconsistency — `QueueReader::is_running()` defaults to `return true` for ShmQueue, but ShmQueue has no running concept
**File**: `src/include/utils/hub_queue.hpp`, lines 197–198

```cpp
virtual bool is_running() const noexcept { return true; }  // QueueReader base default
```

`ShmQueue` inherits this default and returns `true` even before any DataBlock is attached. A caller checking `queue_reader->is_running()` on a ShmQueue always gets `true` regardless of state. For `ZmqQueue` the override correctly tracks `running_`. The inconsistency means `is_running()` cannot be used uniformly across the abstraction.

---

### MR-10: `write_acquire()` on a ZmqQueue in write mode returns `write_buf_` even when `send_stop_` is true (after stop())
> ✅ FIXED 2026-03-10 — Added early return `if (send_stop_.load(relaxed)) return nullptr;` in `write_acquire()` Drop path. `send_stop_` is used (not `running_`) to allow pre-start offline fills while still blocking post-stop writes.
**File**: `src/utils/hub/hub_zmq_queue.cpp`, lines 837–867

After `stop()` is called, `running_` is false. But `write_acquire()` only checks `pImpl->mode != ZmqQueueImpl::Mode::Write`, not `running_`. A caller that calls `write_acquire()` after `stop()` will get a valid buffer pointer back. If `write_commit()` is then called, it acquires `send_mu_` and copies to the send ring — but `send_thread_` has already exited. The items sit in the ring forever (no drop accounting). Eventually `queue_.reset()` in `stop_role()` destroys the ring harmlessly.

This is unlikely to happen in practice (the loop checks `running_threads` before calling `write_acquire()`), but it is a latent correctness issue.

---

## LOW

### LR-01: `InboxQueue::send_ack()` sends a three-part message to `current_sender_id_` even when the sender has disconnected — silently ignored by ZMQ
**File**: `src/utils/hub/hub_inbox_queue.cpp`, lines 557–572

If the DEALER has disconnected between `recv_one()` and `send_ack()`, the three-part ZMQ send will fail or be buffered indefinitely (depending on HWM). The return value of `zmq_send` on frames 1 and 2 are checked (lines 564–568), but the behavior when the peer is gone depends on the ZMQ HWM and socket type. With ROUTER sockets and a dead identity, ZMQ drops the message silently (with `ZMQ_ROUTER_MANDATORY=0`, the default). This is acceptable behavior, but there should be a comment explaining this is intentional.

---

### LR-02: `ShmQueue::ShmQueue(ShmQueue&&) noexcept = default` — does not reset `dbc_ref`/`dbp_ref` in the moved-from object
**File**: `src/utils/hub/hub_shm_queue.cpp`, line 166

The defaulted move constructor moves `pImpl` out, leaving the moved-from `ShmQueue` with a null `pImpl`. Calls to the moved-from queue after the move correctly check `!pImpl`. Safe, but the comment at line 160–161 in `hub_shm_queue.hpp` ("start()/stop()/is_running() — inherited no-op implementations") should note this.

---

### LR-03: `hub_processor.cpp` line 83 — `sleep_for(10ms)` while handler is null uses busy-wait pattern
**File**: `src/utils/hub/hub_processor.cpp`, line 83

```cpp
std::this_thread::sleep_for(std::chrono::milliseconds{10});
continue;
```

When no handler is installed, the process thread polls every 10ms. If the Processor is used as a long-lived component with a handler installed at some point, this 10ms delay adds to the first-data latency. A condvar or semaphore would be more precise. Minor performance concern.

---

### LR-04: `ProducerAPI::stop()` stores `shutdown_requested` with `memory_order_relaxed` — should be `memory_order_release`
> ✅ FIXED 2026-03-10 — Updated all three API `stop()` methods (producer/consumer/processor) to use `memory_order_release` for both `shutdown_flag_` and `shutdown_requested_` stores.
**File**: `src/producer/producer_api.cpp`, lines 42–47

```cpp
void ProducerAPI::stop()
{
    if (shutdown_flag_)
        shutdown_flag_->store(true, std::memory_order_relaxed);
    if (shutdown_requested_)
        shutdown_requested_->store(true, std::memory_order_relaxed);
}
```

The consuming thread (`run_loop_()`) reads `shutdown_requested_` with `memory_order_acquire` (via `core_.shutdown_requested.load()`). The store should use `memory_order_release` to form a proper release-acquire pair. With `relaxed`+`acquire`, the loop may not see side effects that happened before `stop()` was called. This is unlikely to cause visible bugs (the flag will propagate eventually), but violates the intended synchronization semantics.

---

### LR-05: `parse_on_produce_return()` logs LOGGER_ERROR for wrong return type but still returns `false` (discard) — the error counter is not incremented
> ✅ FIXED 2026-03-10 — `parse_on_produce_return()` now returns `pair<bool,bool>{commit, is_err}`. `call_on_produce_()` calls `api_.increment_script_errors()` when `is_err==true`.
**File**: `src/producer/producer_script_host.cpp`, lines 37–45

```cpp
bool parse_on_produce_return(const py::object &ret)
{
    ...
    LOGGER_ERROR("[prod] on_produce() must return bool or None — treating as skip");
    return false;
}
```

The error is logged but `api_.increment_script_errors()` is not called from this path. `call_on_produce_()` calls `parse_on_produce_return()` and only increments `script_errors_` when an exception is thrown (line 599). A script returning an integer (a common mistake, e.g. `return 1`) silently discards the frame and the error count stays at 0.

---

### LR-06: `InboxClient::acquire()` zero-initializes the write buffer on every call — unnecessary if send() was called since last acquire
**File**: `src/utils/hub/hub_inbox_queue.cpp`, lines 716–722

```cpp
void* InboxClient::acquire() noexcept
{
    if (!pImpl || !pImpl->socket) return nullptr;
    std::fill(pImpl->write_buf_.begin(), pImpl->write_buf_.end(), std::byte{0});
    return pImpl->write_buf_.data();
}
```

Zero-initializing before every acquire prevents information leakage across messages, but for callers that fill all fields completely, this is wasted work. A comment explaining the intentional security/correctness rationale would help.

---

### LR-07: `consumer_script_host.cpp` line 400 — `api_.clear_inbox_cache()` called from `clear_role_pyobjects()` but the consumer API has no open_inbox connected to any producer through the consumer's own InboxQueue (no inbox on consumer side)
> ✅ FIXED 2026-03-10 — Added clarifying comment in `clear_role_pyobjects()` explaining that `clear_inbox_cache()` closes outgoing InboxClient connections (the consumer may have called `api.open_inbox()` to send to other roles).
**File**: `src/consumer/consumer_script_host.cpp`, lines 397–402

```cpp
void ConsumerScriptHost::clear_role_pyobjects()
{
    api_.clear_inbox_cache();   // ← clears InboxClient handles (outgoing sends)
    py_on_consume_ = py::none();
    slot_type_     = py::none();
}
```

This is correct — the consumer may open_inbox() to reach other roles (producers or other consumers). `clear_inbox_cache()` stops those outgoing InboxClient connections. No bug, but the comment should clarify that this refers to outgoing inbox connections, not the consumer's own inbox (consumers have no inbox in the current design).

---

## Interface Consistency Analysis

### IC-01: `QueueReader`/`QueueWriter` — ShmQueue inherits both, exposing mixed read+write API on read-only queue instances
**Files**: `src/include/utils/hub_shm_queue.hpp`, `src/include/utils/hub_queue.hpp`

`ShmQueue` inherits both `QueueReader` and `QueueWriter`. Factories return `unique_ptr<QueueReader>` or `unique_ptr<QueueWriter>`, hiding the dual inheritance. This is architecturally correct.

However, `ZmqQueue` also inherits both but has explicit mode guards returning nullptr for cross-mode calls. The factory contract is the intended safeguard. The API is consistent across ShmQueue and ZmqQueue — callers that hold `unique_ptr<QueueReader>` cannot accidentally call `write_acquire`.

No actionable finding; the design is intentional and documented.

---

### IC-02: `InboxHandle::send()` returns `int` but `InboxClient::send()` returns `uint8_t` — inconsistent return type across layers
**Files**: `src/include/utils/script_host_helpers.hpp`, line 536; `src/include/utils/hub_inbox_queue.hpp`, line 238

```cpp
// InboxHandle (Python-facing)
int send(int timeout_ms = 5000);

// InboxClient (C++ API)
uint8_t send(std::chrono::milliseconds ack_timeout = ...);
```

`InboxHandle::send()` promotes to `int` for Python ergonomics (py::int_ can't hold uint8_t directly). The conversion at line 541 (`static_cast<int>(client_->send(...))`) is correct. Not a bug, but the dual types add cognitive load. A type alias or comment would help.

---

### IC-03: HEP-CORE-0018 §6.1 `on_produce` contract — `False` return triggers `write_discard()` → loop CONTINUES, as documented
**Files**: `src/producer/producer_script_host.cpp`, lines 678–687; HEP-CORE-0018

```cpp
if (commit)
{
    queue_->write_commit();
    api_.increment_out_written();
}
else
{
    queue_->write_discard();
    api_.increment_drops();
}
```

The implementation correctly follows HEP-0018: `True/None` → commit; `False` → discard+continue. The `increment_drops()` on `False` return is intentional (discarded frames are counted). Implementation matches specification.

---

### IC-04: `QueueReader::last_seq()` — ShmQueue returns slot_id (commit_index, global monotonic), ZmqQueue returns wire frame seq (sender-local counter); semantics differ
**Files**: `src/utils/hub/hub_shm_queue.cpp`, line 225; `src/utils/hub/hub_zmq_queue.cpp`, line 915

✅ FIXED 2026-03-09 — Added detailed `@par` documentation in `hub_queue.hpp` `last_seq()` docstring explaining the transport-specific semantics difference. No runtime change needed; this is a documentation-only fix.

`ShmQueue::last_seq()` returns the DataBlock slot commit index (global across all producers for this block). `ZmqQueue::last_seq()` returns the sender's wire frame sequence number (sender-local, starts at 0 per sender lifetime). These are not directly comparable. Callers that use `last_seq()` for gap detection must be aware of this semantic difference.

The `ConsumerAPI::update_last_seq()` call in `run_loop_()` (consumer_script_host.cpp:495) stores whichever queue's value is in use. The Python-level `api.last_seq()` therefore has transport-specific semantics. This is a **design consistency gap** that should be documented in the API docstring.

---

## Cross-Platform Issues

### CP-01: `hub_inbox_queue.cpp` uses `char id_buf[256]` stack buffer for ZMQ identity — identity can be up to 255 bytes per ZMQ spec; the buffer is correctly 256 bytes but null-termination is assumed
**File**: `src/utils/hub/hub_inbox_queue.cpp`, lines 448–462

```cpp
char id_buf[256]{};
int id_rc = zmq_recv(pImpl->socket, id_buf, sizeof(id_buf) - 1, 0);
```

`sizeof(id_buf) - 1 = 255` — this correctly limits reception to 255 bytes, leaving room for null. ZMQ identities are not null-terminated binary blobs, but the code correctly uses `id_rc` as the length when constructing `sender_id` (line 463). This is safe.

However, if a ZMQ identity is exactly 255 bytes, `zmq_recv` returns 255. The actual message size (from `zmq_recv`'s return value = 255) may be truncated from a longer identity, since `zmq_recv` with a buffer limit returns the number of **bytes received** (not the total message size). If the identity is >255 bytes, the excess is silently truncated. Per ZMQ spec, identity is max 255 bytes, so this buffer is adequate. No bug.

---

### CP-02: `zmq_setsockopt(pImpl->socket, ZMQ_RCVTIMEO, &timeout_ms, sizeof(timeout_ms))` — `timeout_ms` is `int`, ZMQ_RCVTIMEO expects `int` — correct on all platforms
**File**: `src/utils/hub/hub_inbox_queue.cpp`, line 442; `src/utils/hub/hub_zmq_queue.cpp`, line 229

Type is `int` on both sides. POSIX/Windows compatible. No issue.

---

### CP-03: `ZmqQueueImpl::last_mismatch_warn_` uses `std::chrono::steady_clock::time_point` default-initialized to epoch — first comparison subtracts epoch from `now()`, which may be a large negative duration on some platforms
**File**: `src/utils/hub/hub_zmq_queue.cpp`, lines 158–159, 285–292

```cpp
std::chrono::steady_clock::time_point last_mismatch_warn_{};  // epoch (or unspecified)
...
if (now - last_mismatch_warn_ >= kMismatchWarnInterval)
```

`steady_clock::time_point` default-constructs to the epoch of `steady_clock`, which is typically zero (or an arbitrary reference point). `now()` will always be >= epoch + 1 second, so the first mismatch always logs. This is the intended behavior (first warning fires immediately). No bug.

---

## Summary Table

| ID | Severity | File(s) | Title |
|----|----------|---------|-------|
| CR-02 | CRITICAL | hub_inbox_queue.cpp, producer_script_host.cpp | InboxQueue::stop() closes socket from wrong thread while inbox_thread_ may be in recv_one() |
| CR-03 | CRITICAL | hub_shm_queue.cpp:249–259 | ShmQueue::write_commit() checksums AFTER commit() — reader sees uncommitted checksum |
| HR-01 | HIGH | producer_api.hpp:185, consumer_api.hpp:172, processor_api.hpp | script_errors_ is non-atomic, written from multiple threads |
| HR-02 | HIGH | consumer_api.hpp:160, consumer_script_host.cpp | ConsumerAPI::reader_ raw pointer read from ctrl_thread_ without atomic/lock |
| HR-03 | HIGH | hub_inbox_queue.cpp:441–443 | ZMQ_RCVTIMEO set per-call on socket potentially shared with stop() |
| HR-05 | HIGH | producer_api.cpp:316 | open_inbox() blocks interpreter thread (GIL held) for broker round-trip up to 1s |
| MR-01 | MEDIUM | hub_inbox_queue.cpp | All wire-format helpers duplicated from hub_zmq_queue.cpp — deferred dedup creates maintainability risk |
| MR-02 | MEDIUM | hub_inbox_queue.cpp:511–519 | InboxQueue gap counter is per-ROUTER-socket, not per-sender — meaningless for multi-sender |
| MR-04 | MEDIUM | hub_zmq_queue.cpp:841–867 | No double-acquire detection on write_acquire() |
| MR-05 | MEDIUM | hub_shm_queue.cpp:306–322 | ShmQueue::capacity() throws, but ConsumerAPI::in_capacity() declares noexcept — std::terminate if called on disconnected queue |
| MR-07 | MEDIUM | producer_api.cpp:213 | snapshot_metrics_json() accesses producer_ without null guard after set_producer(nullptr) — currently safe via join order but fragile |
| MR-10 | MEDIUM | hub_zmq_queue.cpp:837–867 | write_acquire() does not check running_ — succeeds after stop() |
| LR-01 | LOW | hub_inbox_queue.cpp:557 | send_ack() comment missing: silent drop to disconnected DEALER is intentional |
| LR-04 | LOW | producer_api.cpp:42–47 | ProducerAPI::stop() stores shutdown flags with memory_order_relaxed; should be release |
| LR-05 | LOW | producer_script_host.cpp:43 | Wrong return type from on_produce logs error but doesn't increment script_error_count |
| LR-07 | LOW | consumer_script_host.cpp:400 | Comment missing: clear_inbox_cache() clears outgoing InboxClient handles, not consumer's own inbox |
| IC-04 | MEDIUM | hub_shm_queue.cpp:225, hub_zmq_queue.cpp:915 | QueueReader::last_seq() has different semantics for ShmQueue vs ZmqQueue — should be documented | ✅ FIXED 2026-03-09 |
| MR-08 | LOW | consumer_script_host.hpp:9 | Stale reference to run_loop_shm_ in file header comment |

