# HEP-CORE-0008: LoopPolicy and Iteration Metrics

**Status**: Pass 2 complete (2026-02-23) — ContextMetrics Pimpl, timing in acquire/release, api.metrics() wired; RAII SlotIterator sleep + explicit JSON config deferred to Pass 3
**Created**: 2026-02-22
**Area**: DataHub RAII Layer / Actor Framework
**Depends on**: HEP-CORE-0002 (DataHub), HEP-CORE-0005 (Script Interface), HEP-CORE-0006 (SlotProcessor API)

---

## 0. Implementation Status

### What was implemented at the actor layer (2026-02-23) — separate from this HEP

The actor framework (`actor_host.cpp`) has its own deadline-based pacing loop that
operates **above** the RAII layer. It uses explicit `sleep_until` in the
`ProducerRoleWorker` while-loops and calls `acquire_write_slot()` / `release_write_slot()`
directly on the primitive API — it does **not** go through `ctx.slots()` / `SlotIterator`.

What was added:
- `RoleConfig::LoopTimingPolicy`: `FixedPace` (`next = now() + interval`) / `Compensating` (`next += interval`)
- `api.loop_overrun_count()` — overrun counter incremented in actor_host.cpp
- `api.last_cycle_work_us()` — work time measured in actor_host.cpp
- JSON: `"loop_timing": "fixed_pace" | "compensating"` per role
- **`SlotIterator::operator++()` and `TransactionContext` are completely unchanged.**

These actor-layer metrics are supervised (C++ host writes, Python reads). They address
the most common actor use case but do not expose `ctx.metrics()` to C++ RAII users.

### What this HEP defines (RAII-layer — PENDING)

`SlotIterator::operator++()` currently does only two things: `update_heartbeat()` +
`acquire_next_slot()`. No timing, no sleep, no overrun counting.

This HEP adds `ContextMetrics` to the DataBlock Pimpl and timing/pacing to
`SlotIterator::operator++()`. This gives C++ RAII users precise per-iteration
observability and unifies the measurement path shared by both the actor framework and
the RAII layer (both paths ultimately call `acquire_write_slot()`).

**Interaction with actor-layer pacing**: When the RAII-layer `LoopPolicy::FixedRate`
is implemented, the actor-layer `LoopTimingPolicy` (fixed_pace/compensating) becomes
the deadline-advancement policy on top of `LoopPolicy` pacing. The two are complementary:
`LoopPolicy` controls the sleep; `LoopTimingPolicy` controls how the next deadline
advances after an overrun.

---

## 1. Motivation

The primary DAQ model in the actor framework is a `with_transaction` session containing
an indefinite slot-iteration loop:

```python
# on_write callback drives one slot per iteration
def on_write(slot, flexzone, api) -> bool:
    slot.count += 1
    return True

# C++ side (conceptual — actor framework wires this automatically):
with producer.with_transaction() as ctx:
    for slot in ctx.slots():
        call_on_write(slot)
        if shutdown_requested:
            break
```

**Gaps identified in the current design:**

1. No way to pace the write loop to a target frequency (fixed-rate DAQ).
2. No per-iteration timing observability — Python scripts cannot measure their own
   execution time or detect overruns.
3. The existing `interval_ms` approach is a crude `sleep()` in the handler body,
   unaware of actual handler execution time and unable to detect drift.

This HEP defines:
- `LoopPolicy` — how the iterator paces between slot acquisitions
- `ContextMetrics` — per-context and per-iteration timing state, organized by domain
- Integration with the actor framework (`api.metrics()` from Python)

### 1.1 Metric Domain Model

Runtime metrics in pylabhub are organized by **measurement domain** — where they
naturally arise and what subsystem produces them. This avoids assigning ownership to
a single layer and instead focuses on where values are computed (the measurement site),
where they are stored (the collection site), and what interface surfaces them to callers.

Three organizing concepts:

| Concept | Definition |
|---------|-----------|
| **Measurement site** | Where the value is computed — follows the code, non-negotiable |
| **Collection site** | Where the value is stored for later retrieval — a design choice |
| **Access API** | The interface presented to callers (C++ RAII, Python script, broker) |

Five natural domains in the stack:

| Domain | What is measured | Measurement site | Accessible to |
|--------|-----------------|------------------|---------------|
| 1. Channel throughput | `slots_written`, `commit_index` | SHM `SharedMemoryHeader` (cross-process) | All parties via SHM read |
| 2. Acquire/release timing | `last_slot_wait_us`, `iteration_count`, `last_iteration_us`, `max_iteration_us` | `acquire_write_slot()` / `acquire_consume_slot()` in data_block.cpp | DataBlock Pimpl → RAII ctx → Python |
| 3. Loop scheduling | `overrun_count`, `last_slot_work_us`, `period_ms`, `context_elapsed_us` | Whoever runs the timing loop — `SlotIterator` (RAII path) or `actor_host.cpp` (actor path) | DataBlock Pimpl → RAII ctx → Python |
| 4. Script supervision | `script_error_count`, `slot_valid` | Actor host (Python error paths) | Python only (actor-specific) |
| 5. Channel topology | `consumer_count`, `last_heartbeat_us` | Broker / heartbeat protocol | Broker; Python via broker query |

This HEP covers **Domains 2 and 3** only. Domain 1 is already in the SHM header.
Domains 4 and 5 are out of scope.

**Collection site for Domains 2 and 3**: `DataBlockProducer::Impl` /
`DataBlockConsumer::Impl` (the Pimpl structs). Storing metrics here, rather than inside
`TransactionContext`, gives two important properties:
- Metrics survive across `with_transaction()` calls (useful for long-running services).
- The actor framework can read metrics from `producer_->metrics()` directly, without
  needing a `TransactionContext` in scope. `TransactionContext::metrics()` is a
  pass-through reference to the same Pimpl storage.

**Unification via `set_loop_policy()`**: calling `set_loop_policy(FixedRate, period_ms)` on
the DataBlockProducer/Consumer stores the timing target in the Pimpl. The primitive
`acquire_write_slot()` implementation then detects overruns using that target. Since both
the actor path (primitive calls in actor_host.cpp) and the RAII path (via SlotIterator)
go through `acquire_write_slot()`, the measurement is shared — no duplication.

---

## 2. LoopPolicy Enum

**Location**: `src/include/utils/data_block.hpp`

```cpp
enum class LoopPolicy : uint8_t {
    MaxRate,      ///< No sleep — run as fast as possible (default).
    FixedRate,    ///< Start-to-start period: sleep(max(0, period_ms − elapsed)).
    MixTriggered, ///< Reserved — not implemented this version.
};
```

**Semantics:**

| Policy | Behavior | Use case |
|--------|----------|----------|
| `MaxRate` | No sleep; `operator++()` returns immediately after slot acquire | Maximum throughput; backlog drain |
| `FixedRate` | Measures start-to-start interval; sleeps the remainder of `period_ms` | Fixed-rate DAQ (e.g. 100 Hz sensor) |
| `MixTriggered` | Reserved; behaviour undefined until designed | Event-driven mixed timing |

**Configuration API** (on `DataBlockProducer` / `DataBlockConsumer`):

```cpp
void set_loop_policy(LoopPolicy policy,
                     std::chrono::milliseconds period = {});
```

`period` is ignored when `policy == MaxRate`.
For `FixedRate`, `period = 0ms` is treated as `MaxRate` (no sleep).

**JSON config** (`actor_config`):

```json
"loop_policy": "fixed_rate",
"period_ms": 10
```

Both fields are optional; `loop_policy` defaults to `"max_rate"` and `period_ms` defaults to `0`.
The existing `interval_ms` field controls the actor-layer deadline loop (separate concern);
`loop_policy`/`period_ms` control the DataBlock Pimpl overrun detection and (in a future pass)
the RAII `SlotIterator` sleep.

---

## 3. ContextMetrics Struct

**Location**: `src/include/utils/data_block.hpp`

