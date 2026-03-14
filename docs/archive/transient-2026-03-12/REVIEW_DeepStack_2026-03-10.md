# Deep Stack Review — 2026-03-10

**Status:** ✅ CLOSED 2026-03-10 (all actionable items resolved)
**Reviewer:** AI (automated deep review)
**Scope:** Full codebase audit — docs/HEP/, docs/tech_draft/, src/, share/demo/, docs/README/
**Test baseline:** 1045/1045 passing at time of review (per TODO_MASTER.md)

---

## Executive Summary

1. **README_Deployment.md is severely stale** — it documents the eliminated `pylabhub-actor`
   binary throughout, with no mention of `pylabhub-producer`, `pylabhub-consumer`, or
   `pylabhub-processor`. A new user following this document cannot deploy the current system.

2. **HEP-0015 processor inbox config schema is inconsistent with code** — HEP-0015 shows
   inbox fields nested under `"inbox": { "schema": ..., "endpoint": ... }` but
   `processor_config.cpp` parses them as flat top-level keys (`inbox_schema`, `inbox_endpoint`).
   Similarly, HEP-0015 documents `in_flexzone_schema`/`out_flexzone_schema` (per-direction),
   but the code only reads `flexzone_schema` (single shared field, output-side only).

3. **HEP-0018 producer field reference uses `interval_ms` but code uses `target_period_ms`** —
   The HEP-0018 §5.1 field reference table lists `target_period_ms` as default 0, but the
   example JSON block in §5.1 shows `"interval_ms": 100`. The actual parser reads
   `target_period_ms`. The stale `interval_ms` key in the HEP example will silently be ignored
   by the parser, leading to unexpected free-run mode.

4. **`ConsumerAPI::set_verify_checksum()` is missing from the Python API** — `ProcessorAPI`
   exposes `set_verify_checksum(enable)` to Python scripts; `ConsumerAPI` does not (verify
   is config-only for consumers). This is an undocumented intentional asymmetry — HEP-0018
   §6.3 API table does not mention `verify_checksum` at all for ConsumerAPI, yet it is a
   documented feature of the consumer config.

5. **Several open code-review items in API_TODO.md are tracked but not blocked** — HR-02
   (ConsumerAPI reader_ raw pointer), HR-03 (ZMQ_RCVTIMEO per-call), HR-05 (GIL in open_inbox),
   MR-02 (per-sender seq), MR-09 (QueueReader::is_running() always true), LR-04 (ProducerAPI
   stop() memory order — already FIXED but listed as open), and several others remain open in
   API_TODO.md with no scheduled sprint.

---

## Section 1: Tech Draft → HEP Consistency

`docs/tech_draft/` contains only a `README.md` with no active draft files.

| Finding | Verdict |
|---------|---------|
| `docs/tech_draft/` is empty except for README.md | ✅ Compliant — all prior drafts were archived per DOC_STRUCTURE.md |
| Archive folders `docs/archive/transient-2026-03-09/` and `docs/archive/transient-2026-03-10/` appear in git status as untracked | INFO — not committed yet, may contain work-in-progress that is ready to commit |

**Conclusion:** The tech_draft directory is clean. No stranded draft content found outside HEPs.

---

## Section 2: HEP Document Quality

### Per-HEP Assessment

