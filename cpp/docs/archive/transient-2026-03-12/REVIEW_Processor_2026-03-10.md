# Processor Role Code Review

**Scope:** `src/processor/` (all files) + cross-check against `src/consumer/`, `src/producer/`, `src/scripting/`
**Date:** 2026-03-10
**Status:** ✅ CLOSED 2026-03-10

---

## Summary Table

| ID | Severity | Area | Short Description | Status |
|----|----------|------|-------------------|--------|
| PR-01 | HIGH | Correctness | Inbox not advertised via REG_REQ — `open_inbox()` always returns None | ✅ FIXED 2026-03-10 |
| PR-02 | HIGH | Correctness | Inbox start failure non-fatal — silently continues without inbox | ✅ FIXED 2026-03-10 |
| PR-03 | HIGH | Shutdown | `cleanup_on_start_failure` missing ctrl_thread_ join + owned queue cleanup | ✅ FIXED 2026-03-10 |
| PR-04 | HIGH | Shutdown | `processor_.reset()` called with GIL held — potential block | ✅ FIXED 2026-03-10 |
| PR-05 | MEDIUM | Correctness | Dead condition in timeout handler: `msgs.empty() && config_.timeout_ms <= 0` always false | ✅ FIXED 2026-03-10 |
| PR-06 | MEDIUM | Correctness | `verify_checksum` silently ignored for broker-negotiated ZMQ input path | ✅ FIXED 2026-03-10 |
| PR-07 | MEDIUM | API Parity | `ProcessorAPI` missing `last_seq()`, `in_capacity()`, `in_policy()`, `out_capacity()`, `out_policy()` | ✅ FIXED 2026-03-10 |
| PR-08 | MEDIUM | API Parity | `last_seq` never updated — hub::Processor drives the loop, no `update_last_seq()` call in handler | ✅ FIXED 2026-03-10 (reads live from QueueReader, no stored member needed) |
| PR-09 | MEDIUM | API Parity | `api.verify_checksum()` Python method not exposed (ConsumerAPI has it) | ✅ FIXED 2026-03-10 |
| PR-10 | MEDIUM | Correctness | `inbox_schema_slot_size_` passed to `make_inbox_slot_view_` — use `inbox_queue_->item_size()` instead | ✅ FIXED 2026-03-10 |
| PR-11 | MEDIUM | Thread Safety | `core_.running_threads.store(false)` in handler lambda — wrong flag, wrong ordering | ✅ FIXED 2026-03-10 |
| PR-12 | MEDIUM | Config | `timeout_ms == 0` (non-blocking) silently becomes 5000 ms in ProcessorOptions | ✅ FIXED 2026-03-10 |
| PR-13 | LOW | Shutdown | `processor_.reset()` with GIL held after `call_on_stop_common_()` | ✅ FIXED 2026-03-10 (part of PR-04) |
| PR-14 | LOW | Correctness | `loop_timing` / `target_period_ms` parsed but never wired — silently dead | ✅ FIXED 2026-03-10 (LOGGER_WARN added) |
| PR-15 | LOW | Error Handling | Resource leak if `main_thread_release_.emplace()` throws after `processor_->start()` | ✅ FIXED 2026-03-10 (resolved by PR-03 fix) |
| PR-16 | LOW | API Parity | `api_.set_messenger(nullptr)` never called in `stop_role()` | ✅ FIXED 2026-03-10 |
| PR-17 | INFO | Ordering | Heartbeat enqueued before `out_producer_->start_embedded()` — confirmed safe (false positive) | ✅ FALSE POSITIVE |
| PR-18 | LOW | Config | Inbox packing fallback hardcoded `"aligned"` — should use `config_.in_zmq_packing` | ✅ FIXED 2026-03-10 |
| PR-19 | INFO | Correctness | `parse_on_process_return` does not increment `script_errors_` on wrong return type | ✅ FIXED 2026-03-10 |
| PR-20 | INFO | Docs | Comment "Only stop owned SHM queues" inaccurate — direct ZMQ PULL also owned | ✅ FIXED 2026-03-10 |

---

## Detailed Findings

### PR-01 — HIGH — Inbox not advertised via REG_REQ

**File:Line:** `src/processor/processor_script_host.cpp` (inbox setup block, after line 569)

**Description:**
The processor builds `inbox_queue_` and calls `inbox_queue_->start()`, but never writes
`actual_endpoint()`, schema JSON, or packing into `out_opts` before `hub::Producer::create()`.
Therefore REG_REQ carries no `inbox_endpoint`. Any peer calling `api.open_inbox(proc_uid)` gets
`py::none()` because `ROLE_INFO_ACK` returns `found=false` when `entry.inbox_endpoint.empty()`.

