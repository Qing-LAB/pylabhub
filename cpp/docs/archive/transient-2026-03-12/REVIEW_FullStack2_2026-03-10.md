# Code Review: Full Stack Round 2 — Producer/Consumer/Processor Config, InboxQueue, Script Hosts
**Date**: 2026-03-10
**Scope**: ProducerConfig, ConsumerConfig, ProcessorConfig, InboxQueue, all three ScriptHosts,
ProcessorAPI, ConsumerAPI, ProducerAPI, config parsers
**Status**: ✅ CLOSED 2026-03-10 — A2/A14 confirmed pre-fixed; A13 fixed; broker/federation items deferred to separate sprint

Review source: background agent (full cross-reference of HEP docs vs implementation vs tests) 2026-03-10.

---

## Status Table

| ID | Sev | Area | Finding | Status |
|----|-----|------|---------|--------|
| A1 | CRITICAL | Config | `inbox_overflow_policy` documented in HEP-0018 §5.3/§5.4 but not parsed in ProducerConfig or ConsumerConfig; hardcoded to ZMQ "drop" silently | ✅ FIXED 2026-03-10 |
| A2 | CRITICAL | Processor | Processor cannot receive inbox messages: ProcessorConfig has no inbox fields; ProcessorScriptHost has no `inbox_thread_`; processors are asymmetric vs consumer | ✅ PRE-FIXED — ProcessorConfig has inbox fields; ProcessorScriptHost has full run_inbox_thread_() + make_inbox_slot_view_(); ProcessorAPI has open_inbox/wait_for_role |
| A3 | HIGH | ConsumerAPI | `ConsumerAPI::snapshot_metrics_json()` reads `reader_` from ctrl_thread_ without atomic; fragile via join ordering | ✅ PRE-FIXED (reader_ is std::atomic in committed code) |
| A4 | HIGH | ProducerAPI | `ProducerAPI::open_inbox()` holds GIL during synchronous 1000ms broker round-trip; blocks Python-side loop | ✅ PRE-FIXED (py::gil_scoped_release wraps query_role_info in committed code) |
| A5 | HIGH | ConsumerConfig | `zmq_buffer_depth` field absent; ProducerConfig has it (configurable) but ConsumerConfig hardcodes `size_t{64}` in ZmqQueue PULL creation | ✅ FIXED 2026-03-10 |
| A6 | HIGH | Tests | Missing config tests: `verify_checksum` field (ConsumerConfig), `heartbeat_interval_ms` (both configs), `zmq_buffer_depth` (ConsumerConfig), `inbox_overflow_policy` (both configs) | ✅ FIXED 2026-03-10 (+15 tests → 1011/1011) |
| A7 | MEDIUM | Broker | HEP-0019 METRICS_REQ SHM merge gap: broker returns only in-memory `metrics_store_`; design requires merged broker+SHM view (`broker_service.cpp:1817`) | ⚪ DEFERRED (separate sprint) |
| A8 | MEDIUM | Broker | HEP-0022 inbound federation peer UID not validated; unknown UIDs accepted by broker; should reject unknown hub_uid in PEER_HELLO (`broker_service.cpp:1884`) | ⚪ DEFERRED (separate sprint) |
| A9 | MEDIUM | InboxQueue | `recv_one()` calls `zmq_setsockopt(ZMQ_RCVTIMEO)` on every call — wasted work + unsafe if stop() races; move to `start()` | ✅ PRE-FIXED (last_rcvtimeo caching in committed code) |
| A10 | MEDIUM | ProducerAPI | `ProducerAPI::stop()` stores flags with `memory_order_relaxed`; should be `memory_order_release` to pair with `_acquire` loads in loop threads | ✅ PRE-FIXED (memory_order_release in committed code) |
| A11 | MEDIUM | InboxQueue | seq-gap counter is per-ROUTER (single `expected_seq_`), not per-sender; meaningless for multi-sender use | ✅ FIXED 2026-03-10 (unordered_map<string,uint64_t> per sender_id) |
| A12 | MEDIUM | ConsumerConfig | `loop_driver` field name overlaps with HEP-0015's "LoopDriver" concept but is actually a transport selector; creates confusion in cross-role reading | ✅ FIXED 2026-03-10 (renamed to `queue_type`/`QueueType`; wire key `consumer_queue_type`; HEP-0018/0009 updated) |
| A13 | LOW | Config | `script.type` silently defaults to "python"; HEP-0011 requires explicit type; hides typos | ✅ FIXED 2026-03-10 — `script_type_explicit` flag in all 3 configs; LOGGER_WARN in build_role_types() when type absent |
| A14 | LOW | ProcessorAPI | `ProcessorAPI` lacks `open_inbox()` outbound and `wait_for_role()` — present in Producer/ConsumerAPI but missing in Processor | ✅ PRE-FIXED — ProcessorAPI has open_inbox(), wait_for_role(), clear_inbox_cache(); pybind11 bound in pylabhub_processor module |
| A15 | LOW | ProcessorAPI | `ProcessorAPI` lacks `in_capacity()`, `in_policy()`, `last_seq()`, `overrun_count()` — consumer-side metrics absent | ✅ FIXED 2026-03-10 (all 6 accessors added; atomic QueueReader*/QueueWriter* backing; pybind11 bound) |
| A16 | LOW | Tests | No negative federation tests for unknown peer UID rejection | ⚪ DEFERRED (separate sprint) |
| A17 | LOW | Tests | No test that enforces HEP-0019 SHM merge behavior in METRICS_REQ | ⚪ DEFERRED (separate sprint) |
| A18 | LOW | InboxQueue | seq counter per-sender (MR-02): `unordered_map<string,uint64_t>` keyed by sender_id | ✅ FIXED 2026-03-10 (same as A11) |
| A19 | LOW | ZmqQueue | `actual_endpoint()` before `start()` returns configured endpoint (may contain `:0`); add `endpoint_resolved_` flag | ⚪ DEFERRED |
| A20 | LOW | Docs | HEP-0018 §5.4 uses `loop_trigger` / `loop_driver` for consumer field; code now uses `queue_type` | ✅ FIXED 2026-03-10 (HEP-0018 §5.2/§5.4 updated to `queue_type`) |