| HEP | Status | Diagrams | File Refs | Staleness Issues |
|-----|--------|----------|-----------|-----------------|
| HEP-0002 (DataHub) | ✅ Implemented | ✅ ASCII + mermaid | ✅ file refs present | Minor: test count "750/750" is stale (>1045 now), but caveat defers to TODO_MASTER |
| HEP-0007 (Protocol) | ✅ Implemented | ✅ mermaid slot state machine, sequence diagrams | ✅ file refs present | ✅ No staleness |
| HEP-0009 (Policy Reference) | ✅ Implemented | ✅ | ✅ | ✅ |
| HEP-0011 (ScriptHost) | ✅ Implemented | ✅ | ✅ | ✅ |
| HEP-0013 (Channel Identity) | ✅ Implemented | ✅ | ✅ | ✅ |
| HEP-0015 (Processor Binary) | ✅ Phase 1+2; ⚠ Phase 3 design ahead of code | ✅ ASCII 3-layer diagram | ⚠ Partial | **CRITICAL**: inbox config nested vs flat (see §3); `in_flexzone_schema`/`out_flexzone_schema` not parsed; startup coordination block documented but not yet implemented |
| HEP-0016 (Named Schema Registry) | ✅ All 5 phases | ✅ mermaid flowcharts | ✅ | ✅ |
| HEP-0017 (Pipeline Architecture) | ✅ | ✅ mermaid 5-planes diagram | ✅ | ✅ Minor: Updated field "Updated" says 2026-03-01 though last update was 2026-03-09 |
| HEP-0018 (Producer/Consumer) | ✅ | ✅ | ✅ | **HIGH**: §5.1 JSON example uses `"interval_ms"` (stale) not `"target_period_ms"`; §5.1 consumer JSON example shows `"shm.slot_count": 4` but ConsumerConfig has no `shm_slot_count` field; HEP §6.3 shows `api.overrun_count()` under "Queue metadata — consumer only" but ConsumerAPI actually names it `loop_overrun_count()` in the pybind11 module |
| HEP-0019 (Metrics Plane) | ✅ | ✅ mermaid graph | ✅ | Minor: says "828/828 passing" in header (stale) |
| HEP-0020 (Interactive Signal Handler) | ✅ | ✅ | ✅ | ✅ |
| HEP-0021 (ZMQ Virtual Channel Node) | ✅ | ✅ | ✅ | ✅ |
| HEP-0022 (Hub Federation Broadcast) | ✅ | ✅ | ✅ | ✅ |
| HEP-0023 (Startup Coordination) | ⚠ Design only — implementation pending | ✅ ASCII sequence diagrams | ✅ | This is intentional — HEP-0015 references HEP-0023 but startup coordination is not yet implemented (config fields documented in HEP-0015 §5 `startup` block are unimplemented) |

### HEP-0018 Specific Inconsistencies

1. **`interval_ms` in JSON example (§5.1 producer.json block)** — The HEP shows:
   ```json
   "interval_ms": 100,
   ```
   The actual parser (`producer_config.cpp:108`) reads `target_period_ms`. The stale key
   will be silently ignored, defaulting the period to 100ms (coincidentally the same value),
   but only because the hardcoded default in ProducerConfig matches. This is a documentation
   bug that would mislead users trying to configure free-run mode.

2. **Consumer JSON example shows `"shm.slot_count": 4`** — `ConsumerConfig` struct has no
   `shm_slot_count` field. The parser does not read this field. The HEP example is misleading.

3. **`api.overrun_count()` vs `loop_overrun_count()`** — HEP §6.3 lists `api.overrun_count()`
   under "Queue metadata — consumer only", but the pybind11 module registers it as
   `loop_overrun_count` (consumer_api.cpp:351). The method names differ between documentation
   and binding. Scripts using `api.overrun_count()` on a consumer would get `AttributeError`.

4. **`api.set_critical_error(msg)` signature in HEP §6.3** — HEP shows
   `api.set_critical_error(msg)` but the C++ signature is `set_critical_error()` with no
   argument. The HEP's Python example is wrong.

### HEP-0015 Specific Inconsistencies

1. **Inbox config structure (nested vs flat)** — HEP shows a nested block:
   ```json
   "inbox": { "schema": {...}, "endpoint": "...", "buffer_depth": 64, ... }
   ```
   The processor_config.cpp parser reads flat top-level keys:
   `inbox_schema`, `inbox_endpoint`, `inbox_buffer_depth`, `inbox_overflow_policy`.
   The `share/demo-dual-hub/processor*.json` examples would need to match one or the other.

2. **`in_flexzone_schema` / `out_flexzone_schema` fields** — HEP §5 field reference table lists
   both `in_flexzone_schema` and `out_flexzone_schema` as separate per-direction fields.
   The code only reads a single `flexzone_schema` key and stores it in
   `ProcessorConfig::flexzone_schema_json` (used for output-side only). Input side flexzone
   is not supported via config at all.

3. **`startup` block** — HEP §5 documents the `startup.wait_for_roles`,
   `startup.wait_timeout_ms`, `startup.hub_b_after_input_ready` config fields. HEP-0023 is
   marked "implementation pending". The `ProcessorConfig` struct has no corresponding fields.
   Any user following the HEP-0015 config example with a `"startup"` block would be silently
   ignored by the parser.

---

## Section 3: Implementation Completeness

### HEP-0002 (DataBlock/SHM)