**Evidence:**
```cpp
// inbox setup builds inbox_queue_ and starts it, but out_opts never gets:
//   out_opts.inbox_endpoint    = inbox_queue_->actual_endpoint();
//   out_opts.inbox_schema_json = spec_json.dump();
//   out_opts.inbox_packing     = packing;
// Compare: producer_script_host.cpp:215–217 sets all three
```

**Fix:** Move inbox setup before `hub::Producer::create()` and set the three `out_opts` fields after `inbox_queue_->start()` succeeds.

---

### PR-02 — HIGH — Inbox start failure is silently non-fatal

**File:Line:** `src/processor/processor_script_host.cpp:596–604`

**Description:**
When `inbox_queue_->start()` fails, the code logs an error and resets `inbox_queue_` to nullptr,
then proceeds to start the processor. Scripts expecting `on_inbox` to fire receive nothing.
Producer returns `false` on inbox start failure (`producer_script_host.cpp:209–213`).

**Evidence:**
```cpp
if (!inbox_queue_ || !inbox_queue_->start())
{
    LOGGER_ERROR("[proc] Failed to start InboxQueue at '{}'", ep);
    if (inbox_queue_) inbox_queue_.reset();
    // ← continues silently; processor still starts
}
```

**Fix:** Return `false` from `start_role()` on inbox start failure, matching the producer pattern.

---

### PR-03 — HIGH — `cleanup_on_start_failure` missing ctrl_thread_ join + owned queue cleanup

**File:Line:** `src/processor/processor_script_host.cpp:778–794`

**Description:**
`cleanup_on_start_failure()` joins `inbox_thread_` and stops `inbox_queue_`, but:
1. Does NOT join `ctrl_thread_` — `ctrl_thread_` may be running and accessing `out_producer_` / `in_consumer_` which are then destroyed below → use-after-free.
2. Does NOT stop/reset `in_queue_` / `out_queue_` — owned queues allocated during queue setup but not cleaned up if Processor::create() fails.

**Evidence:**
```cpp
void ProcessorScriptHost::cleanup_on_start_failure()
{
    if (inbox_thread_.joinable()) inbox_thread_.join();
    if (inbox_queue_) { inbox_queue_->stop(); inbox_queue_.reset(); }
    // ← ctrl_thread_ NOT joined
    // ← in_queue_ / out_queue_ NOT cleaned up
    if (out_producer_.has_value()) { ... }
    if (in_consumer_.has_value()) { ... }
}
```

**Fix:**
1. `core_.running_threads.store(false)` + `core_.notify_incoming()` at top.
2. Join `ctrl_thread_` before stopping producer/consumer.
3. Stop and reset `in_queue_` and `out_queue_`.

---

### PR-04 — HIGH — `processor_.reset()` called with GIL held

**File:Line:** `src/processor/processor_script_host.cpp:748`

**Description:**
`stop_role()` calls `call_on_stop_common_()` (GIL held) then `processor_.reset()` also with GIL held.
The Processor destructor calls `stop()` again internally (idempotent), but holding the GIL while
destroying a pImpl object with an internal thread is an unnecessary risk. If `process_thread_` is
somehow still live at destructor time, the GIL blocks all Python callbacks.

**Fix:** Move `processor_.reset()` inside the GIL-release block in `stop_role()`, immediately after `processor_->stop()`.

---

### PR-05 — MEDIUM — Dead condition in timeout handler

**File:Line:** `src/processor/processor_script_host.cpp:669`

**Description:**
The timeout handler lambda is only installed when `config_.timeout_ms > 0`, yet inside it checks:
```cpp
if (msgs.empty() && config_.timeout_ms <= 0)
    return false;
```
`config_.timeout_ms <= 0` is always false inside this lambda. Dead code from copy-paste.

**Fix:** Remove the dead branch. If early-return on empty messages with no output is desired, use `if (msgs.empty() && !out_data) return false;`.

---

### PR-06 — MEDIUM — `verify_checksum` silently ignored for broker-negotiated ZMQ input

**File:Line:** `src/processor/processor_script_host.cpp:521–524`

**Description:**
`set_verify_checksum()` is only called in the SHM branch. For broker-negotiated ZMQ input,
`verify_checksum=true` in config silently has no effect.

**Fix:** Add `LOGGER_WARN` when `config_.verify_checksum` is true but the input path is ZMQ (TCP provides integrity, checksum not applicable).

---

### PR-07 — MEDIUM — ProcessorAPI missing queue-state accessors