**Items already fixed before this review was formalized (2026-03-10):**

| ID | Finding | Status |
|----|---------|--------|
| A-D1 | Stale HEP-0021 (QueueReader/QueueWriter split not documented) | ✅ FIXED 2026-03-10 (tech_draft merge) |
| A-D2 | Stale HEP-0018 (consumer thread model + inbox_thread_ not documented) | ✅ FIXED 2026-03-10 (tech_draft merge) |
| A-D3 | tech_draft docs still "draft" instead of merged to HEPs | ✅ FIXED 2026-03-10 (archived to transient-2026-03-10/) |

---

## A1 — `inbox_overflow_policy` not implemented

**Files**: `src/producer/producer_config.hpp/cpp`, `src/consumer/consumer_config.hpp/cpp`
**Severity**: CRITICAL — documented config field silently ignored; users have no way to enable blocking

HEP-0018 §5.3 and §5.4 both list `inbox_overflow_policy` as a configurable field (`"drop"` or `"block"`,
default `"drop"`). HEP-0018 §9.5 example struct includes it. Neither ProducerConfig nor ConsumerConfig
has this field; `InboxQueue::bind_at()` always uses `rcvhwm = inbox_buffer_depth` (which implies Drop).

**Intended semantics:**
- `"drop"`: finite `rcvhwm = inbox_buffer_depth`; ZMQ drops arriving messages when queue full
- `"block"`: `rcvhwm = 0` (unlimited ZMQ queue); sender blocks when ZMQ send buffer full