| Feature | Status |
|---------|--------|
| SharedMemoryHeader ring buffer | ✅ Confirmed |
| Two-tier synchronization (DataBlockMutex + SharedSpinLock) | ✅ Confirmed |
| BLAKE2b checksum (slot + flexzone) | ✅ Confirmed |
| ConsumerSyncPolicy (Latest_only, DoubleBuffer, RingBuffer) | ✅ Confirmed |
| PID-based recovery | ✅ Confirmed |
| DataBlock expand/remap API | ⚠ Confirmed as placeholder stubs (`data_block.cpp:1382/2476` with comment "placeholder for future broker-coordinated remapping") |

### HEP-0007 (Control Plane Protocol)

| Feature | Status |
|---------|--------|
| REG_REQ / REG_ACK / DISC_REQ / DISC_ACK | ✅ |
| CONSUMER_REG_REQ / CONSUMER_REG_ACK | ✅ |
| HEARTBEAT_REQ | ✅ |
| CHANNEL_CLOSING_NOTIFY + FORCE_SHUTDOWN | ✅ |
| METRICS_REPORT_REQ / METRICS_REQ / METRICS_ACK | ✅ |
| CHANNEL_NOTIFY_REQ / CHANNEL_BROADCAST_REQ relay | ✅ |
| SHM_BLOCK_QUERY_REQ / SHM_BLOCK_QUERY_ACK | ✅ |
| ROLE_PRESENCE_REQ / ROLE_INFO_REQ | ✅ |
| TRANSPORT_MISMATCH error | ✅ |
| SCHEMA_MISMATCH error | ✅ |
| ZMQ data-plane runtime checksum+type-tag | 🔵 Deferred (HEP-0023) |
| Deferred DISC_ACK (HEP-0023) | ❌ Not yet implemented |
| ROLE_REGISTERED_NOTIFY / ROLE_DEREGISTERED_NOTIFY | ❌ Not yet implemented |

**Placeholder sockets in messenger.cpp:** Lines 625 and 740 create
`zmq::socket_type::push` and `zmq::socket_type::pull` sockets labeled `// placeholder`.
These appear to be leftover scaffolding from early Messenger development. They create
sockets that are never used in the happy path. This may be harmless (no bind, no connect,
cleaned up at scope exit) but adds noise to the code and should be verified or removed.

### HEP-0015 (Processor Binary)

| Feature | Status |
|---------|--------|
| Phase 1: config, script host, loop | ✅ |
| Phase 2: dual-broker, hub::Processor delegation | ✅ |
| Phase 3: timing policy, inbox thread | ✅ (code complete) |
| Phase 3: startup coordination (wait_for_roles) | ❌ Not implemented — pending HEP-0023 |
| Phase 3: in_flexzone_schema / out_flexzone_schema | ❌ Not implemented — only single `flexzone_schema` |
| Phase 3: inbox nested config block | ⚠ Implemented but with flat keys (HEP mismatch) |

### HEP-0016 (Named Schema Registry)

All 5 phases confirmed implemented. ✅ No gaps found.

### HEP-0017 (Pipeline Architecture)

Design complete. All five planes confirmed. ✅

### HEP-0018 (Producer/Consumer Binaries)

| Feature | Status |
|---------|--------|
| Producer SHM path | ✅ |
| Producer ZMQ path | ✅ |
| Consumer SHM path | ✅ |
| Consumer ZMQ path | ✅ |
| Inbox (producer + consumer) | ✅ |
| transport arbitration | ✅ |
| QueueReader/QueueWriter split | ✅ |
| `api.set_verify_checksum()` on consumer (Python-accessible) | ❌ Missing — only config-driven |

### HEP-0019 (Metrics Plane)

Full pipeline confirmed: `report_metric()` → `snapshot_metrics_json()` → HEARTBEAT
metrics extension → broker `METRICS_REQ/ACK`. ✅

One observation: the consumer `snapshot_metrics_json()` does not include `overrun_count`
in its base metrics JSON (there is no `loop_overrun_count` in the output). The producer
includes `overrun_count`. If the broker aggregator expects all three roles to report the
same keys, consumers will always have a missing key.

### HEP-0020 (Interactive Signal Handler)

✅ Confirmed in `src/utils/core/interactive_signal_handler.cpp`.

### HEP-0021 (ZMQ Virtual Channel Node)

✅ `data_transport` + `zmq_node_endpoint` in REG_REQ/DISC_ACK, `queue()` accessors.
Deferred: ZMQ data-plane runtime checksum+type-tag (per HEP-0023).