**File:Line:** `src/processor/processor_api.hpp`

**Description:**
`ConsumerAPI` exposes `last_seq()`, `in_capacity()`, `in_policy()`. `ProducerAPI` exposes
`out_capacity()`, `out_policy()`. `ProcessorAPI` has none of these. Scripts cannot query ring
size or slot index.

**Missing:** `last_seq()`, `in_capacity()`, `in_policy()`, `out_capacity()`, `out_policy()`

**Fix:** Add atomic `in_q_` / `out_q_` pointers to `ProcessorAPI` (same pattern as `ConsumerAPI::reader_`). Expose the five methods via pybind11.

---

### PR-08 — MEDIUM — `last_seq` never updated in process handler

**File:Line:** `src/processor/processor_script_host.cpp:626–660`

**Description:**
Even if PR-07 adds `last_seq()`, the value would always be 0 — the raw_handler lambda never
calls `api_.update_last_seq()`. The consumer does this in `run_loop_()` after each `read_acquire()`.

**Fix:** Call `api_.update_last_seq(in_q_->last_seq())` at the top of the raw_handler lambda.
`last_seq()` reads an atomic counter — safe to call without the GIL.

---

### PR-09 — MEDIUM — `api.verify_checksum()` not exposed to Python

**File:Line:** `src/processor/processor_api.cpp` (pybind11 module)

**Description:**
`ConsumerAPI` exposes `api.verify_checksum(bool)` for runtime toggle. `ProcessorAPI` has no
equivalent, even though `verify_checksum` affects the SHM input path.

**Fix:** Add `set_verify_checksum(bool enable)` to `ProcessorAPI`, forward to
`in_q_->set_verify_checksum(enable, core_.has_fz)` (null-guarded). Expose in pybind11 module.

---

### PR-10 — MEDIUM — `inbox_schema_slot_size_` vs `InboxQueue::item_size()` in slot view

**File:Line:** `src/processor/processor_script_host.cpp:895` (run_inbox_thread_)

**Description:**
`make_inbox_slot_view_(item->data, inbox_schema_slot_size_)` uses the ctypes-computed size.
`InboxQueue::item_size()` is the actual decoded payload size. For "packed" packing these may
differ, causing `from_buffer_copy` to raise `ValueError` in Python.

**Fix:** Use `inbox_queue_->item_size()` as the size argument in `run_inbox_thread_()`.
Same fix applies to the consumer's `run_inbox_thread_()` for parity.

---

### PR-11 — MEDIUM — Wrong flag/ordering in `stop_on_script_error` paths

**File:Line:** `src/processor/processor_script_host.cpp:654–655, 694–695`

**Description:**
Both raw_handler and timeout_handler call `core_.running_threads.store(false)` on script error.
This is the wrong flag for triggering base-class loop exit — `core_.shutdown_requested` is what
the base-class `do_python_work()` wait loop checks. Also missing `memory_order_release`.

**Evidence:**
Compare: `ProducerScriptHost::run_inbox_thread_()` correctly uses:
```cpp
core_.shutdown_requested.store(true, std::memory_order_release);
```

**Fix:** Replace both instances with `core_.shutdown_requested.store(true, std::memory_order_release)`.

---

### PR-12 — MEDIUM — `timeout_ms == 0` maps to 5000 ms

**File:Line:** `src/processor/processor_script_host.cpp:612–614`

**Description:**
Config documents `timeout_ms`: `-1=infinite, 0=non-blocking, >0=ms`. But the mapping is:
```cpp
proc_opts.input_timeout = (config_.timeout_ms > 0)
    ? std::chrono::milliseconds{config_.timeout_ms}
    : std::chrono::milliseconds{5000};
```
`timeout_ms == 0` → 5000 ms, contradicting "non-blocking".

**Fix:**
```cpp
proc_opts.input_timeout = (config_.timeout_ms == 0)
    ? std::chrono::milliseconds{0}
    : (config_.timeout_ms > 0)
        ? std::chrono::milliseconds{config_.timeout_ms}
        : std::chrono::milliseconds{5000};
```

---

### PR-13 — LOW — `processor_.reset()` with GIL held (after on_stop)

**File:Line:** `src/processor/processor_script_host.cpp:748`

Already partially described in PR-04. The reset call itself is idempotent since `stop()` was
already called. The main risk is an unnecessary GIL hold during destruction of a complex object.

**Fix:** Part of PR-04 fix — move reset into the GIL-release block.

---

### PR-14 — LOW — `loop_timing` / `target_period_ms` silently dead

**File:Line:** `src/processor/processor_config.hpp:174–177`

