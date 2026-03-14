# Code Review: Full Stack — ZmqQueue, ShmQueue, Producer, Consumer, Config
**Date**: 2026-03-10
**Scope**: ZmqQueue, ShmQueue, hub::Producer, hub::Consumer, ProducerScriptHost,
ConsumerScriptHost, config parsing, broker federation
**Status**: ✅ CLOSED — all actionable items resolved 2026-03-10

Review source: background agent (agentId a6ddfe932c7dee2ff) + manual triage 2026-03-10.

---

## Status Table

| ID | Sev | Finding | Status |
|----|-----|---------|--------|
| FS-01 | HIGH | Producer hub callbacks invoked from peer_thread_ without GIL guard | ✅ FALSE POSITIVE — callbacks only enqueue to C++ message queue (enqueue_message, mutex-protected); Python called from loop_thread_ only |
| FS-02 | MEDIUM | InboxQueue config fields not validated at parse time | ✅ FIXED 2026-03-10 — zmq_packing throws in ConsumerConfig; inbox_buffer_depth>0 check added; inbox_schema_json type check (string\|object) in both configs; +8 tests → 996 |
| FS-03 | MEDIUM | ShmQueue::capacity() throws; base QueueReader contract is noexcept | ✅ FALSE POSITIVE — ConsumerAPI::in_capacity() already wraps in try/catch (consumer_api.cpp:268-269) |
| FS-04 | MEDIUM | Consumer inbox_thread_ — receiving side | ✅ FIXED 2026-03-10 |
| FS-05 | MEDIUM | ZmqQueue capacity() returns send_depth, not SNDHWM | ⚪ DEFERRED (doc gap) |
| FS-06 | MEDIUM | loop_driver field name confuses with LoopDriver enum | ⚪ DEFERRED |
| FS-07 | MEDIUM | HEP-0022 lacks cryptographic peer authentication | ⚪ DEFERRED (by design) |
| FS-08 | MEDIUM | ZmqQueue actual_endpoint() semantics unclear before start() | ⚪ DEFERRED (doc gap) |
| FS-09 | LOW | ZmqQueue is_running() true before threads actually start | ⚪ KNOWN (MR-09) |
| FS-10 | LOW | Consumer::connect_from_parts() missing Doxygen | ⚪ DEFERRED |
| FS-11 | LOW | Template vs non-template connect() guidance missing | ⚪ DEFERRED |
| FS-12 | LOW | consumer_loop_driver conditionally sent in CONSUMER_REG_REQ | ⚪ DEFERRED |
| FS-13 | ✅ LOW | ShmQueue checksum ordering (CR-03) | ✅ PRE-EXISTING |
| FS-14 | ✅ LOW | Producer callback thread-safety via callbacks_mu | ✅ PRE-EXISTING |

---

## FS-01 — Producer hub callbacks invoked without GIL guard

**Files**: `src/utils/hub/hub_producer.cpp:236–270`
**Severity**: HIGH — deadlock risk when Python scripts install callbacks

`hub::Producer` calls `on_consumer_joined_cb`, `on_consumer_left_cb`, etc. from `peer_thread_`.
If a Python script sets these callbacks via pybind11, the callback is a `std::function`
wrapping a Python callable. Invoking it from peer_thread_ **without the GIL** causes a deadlock
or crash if the Python runtime is also active on another thread.

**Evidence**: Pattern in hub_producer.cpp:236–241:
```cpp
Producer::ConsumerCallback joined_cb;
{ std::lock_guard<std::mutex> lk(callbacks_mu); joined_cb = on_consumer_joined_cb; }
if (joined_cb)
    joined_cb(identity);  // Called from peer_thread_ — no GIL
```

**Mitigation**: Verify whether ProducerScriptHost actually sets `on_consumer_joined()` on the
hub::Producer. If callbacks are set in the script host, they must wrap the Python call in
`py::gil_scoped_acquire`. If callbacks are C++-only (no Python), this is not a bug.

**Recommended fix** (in ProducerScriptHost, wherever hub::Producer callbacks are installed):
```cpp
producer_->on_consumer_joined([this](const std::string& id) {
    py::gil_scoped_acquire acquire;
    if (py_on_consumer_joined_)
        py_on_consumer_joined_(id);
});
```

**Action**: Audit `src/producer/producer_script_host.cpp` and `src/consumer/consumer_script_host.cpp`
for all `producer_->on_*` and `consumer_->on_*` calls; wrap Python invocations in GIL acquire.

---

## FS-02 — InboxQueue config fields not validated at parse time

**Files**: `src/producer/producer_config.cpp`, `src/consumer/consumer_config.cpp`
**Severity**: MEDIUM — config error surfaces at runtime, not at startup

When `has_inbox()` is true, neither ProducerConfig nor ConsumerConfig validates:
1. That `inbox_schema_json` is valid JSON parseable as a ZmqSchemaField array
2. That `inbox_packing` is "aligned" or "packed" (not some typo like "default" or "natural")