### HEP-0022 (Hub Federation)

✅ Confirmed: HUB_PEER_HELLO/ACK/BYE, HUB_RELAY_MSG, dedup window,
`on_hub_connected/disconnected/message`, `api.notify_hub()`.

### HEP-0023 (Startup Coordination)

Design written. Implementation pending (noted explicitly in HEP status field). Not
yet implemented in broker or script hosts. ✅ Properly labeled as design-only.

---

## Section 4: API Symmetry and Exposure

### Symmetry Audit — Common Methods

| Method | ProducerAPI | ConsumerAPI | ProcessorAPI | Notes |
|--------|-------------|-------------|--------------|-------|
| `uid()`, `name()`, `log()`, `stop()` | ✅ | ✅ | ✅ | Symmetric |
| `set_critical_error()` / `critical_error()` | ✅ | ✅ | ✅ | Symmetric |
| `script_dir()`, `log_level()` | ✅ | ✅ | ✅ | Symmetric |
| `report_metric()`, `report_metrics()`, `clear_custom_metrics()` | ✅ | ✅ | ✅ | Symmetric |
| `metrics()` | ✅ | ✅ | ✅ | Symmetric |
| `open_inbox()`, `wait_for_role()` | ✅ | ✅ | ✅ | Symmetric |
| `notify_channel()`, `broadcast_channel()`, `list_channels()` | ✅ | ✅ | ✅ | Symmetric |
| `shm_blocks()` | ✅ | ✅ | ✅ | Symmetric |
| `spinlock()`, `spinlock_count()` | ✅ | ✅ | ✅ | Symmetric |
| `out_capacity()`, `out_policy()` | ✅ (out) | — | ✅ (both) | Consumer has no output |
| `in_capacity()`, `in_policy()` | — | ✅ (in) | ✅ (both) | Producer has no input |
| `last_seq()` | — | ✅ | ✅ | Producer has no input read — intentional |
| `overrun_count()` | ✅ | — | — | **ASYMMETRY**: exists only on Producer |
| `loop_overrun_count()` | — | ✅ (hardcoded 0) | ✅ (hardcoded 0) | Consumer+Processor always 0 |
| `set_verify_checksum(bool)` | — | ❌ NOT EXPOSED | ✅ Python-exposed | **ASYMMETRY** |
| `broadcast()`, `send()`, `consumers()`, `update_flexzone_checksum()` | ✅ | — | ✅ | Consumer has no output ZMQ |
| `flexzone()` | ✅ | — | ✅ | Consumer reads fz via in_slot arg |
| `channel()` | ✅ | ✅ | — | Processor has `in_channel()`/`out_channel()` |

### Key Asymmetry Issues

**DS-01 (MEDIUM): `overrun_count()` naming inconsistency**
- `ProducerAPI` exposes `overrun_count()` bound as `"overrun_count"` in the pybind11 module.
- `ConsumerAPI` and `ProcessorAPI` expose a stub that always returns 0, bound as
  `"loop_overrun_count"` in both their modules.
- A script that calls `api.overrun_count()` on a producer will get the real value;
  the same call on a consumer will raise `AttributeError` (method not found).
- The HEP-0018 §6.3 table documents `api.overrun_count()` under "Queue metadata — consumer
  only" — this is incorrect (it does not exist on `ConsumerAPI` as that name).
- **Fix**: Either expose `overrun_count` as an alias on ConsumerAPI, or rename the producer
  binding from `overrun_count` to `loop_overrun_count` for consistency.

**DS-02 (MEDIUM): `set_verify_checksum()` not Python-accessible on ConsumerAPI**
- `ProcessorAPI::set_verify_checksum(bool)` is exposed to Python (`processor_api.cpp:533`).
- `ConsumerAPI` has no equivalent Python method. The consumer's checksum verification is
  config-only (`consumer.json "verify_checksum": true`).
- A consumer script cannot toggle checksum verification at runtime (e.g., after calibration).
- This may be intentional (consumers don't "choose" to verify; it's a deployment decision),
  but if so it should be documented in HEP-0018 as an explicit design decision.

**DS-03 (LOW): `api.set_critical_error(msg)` documented with a `msg` argument**
- HEP-0018 §6.3 shows `api.set_critical_error(msg)` with a message argument.
- The actual C++ signature is `set_critical_error()` — no argument.
- All three pybind11 bindings confirm: no arguments.