```cpp
using Clock = std::chrono::steady_clock;

struct ContextMetrics {
    // ── Session boundaries ──────────────────────────────────────────────────────
    Clock::time_point context_start_time{};    ///< Set on first acquire or set_loop_policy() call.
    uint64_t          context_elapsed_us{0};   ///< Updated at each acquire; final at session end.
    Clock::time_point context_end_time{};      ///< Zero while running; set when handle is destroyed.

    // ── Domain 2: Acquire/release timing ──────────────────────────────────────
    uint64_t last_slot_wait_us{0};  ///< Time blocked inside acquire_*_slot().
    uint64_t last_iteration_us{0};  ///< Start-to-start time between consecutive acquires.
    uint64_t max_iteration_us{0};   ///< Peak start-to-start time since session start.
    uint64_t iteration_count{0};    ///< Successful slot acquisitions since session start.

    // ── Domain 3: Loop scheduling ──────────────────────────────────────────────
    uint64_t overrun_count{0};      ///< Iterations where start-to-start gap > period_ms.
    uint64_t last_slot_work_us{0};  ///< Time from acquire to release (user code + overhead).

    // ── Config reference (informational, not a metric) ─────────────────────────
    uint64_t period_ms{0};          ///< Configured target period (0 = MaxRate).
};
```

### 3.1 Ownership and Lifetime

- **Owned by `DataBlockProducer::Impl` / `DataBlockConsumer::Impl`** (the Pimpl structs).
- Initialized to zero-value at construction.
- `context_start_time` is set on the first `acquire_*_slot()` call (or when `set_loop_policy()`
  is called with a non-zero period), whichever comes first.
- `context_end_time` is set when the DataBlock producer/consumer handle is destroyed.
- **Persists across `with_transaction()` calls** — all timing state accumulates for the
  lifetime of the handle. Call `clear_metrics()` at session start to reset counters.
- **Not stored in SHM** — entirely process-local.

### 3.2 Access

```cpp
// On DataBlockProducer / DataBlockConsumer handle:
const ContextMetrics& metrics() const noexcept;    // read-only live view from Pimpl
void clear_metrics() noexcept;                     // reset all counters; preserve context_start_time

static Clock::time_point now() noexcept;           // consistent timestamp

// On TransactionContext — pass-through to the producer/consumer Pimpl:
const ContextMetrics& ctx.metrics() const noexcept;    // reference into Pimpl storage
static Clock::time_point ctx.now() noexcept;
```

---

## 4. Measurement Sites

ContextMetrics fields are populated at two sites in `data_block.cpp`:

### 4.1 Domain 2 — inside `acquire_write_slot()` / `acquire_consume_slot()`

These functions are the natural measurement site: they know when blocking started and
when the slot was granted. The Pimpl holds a `t_iter_start_` anchor tracking when the
previous acquire completed.

```
acquire_write_slot() called:
  t_now = Clock::now()

  if context_start_time is zero:
    context_start_time = t_now

  if t_iter_start_ is valid (not the first call):
    elapsed = t_now − t_iter_start_
    metrics_.last_iteration_us  = elapsed_us
    metrics_.max_iteration_us   = max(max, elapsed_us)
    metrics_.context_elapsed_us = (t_now − context_start_time) as us

    if loop_policy_ == FixedRate and period_ms_ > 0:
      if elapsed_us > period_ms_us:
        ++metrics_.overrun_count     // start-to-start was late

  t_acquire_start = Clock::now()
  [existing acquire logic — blocks until slot or timeout]
  t_acquire_done  = Clock::now()

  if slot acquired:
    metrics_.last_slot_wait_us = (t_acquire_done − t_acquire_start) as us
    ++metrics_.iteration_count
    t_iter_start_ = t_acquire_done   // anchor for next iteration
```

### 4.2 Domain 2 — inside `release_write_slot()` / `release_consume_slot()`

```
release_write_slot() called:
  metrics_.last_slot_work_us = (Clock::now() − t_iter_start_) as us
  // then existing release logic
```

### 4.3 Domain 3 — sleep control in SlotIterator (RAII path only)

For the **RAII path**, the sleep lives in `SlotIterator::operator++()`.
Before calling into `acquire_next_slot()`, operator++() paces to the target period:

```
operator++() called:
  update_heartbeat()    // existing — unchanged

  if loop_policy_ == FixedRate and period_ms_ > 0 and t_last_acquire_ is valid:
    elapsed = Clock::now() − t_last_acquire_
    if elapsed < period_ms:
      sleep_for(period_ms − elapsed)   // pace before next acquire

  acquire_next_slot()   // calls acquire_write_slot() → timing updates happen there
```