**Fix**: Add `std::string inbox_overflow_policy{"drop"}` to ProducerConfig and ConsumerConfig.
Parse in `from_json_file()`. Validate: must be `"drop"` or `"block"`. Compute effective rcvhwm:
- `"drop"` → `static_cast<int>(inbox_buffer_depth)`
- `"block"` → `0`

Pass computed rcvhwm to `InboxQueue::bind_at()` call in both script hosts.

---

## A2 — Processor cannot receive inbox messages

**Files**: `src/processor/processor_config.hpp/cpp`, `src/processor/processor_script_host.hpp/cpp`,
`src/processor/processor_api.hpp/cpp`
**Severity**: CRITICAL — Processor is a first-class role alongside Producer and Consumer; the asymmetry
is a design gap, not a deliberate limitation

HEP-0018 §5.3 (Producer) and §5.4 (Consumer) both document inbox facilities. Processor has
`open_inbox()` (outbound DEALER to send messages) but no inbox receive path.

**Required additions:**
1. `ProcessorConfig`: Add `inbox_schema_json`, `inbox_endpoint`, `inbox_buffer_depth{64}`,
   `inbox_overflow_policy{"drop"}`, `zmq_packing{"aligned"}`, `has_inbox()` — mirrors ConsumerConfig
2. `ProcessorConfig::from_json_file()`: Parse above fields with same validation as ConsumerConfig
3. `ProcessorScriptHost`: Add `inbox_spec_`, `inbox_schema_slot_size_`, `inbox_type_`,
   `py_on_inbox_`, `inbox_queue_`, `inbox_thread_`, `make_inbox_slot_view_()`, `run_inbox_thread_()`
4. `ProcessorScriptHost::extract_callbacks()`: Extract `py_on_inbox_` from script module
5. `ProcessorScriptHost::start_role()`: Create InboxQueue when `has_inbox()`; start `inbox_thread_`
6. `ProcessorScriptHost::stop_role()`: Join `inbox_thread_` before `inbox_queue_->stop()`
7. `ProcessorScriptHost::cleanup_on_start_failure()`: Join + reset inbox
8. `ProcessorScriptHost::clear_role_pyobjects()`: Clear `py_on_inbox_` and `inbox_type_`
9. `ProcessorAPI` pybind11 module: Expose `on_inbox` callback field

---

## A3 — ConsumerAPI::snapshot_metrics_json() reads reader_ without atomic (HR-02)

**File**: `src/consumer/consumer_api.hpp:160`, `src/consumer/consumer_api.cpp`
**Severity**: HIGH — raw pointer nulled in `stop_role()` from main thread while ctrl_thread_ may call
`snapshot_metrics_json()` concurrently; safe via join ordering today but fragile

Change `reader_` from `const hub::QueueReader*` to `std::atomic<const hub::QueueReader*>`.
All reads via `load(std::memory_order_acquire)`; all writes via `store(std::memory_order_release)`.

---

## A4 — ProducerAPI::open_inbox() holds GIL during synchronous broker round-trip (HR-05)

**File**: `src/producer/producer_api.cpp:316`
**Severity**: HIGH — `messenger_->query_role_info()` has a 1000ms timeout; holding GIL blocks the
Python-side loop and any other thread waiting to acquire GIL

```cpp
// Before:
auto result = messenger_->query_role_info(uid, ...);

// After:
std::optional<RoleInfoResult> result;
{
    py::gil_scoped_release release;
    result = messenger_->query_role_info(uid, ...);
}
```

---

## A5 — ConsumerConfig missing `zmq_buffer_depth`

**File**: `src/consumer/consumer_config.hpp/cpp`, `src/consumer/consumer_script_host.cpp`
**Severity**: HIGH — asymmetry with ProducerConfig; consumer ZMQ PULL depth hardcoded to 64

Add `size_t zmq_buffer_depth{64}` to ConsumerConfig. Parse in `from_json_file()`. Validate > 0.
Pass to `ZmqQueue::pull_from()` call in `start_role()` (where `hub::Consumer` is created with
`ConsumerOptions::zmq_buffer_depth`).