**DS-04 (LOW): Consumer does not expose `flexzone()` method**
- Producer and Processor both expose `api.flexzone()` to Python for accessing the persistent
  flexzone ctypes object.
- Consumer receives the flexzone as the `flexzone` callback argument (always `None` for ZMQ),
  so the pattern is different — but there is no `api.flexzone()` for inter-callback state.
- This is architecturally consistent (consumer flexzone is passed per-call) but may
  surprise users expecting the same pattern as producer.

### pybind11 Module Completeness

All three pybind11 modules were audited:
- `pylabhub_producer`: all ProducerAPI methods exposed. `InboxHandle` exposed. `ProducerSpinLock` exposed. ✅
- `pylabhub_consumer`: all ConsumerAPI methods exposed. `InboxHandle` exposed. `ConsumerSpinLock` exposed. ✅
- `pylabhub_processor`: all ProcessorAPI methods exposed. `InboxHandle` exposed. `ProcessorSpinLock` exposed. ✅

All three `InboxHandle` bindings are identical (same methods, same signature). ✅

---

## Section 5: Inconsistency and Duplication

### Wire Format Duplication (MR-01 — tracked)

`hub_inbox_queue.cpp` and `hub_zmq_queue.cpp` duplicate wire-format helpers (field layout,
pack/unpack, frame-size computation). Tracked in `API_TODO.md` as MR-01 (deferred).
This is a maintenance risk if the wire format ever changes.

### Script Type Default (tracked in API_TODO.md as P2)

All three `*_config.cpp` files silently default `script_type` to `"python"` when the
`"script.type"` field is absent. HEP-0011 says the type field should be explicit.
Tracked in API_TODO.md as P2.

### Remaining `// TODO` comments in source

No `// TODO` comments found in `src/**/*.cpp` or `src/**/*.hpp`. Clean. ✅

### Placeholder sockets in messenger.cpp

`messenger.cpp:625` and `messenger.cpp:740` declare `data_sock` objects with
`// placeholder` comments. These appear to be dead scaffolding from early development —
the data sockets are initialized but only used when `pattern != ChannelPattern::Bidir`.
In the `Bidir` case (`has_data_sock = false`) the socket is created but never bound or
connected, then goes out of scope. This is harmless but adds dead code noise.

### QueueReader::is_running() base default (MR-09 — tracked)

`hub_queue.hpp:202`: `virtual bool is_running() const noexcept { return true; }` defaults
to always-true. `ShmQueue` never overrides this, so it always reports "running" even before
a DataBlock connection is established. This can mask shutdown races in tests. Tracked in
API_TODO.md as MR-09 (open).

### HR-02: ConsumerAPI reader_ raw pointer (tracked)

`consumer_api.hpp:160`: `std::atomic<const hub::QueueReader*> reader_{nullptr}` — this is
actually already atomic (the code already uses `std::atomic`). The API_TODO.md entry for
HR-02 says to "change to std::atomic" but the field is already atomic. This item in
API_TODO.md may be a false positive / already addressed. Verify and close if so.

### LR-04: ProducerAPI::stop() memory ordering (tracked as open but already fixed)

`API_TODO.md` lists LR-04 as open ("stores shutdown flags with `memory_order_relaxed`")
but the current code at `producer_api.cpp:46` uses `std::memory_order_release`. The fix
was applied (per REVIEW_DataHubInbox_2026-03-09.md as LR-04 ✅). The API_TODO.md entry
is stale. **Action: mark LR-04 as resolved in API_TODO.md.**

---

## Section 6: Cross-Platform Compatibility

### SHM Implementation

The POSIX `shm_open` / `mmap` path and the Windows `CreateFileMapping` / `MapViewOfFile`
path are both present and properly guarded via `#if defined(PYLABHUB_PLATFORM_WIN64)`.
`src/utils/core/platform.cpp` implements both paths. ✅

Windows SHM name semantics differ: POSIX uses `/name` in `/dev/shm`; Windows uses
named objects. The current code passes the name directly. The abstraction appears sound.

### Known Windows Gaps (tracked in PLATFORM_TODO.md)

| Item | Status |
|------|--------|
| `/Zc:preprocessor` PUBLIC propagation audit | Open — documented in PLATFORM_TODO.md |
| MSVC warnings-as-errors gate `/W4 /WX` | Open — documented in PLATFORM_TODO.md |
| Clang-tidy full pass | Open — documented in PLATFORM_TODO.md |