Errors only surface when `ProducerScriptHost::start_role()` calls
`ZmqQueue::pull_from()` (which does validate), crashing startup with a cryptic exception.

**Recommended fix**: In `producer_config.cpp` / `consumer_config.cpp`, after parsing inbox fields:
```cpp
if (has_inbox()) {
    if (inbox_packing != "aligned" && inbox_packing != "packed")
        throw std::runtime_error("inbox: zmq_packing must be 'aligned' or 'packed'");
    try {
        auto arr = nlohmann::json::parse(inbox_schema_json);
        if (!arr.is_array() || arr.empty())
            throw std::runtime_error("inbox_schema must be non-empty JSON array");
    } catch (const nlohmann::json::exception& e) {
        throw std::runtime_error(std::string("inbox_schema parse error: ") + e.what());
    }
}
```

---

## FS-03 — ShmQueue::capacity() throws; base class is noexcept (KNOWN MR-05)

**Files**: `src/utils/hub/hub_shm_queue.cpp:309-325`
**Status**: ⚪ Already tracked as MR-05 in `docs/todo/API_TODO.md`.

ShmQueue::capacity() throws `std::runtime_error` but QueueReader::capacity() is an abstract
virtual method with no exception specification. ConsumerAPI::in_capacity() is `noexcept`
(consumer_api.hpp:136), so if ShmQueue::capacity() throws inside `in_capacity()`, the program
calls `std::terminate`.

Fix: return 0 or cache the capacity at construction time.

---

## FS-04 — Consumer inbox_thread_ (FIXED 2026-03-10)

**Status**: ✅ FIXED — ConsumerConfig inbox fields + ConsumerScriptHost::run_inbox_thread_()
implemented 2026-03-10 (979→979 tests).

---

## FS-05 — ZmqQueue write-mode capacity() returns send_depth, not SNDHWM

**Files**: `src/utils/hub/hub_zmq_queue.cpp:929-935`
**Status**: ⚪ DEFERRED (documentation gap, not a correctness bug)

Write-mode capacity() returns `send_depth_` (internal ring), not `ZMQ_SNDHWM`.
The two values may differ. Caller gets misleading info about the actual backpressure limit.

Deferred: the send_depth_ IS the primary write bottleneck for normal operation; SNDHWM
is an OS-level secondary limit. Document the distinction in the QueueWriter::capacity() docstring.

---

## FS-06 — loop_driver field name confusion

**Files**: `src/include/utils/hub_consumer.hpp:...`, `src/consumer/consumer_config.hpp`
**Status**: ⚪ DEFERRED

`ConsumerOptions::loop_driver` is a transport string ("shm"/"zmq"), not a LoopDriver enum.
The name overlaps with HEP-0015's "LoopDriver" concept (which IS a different thing).
Renaming to `expected_data_transport` would be more precise but is a breaking API change.

---

## FS-07 — HEP-0022 lacks cryptographic peer authentication

**Files**: `src/utils/ipc/broker_service.cpp:2130-2159`
**Status**: ⚪ DEFERRED — by design

Current implementation validates peer_hub_uid against configured peers (static topology).
No cryptographic verification: a man-in-the-middle can send HELLO with a valid configured
hub_uid and be accepted.

Deferred: ZMQ CURVE authentication protects the transport layer. Broker-level HMAC would
add defense-in-depth. Accepted risk for now; revisit when deploying in adversarial networks.

---

## FS-08 — ZmqQueue actual_endpoint() before start() misleading

**Files**: `src/utils/hub/hub_zmq_queue.cpp:948-954`
**Status**: ⚪ DEFERRED (documentation gap)

`actual_endpoint()` before `start()` falls back to configured endpoint string (which may
contain ":0"). After `start()` with port-0 bind, returns OS-resolved port. If getsockopt
fails after bind, falls back to ":0" endpoint — misleading.

Add explicit flag `endpoint_resolved_` to distinguish "not-started" from "getsockopt-failed".
Low priority; current tests use `start()` then `actual_endpoint()` correctly.

---

## FS-09 — ZmqQueue is_running() race (KNOWN MR-09)

See MR-09 in `docs/todo/API_TODO.md`. `running_` set before threads launch.

---

## Closure Note (2026-03-10)

All actionable items resolved:
- FS-01, FS-03 (MR-05), FS-14: false positives confirmed by code audit
- FS-02: inbox config validation hardened in ProducerConfig + ConsumerConfig; 8 new tests added
- FS-04: consumer inbox_thread_ implemented
- MR-10: write_acquire() after stop() guard already present (send_stop_ at hub_zmq_queue.cpp:847)
- All other items: deferred (doc gaps, design decisions, known issues tracked in API_TODO)

Final test count: **996/996**
