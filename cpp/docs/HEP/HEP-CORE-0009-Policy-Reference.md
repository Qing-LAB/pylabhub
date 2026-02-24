# HEP-CORE-0009: Policy Reference

**Status**: Active — maintained document; updated when new policies are added
**Created**: 2026-02-22
**Area**: Cross-cutting — DataHub, BrokerService, Actor Framework

---

## 1. Purpose

pylabhub has several distinct "policy" enums that control behaviour at different
layers of the stack. This document is the canonical cross-reference: where each
policy lives, what layer applies it, and how it interacts with other policies.

Related documents:
- HEP-CORE-0002: DataHub — slot state machine, SHM layout
- HEP-CORE-0007: DataHub Protocol and Policy — protocol flows, DRAINING proof
- HEP-CORE-0008: LoopPolicy and Iteration Metrics — loop pacing
- `docs/IMPLEMENTATION_GUIDANCE.md` § "Error Taxonomy" — Cat 1 / Cat 2 errors

---

## 2. Policy Categories

### 2.1 Buffer Management

**`DataBlockPolicy`** — `src/include/utils/data_block.hpp`
**Applied by**: `DataBlockProducer` at SHM creation time. Stored in `SharedMemoryHeader`.

| Value | Description |
|-------|-------------|
| `Single` | One slot; producer blocks until consumer has released it. |
| `DoubleBuffer` | Two slots; producer writes to the idle slot while consumer reads the other. |
| `RingBuffer` | N slots ring buffer; writer advances independently; fastest throughput. |
| `Unset` | Sentinel — must not appear in a live SHM header. |

**Configuration**: `DataBlockConfig::policy` in `ProducerOptions::shm_config`.
**Immutable** once the SHM segment is created; cannot change without destroying and
recreating the segment.

---

### 2.2 Consumer Read Advancement

**`ConsumerSyncPolicy`** — `src/include/utils/data_block.hpp`
**Applied by**: `DataBlockProducer` at SHM creation; `DataBlockConsumer` at attach time.
Stored in `SharedMemoryHeader`.

| Value | Consumers | Read order | Writer block |
|-------|-----------|------------|--------------|
| `Latest_only` | Any number | Latest slot only; older slots silently skipped | Never — writer always advances freely |
| `Single_reader` | Exactly 1 | Strict FIFO; read_index = tail | Yes — blocks when ring full |
| `Sync_reader` | Multiple | Per-consumer position; slowest determines ring space | Yes — blocks until slowest consumer catches up |
| `Unset` | — | Sentinel | — |

**Configuration**: `DataBlockConfig::consumer_sync_policy`.
**Immutable** once SHM is created (stored in header).

**Interaction with DataBlockPolicy**:
- `Latest_only` is only meaningful with `RingBuffer`.
- `Single_reader` works with any policy but is most useful with `RingBuffer`.
- `Sync_reader` requires `RingBuffer` with capacity ≥ 2.

---

### 2.3 Checksum Enforcement (DataBlock layer)

**`ChecksumPolicy`** — `src/include/utils/data_block.hpp`
**Applied by**: `DataBlockProducer` on write; `DataBlockConsumer` on read.
**NOT stored in SHM** — set per-handle at `DataBlockConfig` time.

| Value | Producer | Consumer |
|-------|----------|----------|
| `None` | No checksum calls | No verification |
| `Manual` | Caller must call `update_checksum_slot()` / `update_checksum_flexible_zone()` explicitly | Caller must call `verify_checksum_*()` explicitly |
| `Enforced` | System calls update automatically on `release_write_slot` | System calls verify automatically on `release_consume_slot`; error returned on mismatch |

**Configuration**: `DataBlockConfig::checksum_policy`.
**Default** in actor framework: `ChecksumPolicy::Manual` (actor wrapper calls update
from C++ after `on_write` completes).

**Algorithm**: Always BLAKE2b-256 (`ChecksumType::BLAKE2b`). Algorithm selection is
stored in `SharedMemoryHeader`; currently only one algorithm is supported.

---

### 2.4 Actor Script Validation (actor framework layer)

**`ValidationPolicy`** — `src/actor/actor_config.hpp`
**Applied by**: `ProducerRoleWorker` and `ConsumerRoleWorker` in `actor_host.cpp`.
This is the **actor-level** view of checksum policies, with additional script-level
error handling:

```cpp
struct ValidationPolicy {
    enum class Checksum { None, Update, Enforce };
    enum class OnFail   { Skip, Pass };
    enum class OnPyError { Continue, Stop };

    Checksum  slot_checksum{Checksum::Update};
    Checksum  flexzone_checksum{Checksum::Update};
    OnFail    on_checksum_fail{OnFail::Skip};
    OnPyError on_python_error{OnPyError::Continue};
};
```

**`Checksum` sub-enum (per zone)**:

| Value | Producer action | Consumer action |
|-------|----------------|----------------|
| `None` | No update | No verify |
| `Update` | C++ writes BLAKE2b after `on_write()` | No verify (trust producer) |
| `Enforce` | C++ writes BLAKE2b after `on_write()` | C++ verifies before `on_read()` |

**`OnFail`** (consumer only — what to do when `Enforce` fails):

| Value | Action |
|-------|--------|
| `Skip` | Discard slot; do not call `on_read()`; log Cat 2 warning |
| `Pass` | Call `on_read()` with `api.slot_valid() == False` |

**`OnPyError`** (both sides — unhandled Python exception in any callback):

| Value | Action |
|-------|--------|
| `Continue` | Log full traceback; discard current slot; keep running |
| `Stop` | Log traceback; stop the actor cleanly |

**JSON config** (per role):
```json
"validation": {
    "slot_checksum":     "update",
    "flexzone_checksum": "update",
    "on_checksum_fail":  "skip",
    "on_python_error":   "continue"
}
```

**Relationship to DataBlock `ChecksumPolicy`**:
- Actor framework always creates `DataBlockProducer` with `ChecksumPolicy::Manual`.
- `ValidationPolicy::Checksum::Update/Enforce` controls WHEN the actor wrapper
  calls `update_checksum_slot()` / `verify_checksum_slot()`.
- The two layers are complementary: DataBlock owns the SHM mechanism; actor config
  controls the scheduling of that mechanism.

---

### 2.5 Broker Checksum Repair

**`ChecksumRepairPolicy`** — `src/include/utils/broker_service.hpp`
**Applied by**: `BrokerService` when it receives a `CHECKSUM_ERROR_REPORT` message
(Cat 2 error reported by a producer or consumer).

| Value | Broker action |
|-------|--------------|
| `None` | Log the report; ignore (default). |
| `NotifyOnly` | Log + forward the report to all channel parties via `CHANNEL_EVENT_NOTIFY`. |
| `Repair` | Reserved — requires WriteAttach slot repair path; not implemented. |

**Configuration**: `BrokerService::Config::checksum_repair_policy`.
**Default**: `None`.

**Triggering**: A producer or consumer sends `CHECKSUM_ERROR_REPORT` to the broker
when `ChecksumPolicy::Enforced` detects a mismatch. The broker then applies this
policy. This is a Cat 2 (non-fatal, recoverable) error path.

---

### 2.6 Loop Pacing

#### 2.6.1 Actor-layer: LoopTimingPolicy (IMPLEMENTED, 2026-02-23)

**`RoleConfig::LoopTimingPolicy`** — `src/actor/actor_config.hpp`
**Applied by**: `ProducerRoleWorker` / `ConsumerRoleWorker` in `actor_host.cpp`.

| Value | Deadline formula | Overrun behaviour |
|-------|-----------------|-------------------|
| `FixedPace` | `next = now() + interval_ms` | No catch-up; rate ≤ target |
| `Compensating` | `next += interval_ms` | Fires immediately; average rate converges to target |

JSON: `"loop_timing": "fixed_pace"` (default) | `"compensating"`.
Observability: `api.loop_overrun_count()`, `api.last_cycle_work_us()`.

#### 2.6.2 RAII-layer: LoopPolicy (Pass 2 — not yet implemented)

**`LoopPolicy`** — `src/include/utils/data_block.hpp` *(planned)*
**Applied by**: `SlotIterator::operator++()` (sleep); `acquire_write_slot()` (overrun detection).

| Value | Sleep behaviour | Overrun tracking |
|-------|----------------|-----------------|
| `MaxRate` | None — iterate as fast as possible | No |
| `FixedRate` | `sleep(max(0, period_ms − elapsed))` in `SlotIterator::operator++()` | Yes — `acquire_write_slot()` increments `ContextMetrics::overrun_count` |
| `MixTriggered` | Reserved | Reserved |

See **HEP-CORE-0008** for the full design, including the five-domain metrics model
(Channel throughput / Acquire timing / Loop scheduling / Script supervision / Topology)
and the `set_loop_policy()` unification mechanism (`ContextMetrics` lives in DataBlock
Pimpl; `TransactionContext::metrics()` is a pass-through reference).