No new Windows gaps found. The existing list in `docs/todo/PLATFORM_TODO.md` appears
complete and accurate.

### Linux-only assumptions

`shm_open` / `shm_unlink` / `mmap` — properly wrapped with platform guards. ✅
`sched_yield` — not used in src/ (searched). ✅
`mkstemp` — used in `json_config.cpp:988` for atomic file writes. Windows path uses
`GetTempFileName`. ✅

No new Linux-only assumptions found beyond what is already tracked.

---

## Section 7: Configuration System

### Field Validation Coverage

| Role | Config | Validation | Issues |
|------|--------|-----------|--------|
| Producer | `producer_config.cpp` | ✅ target_period_ms≥0, transport enum, zmq_buffer_depth>0, packing valid, inbox_buffer_depth>0, inbox_schema type check | ✅ |
| Consumer | `consumer_config.cpp` | ✅ zmq_packing throws on invalid, inbox_buffer_depth>0, inbox_schema type check, zmq_buffer_depth>0 | ✅ |
| Processor | `processor_config.cpp` | ✅ Similar; inbox_buffer_depth>0, packing validated | ⚠ see notes |

### Processor Config Specific Issues

**CF-01 (HIGH): `inbox_schema` flat vs nested JSON format mismatch**
- `processor_config.cpp:285` reads `j["inbox_schema"]` as a top-level key.
- HEP-0015 §5 and the example config show:
  ```json
  "inbox": { "schema": {...}, "endpoint": "...", "buffer_depth": 64, ... }
  ```
- A user following the HEP example will have their inbox silently not configured
  (flat keys vs nested object). This is a runtime failure mode.
- Note: `share/demo-dual-hub/processor*.json` files are untracked and their format is
  unknown — they may already use the flat format or the nested format.

**CF-02 (HIGH): `in_flexzone_schema` / `out_flexzone_schema` not parsed**
- HEP-0015 documents per-direction flexzone schemas.
- `processor_config.cpp:281` only parses `flexzone_schema` (no prefix).
- `ProcessorConfig` only has `flexzone_schema_json` (single field).
- Result: processor scripts cannot independently configure input/output flexzone layouts.
  The single `flexzone_schema` is used for the output side only.

**CF-03 (MEDIUM): `startup` block silently ignored**
- HEP-0015 §5 documents `startup.wait_for_roles`, `startup.wait_timeout_ms`,
  `startup.hub_b_after_input_ready`.
- `ProcessorConfig` struct has no corresponding fields.
- `processor_config.cpp` does not parse any `startup` key.
- A user writing a `"startup"` block in processor.json will have it silently ignored.
- This should be either (a) explicitly rejected with an error ("not yet implemented")
  or (b) the HEP should be updated to mark these fields as pending.

### Demo Config Accuracy

| File | Issues |
|------|--------|
| `share/demo/producer/producer.json` | ✅ Uses `slot_schema: "lab.demo.counter@1"` (named schema — correct); uses `hub_dir: "../hub"` — correct |
| `share/demo/consumer/consumer.json` | ✅ Flat format; no `queue_type` key (defaults to Shm — acceptable) |
| `share/demo/hub/hub.json` | ✅ Minimal and correct |
| `share/demo-dual-hub/*.json` | ⚠ Untracked (in git status `??`); not audited |

### HEP-0018 §5.1 JSON example issues

The producer JSON example in HEP-0018 §5.1 shows:
```json
"interval_ms": 100,
```
This key is not read by `producer_config.cpp`. The correct key is `target_period_ms`.
Additionally, the default for `target_period_ms` in ProducerConfig is `100`, so the
example accidentally works — but for the wrong reason (the stale key is silently ignored
and the hardcoded default kicks in).

The consumer JSON example in HEP-0018 §5.2 shows:
```json
"shm": { "enabled": true, "slot_count": 4, "secret": 0 }
```
`ConsumerConfig` has no `shm_slot_count` field. The consumer does not allocate a
DataBlock ring buffer — it attaches to the producer's. The `slot_count` field in
consumer.json is ignored by the parser. The HEP example is misleading.

---

## Section 8: Metrics System

### Full Pipeline Trace

1. Script calls `api.report_metric("mykey", 42.0)` → stored in `custom_metrics_` under
   `metrics_spin_` (InProcessSpinState).