`SlotIterator` reads `loop_policy_` and `period_ms_` from the DataBlock Pimpl at
construction. It does **not** update ContextMetrics directly — that happens inside
`acquire_write_slot()`.

For the **actor path**: the sleep is in `actor_host.cpp` (deadline-based loop with
`LoopTimingPolicy`). The actor calls `acquire_write_slot()` directly. Overrun detection
still fires inside `acquire_write_slot()` because `set_loop_policy()` has been called.
This is the unification point: regardless of which caller runs the timing loop, the
overrun measurement occurs at `acquire_write_slot()` — the same code path.

### 4.4 Overrun Definition

**Overrun** = gap between `t_iter_N_acquire` and `t_iter_N+1_acquire` exceeds `period_ms`.

Both slow user code **and** blocked slot acquisition contribute to the gap. This
matches real-time system semantics: the period budget is "start to start", so any
time spent anywhere in the iteration is charged against the budget.

`MaxRate` (period_ms = 0): `overrun_count` is never incremented.

---

## 5. Manual Update API on TransactionContext

For C++ users who do not use the `SlotIterator` loop (single-slot break pattern):

```cpp
// Timestamp facility
static Clock::time_point now() noexcept;     // consistent clock for manual measurements

// Manual helpers — delegate to the Pimpl via TransactionContext
void update_context_elapsed() noexcept;      // metrics_.context_elapsed_us = now() − context_start
void increment_overrun() noexcept;           // ++metrics_.overrun_count

// Read — forwards to DataBlock Pimpl
const ContextMetrics& metrics() const noexcept;
```

For the single-slot break pattern:
- `context_elapsed_us` is auto-updated at every `acquire_write_slot()` call.
- `overrun_count` = 0 unless the loop runs long enough to trigger it, or
  `increment_overrun()` is called manually for edge-case tracking.

---

## 6. Actor Framework Integration

### 6.1 Python `api.metrics()` dict

In `actor_module.cpp`, `api.metrics()` assembles a dict from two sources:
- **Domains 2 + 3**: from `producer_->metrics()` (DataBlock Pimpl)
- **Domain 4** (actor-specific): from `RoleMetrics` in `ActorRoleAPI`

```python
api.metrics() -> dict:
{
    # Session boundaries
    "context_start_us":   int,   # POSIX epoch microseconds of session start
    "context_elapsed_us": int,   # live elapsed since session start (us)

    # Domain 2 — Acquire/release timing (from DataBlock Pimpl)
    "iteration_count":    int,   # slots acquired so far
    "last_iteration_us":  int,   # start-to-start time between last two acquires (us)
    "max_iteration_us":   int,   # peak start-to-start time (us)
    "last_slot_wait_us":  int,   # time blocked waiting for a free slot (us)

    # Domain 3 — Loop scheduling (from DataBlock Pimpl / LoopPolicy config)
    "overrun_count":      int,   # count of start-to-start overruns
    "last_slot_work_us":  int,   # time from acquire to release (us)
    "period_ms":          int,   # configured target period (0 = MaxRate)

    # Domain 4 — Script supervision (from RoleMetrics, actor-specific)
    "script_error_count": int,   # unhandled Python errors in this role run
}
```

**Relationship to existing individual getters**: `api.loop_overrun_count()` and
`api.last_cycle_work_us()` on `ActorRoleAPI` remain as individual getters (they read
the actor-layer RoleMetrics counters incremented in actor_host.cpp). Once Domain 3
is implemented at the Pimpl level, `api.metrics()["overrun_count"]` will be the
canonical unified source; the individual getters become convenience aliases.

### 6.2 Config wiring in actor_host.cpp

After `create_datablock_producer(opts)` / `hub::Consumer::connect(opts)`:

```cpp
// Wire loop policy so acquire_write_slot() can detect overruns:
producer_->set_loop_policy(role_cfg_.loop_policy, role_cfg_.period_ms);

// Reset metrics at the start of each role run:
producer_->clear_metrics();
```

### 6.3 RoleConfig additions