---

## A6 — Missing config tests: verify_checksum, loop_timing, heartbeat_interval_ms

**Files**: `tests/test_layer4_consumer/test_consumer_config.cpp`,
`tests/test_layer4_producer/test_producer_config.cpp`
**Severity**: HIGH — config fields present and documented but not exercised in tests

Missing test cases:
- `ConsumerConfig_VerifyChecksum_Default_False` / `ConsumerConfig_VerifyChecksum_Parsed`
- `ConsumerConfig_LoopTiming_FixedPace_Default` / `ConsumerConfig_LoopTiming_Compensating`
- `ConsumerConfig_HeartbeatInterval_Parsed`
- `ProducerConfig_LoopTiming_FixedPace_Default` / `ProducerConfig_LoopTiming_Compensating` / `ProducerConfig_LoopTiming_Invalid_Throws`
- `ProducerConfig_HeartbeatInterval_Parsed`

---

## A7 — HEP-0019 METRICS_REQ SHM merge gap (design_review P1)

See `docs/code_review/review_design_and_code.md` item 1.
`broker_service.cpp:1817`: query path returns only `metrics_store_` (in-memory); design requires
attaching to each registered channel's SHM and merging SHM counters.

---

## A8 — HEP-0022 inbound federation peer UID not validated (design_review P1)

See `docs/code_review/review_design_and_code.md` item 2.
`broker_service.cpp:1884`: `handle_hub_peer_hello()` does not reject unknown hub_uid.

---

## A9 — InboxQueue::recv_one() ZMQ_RCVTIMEO per-call (HR-03)

**File**: `src/utils/hub/hub_inbox_queue.cpp:441`
Move `zmq_setsockopt(ZMQ_RCVTIMEO, ...)` from `recv_one()` to `start()`.
Cache timeout value in impl; `recv_one(timeout)` parameter → `start()` constructor arg or
`set_recv_timeout(ms)` before `start()`.

Actually: recv_one() takes a variable `timeout` parameter, so per-call setsockopt is needed for
variable-timeout API. If API is fixed-timeout, move to start(). Current API signature is
`recv_one(std::chrono::milliseconds timeout)` — keep per-call but verify not called after stop().

Alternative: cache last timeout in impl; only call setsockopt when timeout changes.

---

## A10 — ProducerAPI::stop() memory_order_relaxed → release (LR-04)

**File**: `src/producer/producer_api.cpp:42`
```cpp
// Before:
stopped_.store(true, std::memory_order_relaxed);

// After:
stopped_.store(true, std::memory_order_release);
```

---

## A11 — InboxQueue seq counter per-sender (MR-02)

**File**: `src/utils/hub/hub_inbox_queue.cpp:511`
Replace single `expected_seq_` with `unordered_map<string, uint64_t>` keyed by `sender_id`.

---

## A13 — script.type silently defaults to "python"

**Files**: `src/producer/producer_config.cpp:192`, `src/consumer/consumer_config.cpp:161`,
`src/processor/processor_config.cpp:250`
HEP-0011 §3.1 states type must be explicit. Current code defaults silently.
Consider: LOGGER_WARN when type absent; or require type when script block present.

---

## A14 — ProcessorAPI missing open_inbox() / wait_for_role()

**File**: `src/processor/processor_api.hpp/cpp`
ProducerAPI and ConsumerAPI both have `open_inbox()` (returns `InboxHandle`) and `wait_for_role()`.
ProcessorAPI exposes neither. Since processor is a peer role, both are needed.

---

## A20 — HEP-0018 §5.4 uses `loop_trigger` but code uses `loop_driver`

**File**: `docs/HEP/HEP-CORE-0018-Producer-Consumer-Binaries.md:274`
Field table says `loop_trigger`; JSON key in consumer_config.cpp and consumer_config.hpp is `loop_driver`.
Fix: unify to `loop_driver` in the HEP (code is authoritative here).

---