**Description:**
These config fields are parsed, validated, and stored, but never wired to `ProcessorOptions`.
A user setting `"loop_timing": "fixed_rate"` will get no timing effect.

**Fix:** Add `LOGGER_WARN` at startup when `loop_timing != MaxRate`, indicating the field is not
yet implemented for the processor role (demand-driven). Or document it clearly in config comments.

---

### PR-15 — LOW — Potential resource leak if `main_thread_release_.emplace()` throws

**File:Line:** `src/processor/processor_script_host.cpp:716–722`

**Description:**
If `main_thread_release_.emplace()` throws after `processor_->start()`, the processor thread and
`ctrl_thread_` are running but `cleanup_on_start_failure()` (as noted in PR-03) doesn't stop them.

**Fix:** Depends on PR-03 fix. Once cleanup_on_start_failure is correct, this becomes low risk.

---

### PR-16 — LOW — `api_.set_messenger(nullptr)` never called in `stop_role()`

**File:Line:** `src/processor/processor_script_host.cpp:760–775`

**Description:**
`stop_role()` calls `api_.set_producer(nullptr)` and `api_.set_consumer(nullptr)` but never
`api_.set_messenger(nullptr)`. Prevents late Python callbacks from accessing the dead messenger.

**Fix:** Add `api_.set_messenger(nullptr)` before `clear_role_pyobjects()`.

---

### PR-17 — INFO — FALSE POSITIVE: heartbeat ordering

Heartbeat is enqueued into messenger's DEALER worker thread (already running). The
`start_embedded()` thread is separate. Safe ordering. No fix needed.

---

### PR-18 — LOW — Inbox packing fallback hardcoded `"aligned"`

**File:Line:** `src/processor/processor_script_host.cpp:575–577`

**Description:**
Consumer uses `config_.zmq_packing` as the fallback when `inbox_spec_.packing` is empty.
Processor hardcodes `"aligned"`, ignoring `config_.in_zmq_packing`.

**Fix:** Use `config_.in_zmq_packing` as the fallback packing for inbox.

---

### PR-19 — INFO — `parse_on_process_return` does not count wrong-type returns as script errors

**File:Line:** `src/processor/processor_script_host.cpp:38–46`

**Description:**
Wrong return type from `on_process()` logs an error but doesn't increment `script_errors_`.
Producer's `parse_on_produce_return` returns `{bool, is_error}` pair; caller increments counter.

**Fix:** Return `std::pair<bool, bool>` (commit, is_error) and increment `api_.increment_script_errors()` in `call_on_process_()` when `is_error` is true.

---

### PR-20 — INFO — Inaccurate comment about owned queue types

**File:Line:** `src/processor/processor_script_host.cpp:749`

**Description:**
Comment "Only stop owned SHM queues; ZMQ queues are owned by Consumer/Producer and stopped below"
is inaccurate. `in_queue_` can also be an owned `ZmqQueue` (direct ZMQ PULL path).

**Fix:** Update comment: "Stop owned queues (SHM or direct-ZMQ-PULL); broker-negotiated ZMQ
queues are borrowed pointers owned by Consumer/Producer."

---

## Verdict

**4 HIGH, 6 MEDIUM, 6 LOW, 3 INFO findings.**

Core architecture is sound. Most critical issue is **PR-01** (inbox completely unadvertisable —
functional regression). **PR-03** (use-after-free in failure path) and **PR-11** (wrong shutdown
flag) are also high priority.

## Suggested Fix Order

1. PR-01 — Advertise inbox in REG_REQ (out_opts.inbox_endpoint/schema/packing)
2. PR-02 — Return false on inbox start failure
3. PR-03 — Fix cleanup_on_start_failure (ctrl_thread_ join + owned queue cleanup)
4. PR-04 + PR-13 — Move processor_.reset() into GIL-release block
5. PR-11 — Replace running_threads → shutdown_requested in handlers
6. PR-05 — Remove dead condition in timeout handler
7. PR-06 — LOGGER_WARN for verify_checksum + ZMQ input
8. PR-12 — Fix timeout_ms=0 → 5000ms mapping
9. PR-18 — Inbox packing fallback use config_.in_zmq_packing
10. PR-14 — LOGGER_WARN for loop_timing != MaxRate
11. PR-16 + PR-20 — set_messenger(nullptr) + comment fix
12. PR-07/08/09 — Add last_seq, capacity, policy, verify_checksum to ProcessorAPI
13. PR-10 — Use inbox_queue_->item_size() in run_inbox_thread_
14. PR-19 — parse_on_process_return script_errors parity
15. PR-15 — Dependent on PR-03 fix