`actor_config.hpp` adds two fields alongside the existing `interval_ms`:

```cpp
struct RoleConfig {
    // existing: actor-layer deadline loop
    int interval_ms{0};
    LoopTimingPolicy loop_timing{LoopTimingPolicy::FixedPace};

    // new: DataBlock-layer pacing (HEP-CORE-0008)
    hub::LoopPolicy          loop_policy{hub::LoopPolicy::MaxRate};
    std::chrono::milliseconds period_ms{0};
};
```

`interval_ms` drives the actor-level deadline loop in `actor_host.cpp`.
`loop_policy`/`period_ms` drive the DataBlock Pimpl overrun detection in `acquire_write_slot()`.
They are independent: a role can have `interval_ms=10` (actor sleep) and
`loop_policy=fixed_rate, period_ms=10` (DataBlock overrun tracking) simultaneously.

---

## 7. Files Affected (Pass 2)

| File | Change | Domain |
|------|--------|--------|
| `src/include/utils/data_block.hpp` | `LoopPolicy` enum; `ContextMetrics` struct; `set_loop_policy()`, `metrics()`, `clear_metrics()` declarations on DataBlockProducer/Consumer | D2+D3 |
| `src/utils/data_block.cpp` | Pimpl gains `ContextMetrics`, `t_iter_start_`, `t_last_acquire_`, `loop_policy_`, `period_ms_`; timing in `acquire_write_slot()` / `release_write_slot()` / `acquire_consume_slot()` / `release_consume_slot()` | D2+D3 |
| `src/include/utils/transaction_context.hpp` | `metrics()` pass-through (const ref to Pimpl); `now()` static; `update_context_elapsed()`, `increment_overrun()` manual helpers | D2+D3 |
| `src/include/utils/slot_iterator.hpp` | `operator++()`: sleep logic for RAII FixedRate path; reads `loop_policy_`/`period_ms_` from Pimpl; does NOT update metrics directly | D3 |
| `src/actor/actor_config.hpp` | `loop_policy` + `period_ms` in `RoleConfig`; deprecate `interval_ms` | config |
| `src/actor/actor_config.cpp` | Parse + deprecation warning for `interval_ms` | config |
| `src/actor/actor_host.cpp` | Wire `set_loop_policy()` + `clear_metrics()` after producer/consumer create | D3 |
| `src/actor/actor_module.cpp` | `api.metrics()` → dict from DataBlock Pimpl + `ActorRoleAPI.script_error_count` | D2+D3+D4 |
| `src/utils/hub_producer.hpp` | `ProducerOptions::loop_policy` + `period_ms` | config |
| `src/utils/hub_consumer.hpp` | `ConsumerOptions::loop_policy` + `period_ms` | config |

**Note**: `ContextMetrics` is declared in `data_block.hpp` (not `transaction_context.hpp`)
because its collection site is the DataBlock Pimpl. `TransactionContext` holds only a
const reference into the Pimpl. No SHM layout change — `ContextMetrics` is entirely
process-local. Core Structure Change Protocol review not required.

---

## 8. Verification (Pass 2)

```bash
cmake --build build
ctest --test-dir build --output-on-failure -j$(nproc)   # 479+ must pass

# FixedRate timing test:
# producer JSON: "loop_policy": "fixed_rate", "period_ms": 10
# Measure actual call rate → should be ~100 Hz ± tolerance
# api.metrics() → {"overrun_count": 0, "last_iteration_us": ~10000}

# MaxRate: no sleep; overrun_count stays 0
# Overrun: period_ms=1 with slow on_write → overrun_count increments

# RAII path:
# set_loop_policy(FixedRate, 10ms) on DataBlockProducer
# SlotIterator loop runs at ~100 Hz
# ctx.metrics().last_iteration_us ≈ 10000
```

---

## 9. Related Documents

- HEP-CORE-0002: DataHub FINAL — SHM layout and slot state machine
- HEP-CORE-0005: Script Interface Framework — Python callback model
- HEP-CORE-0006: SlotProcessor API — C++ RAII transaction layer
- HEP-CORE-0009: Policy Reference — all policy enums in one place
- `docs/todo/RAII_LAYER_TODO.md` — LoopPolicy listed as backlog item