2. ZMQ ctrl thread calls `api_.snapshot_metrics_json()` → builds JSON `{base:{...}, custom:{...}}`.
3. Messenger sends `HEARTBEAT_REQ` with `metrics` field (producer/processor paths).
   Consumer sends `METRICS_REPORT_REQ` (separate message type).
4. Broker receives heartbeat → stores snapshot in `metrics_store_[channel][uid]`.
5. AdminShell or external tool sends `METRICS_REQ` → broker returns `METRICS_ACK` with
   aggregated JSON from `metrics_store_`.

**Confirmed end-to-end: ✅**

### D4 Counter Consistency

| Counter | Producer | Consumer | Processor |
|---------|---------|---------|---------|
| `script_errors` | ✅ | ✅ | ✅ |
| `last_cycle_work_us` | ✅ | ✅ | ✅ |
| `out_written` / `in_received` | ✅ (out_written) | ✅ (in_received) | ✅ (both) |
| `drops` | ✅ | — | ✅ |
| `overrun_count` | ✅ (in base JSON) | ❌ (missing from snapshot_metrics_json) | — |

**MET-01 (LOW): Consumer `snapshot_metrics_json()` does not include `loop_overrun_count`**
The consumer base metrics JSON omits an overrun counter entirely. If the broker aggregator
or monitoring tool expects a consistent schema across all three role types, consumer entries
will be missing this field. For consistency, the consumer's base JSON should include
`"loop_overrun_count": 0` explicitly.

### SHM Merge Gap (DC-01 — tracked as fixed)

Per REVIEW_DesignAndCode_2026-03-09.md DC-01, the METRICS_REQ SHM merge gap was fixed
in broker_service.cpp. `TODO_MASTER.md` confirms ✅ DC-01 FIXED. No re-verification
needed.

---

## Section 9: Deployment Readiness

### README_Deployment.md — Critical Staleness

**DEPL-01 (CRITICAL): Entire deployment guide documents the eliminated `pylabhub-actor`**

`docs/README/README_Deployment.md` was written before the actor elimination (2026-03-01).
The document:
- Shows `pylabhub-actor` as the main user binary throughout
- Documents `actor.json` config format (no longer relevant)
- Quick Start (§2) instructs users to run `pylabhub-actor --init`
- Install tree (§3) lists `pylabhub-actor` as the only non-hub binary
- §5 "Actor Setup" spans multiple subsections describing an eliminated binary
- Makes no mention of `pylabhub-producer`, `pylabhub-consumer`, or `pylabhub-processor`
- Makes no mention of `producer.json`, `consumer.json`, or `processor.json`

**A new user following README_Deployment.md today cannot deploy the current system.**

The TODO_MASTER.md notes "Application-oriented README update ... root `README.md` +
`share/demo/README.md`" was completed (2026-03-05), but `docs/README/README_Deployment.md`
was not updated. This is the ops-facing deployment guide and it is completely stale.

### Demo Run Script

`share/demo/run_demo.sh` is functional and up to date. ✅ It:
- Starts hub in `--dev` mode
- Starts producer, processor, consumer in sequence with liveness checks
- Handles cleanup on SIGTERM/SIGINT
- Uses named schema files (generated inline if absent)
- Binary discovery handles multiple build variants

The consumer PID is not tracked for cleanup (the script runs consumer in foreground, which
is intentional — consumer exit triggers the cleanup trap).

### Four Binaries — Staged Correctly

All four binaries (`pylabhub-hubshell`, `pylabhub-producer`, `pylabhub-consumer`,
`pylabhub-processor`) are referenced in `run_demo.sh` and in `TODO_MASTER.md`. The staged
tree structure appears correct per CLAUDE.md build commands. ✅

### Missing for Production Deployment

| Gap | Severity | Notes |
|-----|----------|-------|
| `README_Deployment.md` completely stale (actor model) | CRITICAL | Needs full rewrite |
| HEP-0023 Startup Coordination not implemented | HIGH | Manual `sleep` workarounds in demo |
| DEFERRED DISC_ACK not implemented in broker | MEDIUM | Demo uses `sleep 0.5-0.6s` workarounds |
| Windows CI validation | MEDIUM | Tracked in PLATFORM_TODO.md |
| Dual-hub demo configs untracked | LOW | `share/demo-dual-hub/` not committed |
| Clang-tidy full pass | LOW | Tracked in PLATFORM_TODO.md |