**JSON config** (per role, Pass 2):
```json
"loop_policy": "fixed_rate",
"period_ms": 10
```

---

## 3. Policy Interaction Summary

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  Actor JSON config                                                          │
│    "validation": { slot_checksum, flexzone_checksum, on_checksum_fail,     │
│                    on_python_error }          ← ValidationPolicy             │
│    "loop_policy", "period_ms"                ← LoopPolicy (Pass 2)          │
│                                                                             │
│  actor_host.cpp (ProducerRoleWorker / ConsumerRoleWorker)                  │
│    Reads ValidationPolicy → controls WHEN update/verify are called          │
│    Reads LoopPolicy → passes to SlotIterator                                │
│                                                                             │
│  hub::Producer / hub::Consumer (ProducerOptions / ConsumerOptions)          │
│    shm_config.checksum_policy = Manual   ← always Manual in actor framework │
│    shm_config.policy = RingBuffer        ← from JSON "shm.slot_count"       │
│    shm_config.consumer_sync_policy = Latest_only                            │
│                                                                             │
│  DataBlockProducer (SHM write path)                                        │
│    ChecksumPolicy::Manual → actor wrapper drives update_checksum_*()        │
│    DataBlockPolicy::RingBuffer → ring buffer slot management                │
│    ConsumerSyncPolicy::Latest_only → consumer always gets newest slot       │
│                                                                             │
│  BrokerService                                                              │
│    ChecksumRepairPolicy → what to do with CHECKSUM_ERROR_REPORT msgs        │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 4. Default Policy Stack (Actor Framework)

When a user creates an actor with default JSON config:

| Layer | Policy | Value |
|-------|--------|-------|
| SHM buffer | `DataBlockPolicy` | `RingBuffer` |
| SHM consumer sync | `ConsumerSyncPolicy` | `Latest_only` |
| SHM checksum mechanism | `ChecksumPolicy` | `Manual` |
| Actor slot checksum | `ValidationPolicy::Checksum` | `Update` (producer updates; consumer does not verify) |
| Actor flexzone checksum | `ValidationPolicy::Checksum` | `Update` |
| Actor on checksum fail | `ValidationPolicy::OnFail` | `Skip` |
| Actor on Python error | `ValidationPolicy::OnPyError` | `Continue` |
| Broker checksum repair | `ChecksumRepairPolicy` | `None` |
| Loop pacing | `LoopPolicy` | `MaxRate` |

This default prioritizes **throughput and resilience**: slots flow as fast as
possible; checksum errors are logged but do not stop the consumer; Python errors
are logged and the slot is discarded.

For **safety-critical** applications, set:
- `slot_checksum: "enforce"` + `on_checksum_fail: "skip"` (Cat 2: verified reads)
- `on_python_error: "stop"` (treat script errors as fatal)
- `checksum_repair_policy: "notify_only"` (alert all parties on mismatch)

---

## 5. Where to Find Each Policy

| Policy | Header | JSON key | Applied in |
|--------|--------|----------|------------|
| `DataBlockPolicy` | `data_block.hpp` | `shm.slot_count` (implicit RingBuffer) | `DataBlockProducer` ctor |
| `ConsumerSyncPolicy` | `data_block.hpp` | N/A (fixed in actor) | `DataBlockProducer` ctor |
| `ChecksumPolicy` | `data_block.hpp` | N/A (fixed Manual in actor) | `DataBlockProducer` / `Consumer` per-slot |
| `ChecksumType` | `data_block.hpp` | N/A (fixed BLAKE2b) | SHM header |
| `ValidationPolicy::Checksum` | `actor_config.hpp` | `validation.slot_checksum` | `ProducerRoleWorker` / `ConsumerRoleWorker` |
| `ValidationPolicy::OnFail` | `actor_config.hpp` | `validation.on_checksum_fail` | `ConsumerRoleWorker` |
| `ValidationPolicy::OnPyError` | `actor_config.hpp` | `validation.on_python_error` | Both role workers |
| `ChecksumRepairPolicy` | `broker_service.hpp` | `BrokerService::Config` | `BrokerService::run()` |
| `RoleConfig::LoopTimingPolicy` | `actor_config.hpp` | `loop_timing` | `ProducerRoleWorker` / `ConsumerRoleWorker` |
| `LoopPolicy` *(RAII Pass 2)* | `data_block.hpp` | `loop_policy` + `period_ms` | Sleep: `SlotIterator::operator++()`; overrun detection: `acquire_write_slot()` |