---

## Prioritized Action Items

| ID | Severity | Area | Description |
|----|----------|------|-------------|
| DS-DEPL-01 | **CRITICAL** | Deployment Docs | `docs/README/README_Deployment.md` is fully stale (actor model); needs complete rewrite | ✅ FIXED 2026-03-10 — full rewrite |
| DS-CF-01 | **HIGH** | Processor Config | HEP-0015 inbox config uses nested `"inbox":{...}` block; code reads flat top-level keys | ✅ FIXED 2026-03-10 — HEP updated to flat keys |
| DS-CF-02 | **HIGH** | Processor Config | HEP-0015 documents `in_flexzone_schema`/`out_flexzone_schema`; code only has `flexzone_schema` | ✅ FIXED 2026-03-10 — HEP updated; per-direction noted as not implemented |
| DS-H18-01 | **HIGH** | HEP-0018 Docs | §5.1 producer JSON example uses stale `"interval_ms"` key | ✅ FIXED 2026-03-10 — updated to `target_period_ms` |
| DS-CF-03 | **MEDIUM** | Processor Config | `startup` block documented in HEP-0015 §5 but not parsed | ✅ FIXED 2026-03-10 — HEP now clearly marks as pending HEP-0023 |
| DS-DS-01 | **MEDIUM** | API Symmetry | `overrun_count()` vs `loop_overrun_count()` naming inconsistency | ✅ FIXED 2026-03-10 — ProducerAPI renamed to `loop_overrun_count` |
| DS-H18-02 | **MEDIUM** | HEP-0018 Docs | consumer JSON shows `shm.slot_count`; `set_critical_error(msg)` wrong signature | ✅ FIXED 2026-03-10 |
| DS-DS-02 | **MEDIUM** | API Symmetry | `ConsumerAPI::set_verify_checksum()` not Python-accessible | ✅ FIXED 2026-03-10 — added to ConsumerAPI C++ + pybind11 |
| DS-API-01 | **MEDIUM** | Open Items | API_TODO.md LR-04 stale (already fixed) | ✅ FIXED 2026-03-10 — marked closed in API_TODO.md |
| DS-API-02 | **MEDIUM** | Open Items | API_TODO.md HR-02 stale (reader_ already atomic) | ✅ FIXED 2026-03-10 — marked closed in API_TODO.md |
| DS-MET-01 | **LOW** | Metrics | Consumer `snapshot_metrics_json()` missing `loop_overrun_count` | ✅ FIXED 2026-03-10 — added to consumer snapshot |
| DS-DEAD-01 | **LOW** | Code Clarity | `messenger.cpp`: placeholder comment misleading (sockets ARE moved into ChannelHandle) | ✅ FIXED 2026-03-10 — comment clarified |
| DS-H15-01 | **LOW** | HEP-0015 Docs | Status field stale ("Phase 3 in progress") | ✅ FIXED 2026-03-10 — updated to Phase 1+2+3 done |
| DS-MR09 | **LOW** | hub_queue.hpp | `QueueReader::is_running()` always true — tracked in API_TODO.md MR-09 | 🔵 DEFERRED (tracked) |
| DS-HR03 | **LOW** | hub_inbox_queue | `ZMQ_RCVTIMEO` set on every recv call — tracked in API_TODO.md HR-03 | 🔵 DEFERRED (tracked) |
| DS-HR05 | **LOW** | producer_api.cpp | GIL release in `ProducerAPI::open_inbox()` — tracked in API_TODO.md HR-05 | 🔵 DEFERRED (tracked) |

---

## Notes on False Positives from Previous Reviews

- **MR-05** (ConsumerAPI::in_capacity() try/catch): confirmed false positive — already wrapped.
- **MR-10** (send_stop_ guard): confirmed false positive — guard present at hub_zmq_queue.cpp:847.
- **FS-01** (GIL in on_consumer_* callbacks): confirmed false positive — Python called only via
  `drain_messages()` from `loop_thread_`.
- **LR-04** (ProducerAPI stop() memory order): already FIXED in code, stale in API_TODO.md.
- **HR-02** (ConsumerAPI reader_ atomic): already uses `std::atomic` — stale in API_TODO.md.

---

*This review was produced by automated analysis on 2026-03-10.*
*File references are to the working directory `/home/qqing/Work/pylabhub/cpp`.*
