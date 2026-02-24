# DataHub C++ RAII Layer – Design Draft

**Status:** Draft (pre-implementation)
**Date:** 2026-02-13
**Purpose:** Design specification for the unified type-safe C++ abstraction. To be merged into `docs/HEP/HEP-CORE-0002-DataHub-FINAL.md`, `docs/IMPLEMENTATION_GUIDANCE.md`, and `docs/examples/RAII_LAYER_USAGE_EXAMPLE.md` after implementation and testing.

**Doc policy:** See `docs/DOC_STRUCTURE.md`. This draft will be merged into standard documents and then moved to `docs/archive/` when integration is complete.

---

## Table of Contents

1. [Overview](#1-overview)
2. [Context Semantics](#2-context-semantics)
3. [API Design](#3-api-design)
4. [Typed Access](#4-typed-access)
5. [Raw Access](#5-raw-access)
6. [Iterator and ctx.slots()](#6-iterator-and-ctxslotstimeout)
7. [Result Type](#7-result-type)
8. [Commit Semantics](#8-commit-semantics)
9. [Heartbeat](#9-heartbeat)
10. [Configuration and Layout](#10-configuration-and-layout)
11. [Scenarios](#11-scenarios)
12. [Error Handling](#12-error-handling)
13. [Deprecations and Removals](#13-deprecations-and-removals)
14. [Implementation Notes](#14-implementation-notes)

---

## 1. Overview

### 1.1 Design Goals

- **Single type-safe abstraction** – One unified API with compile-time and runtime schema enforcement.
- **Context-centric** – Context (`ctx`) represents validation, protection, and datahub state; not per-slot.
- **Iteration within context** – Slot iteration happens inside the transaction; context is created once.
- **Explicit raw access** – Typed access by default; raw access opt-in, after checks, at user's risk.

### 1.2 Layer Changes

| Layer | Action |
|-------|--------|
| **Layer 1.75** (SlotRWAccess on raw SlotRWState*) | **Remove** – Redundant; C API suffices for raw-pointer use. |
| **Layer 2** (Transaction API) | **Replaced** by unified member `with_transaction<FlexZoneT, DataBlockT>`. |
| **Guards** | Keep for explicit control; rename `commit()` → `mark_success()`. |

### 1.3 Key Concepts

- **Context (ctx)** – Session-level validation, protection, layout, schema; established once at transaction entry.
- **Slot** – Current datablock unit (ring-buffer slot); acquired per iteration.
- **Flexible zone** – Shared metadata region; accessible via `ctx.flexzone(idx)`.

---

## 2. Context Semantics

### 2.1 What Context Represents

Context is **not** the current slot. Context represents:

- Validation state (schema, layout, checksum policy)
- Protection (access control, protocol checks)
- Shared datahub state (producer/consumer, layout, flexible zone)

### 2.2 Context Lifecycle

1. **Entry** – `with_transaction` establishes context: schema validation, checksum policy, layout lookup.
2. **Scope** – Context is valid for the entire lambda body.
3. **Iteration** – Within context, user iterates over slots via `ctx.slots(timeout)`.
4. **Exit** – Context torn down when lambda returns or throws.

### 2.3 No Redundant Work

Validation and setup occur once at entry. Slot acquisition/release is per iteration; no repeated validation.

---

## 3. API Design

### 3.1 Member Function

```cpp
producer.with_transaction<FlexZoneT, DataBlockT>(timeout, [](auto& ctx) { ... });
consumer.with_transaction<FlexZoneT, DataBlockT>(timeout, [](auto& ctx) { ... });
```

- **Member function** – Direction (read/write) implicit from owner.
- **Template parameters** – `FlexZoneT`, `DataBlockT` for typed access; schema enforced at compile and runtime.
- **Lambda** – Receives `ctx`; first parameter enforced via `std::invocable<Func, TransactionContext&>`.

### 3.2 Lambda Parameter Enforcement

- Use `std::invocable<Func, WriteTransactionContext&>` (or appropriate context type).
- Lambda may use `[](auto& ctx)`; `auto` deduces to context type.
- Captures (`[&]`, `[=]`, etc.) remain independent.

### 3.3 Direction Implicit

- **Producer** – `ctx.slots()` yields write slots.
- **Consumer** – `ctx.slots()` yields read slots.
- Same name; policy (Latest_only, Single_reader, Sync_reader) governs behavior.

---

## 4. Typed Access

### 4.1 Flexible Zone

```cpp
ctx.flexzone(idx)           // ZoneRef
ctx.flexzone(idx).get()     // FlexZoneT& (typed)
```

- **Index** – Bounds-checked; throws on invalid index.
- **Multiple zones** – `ctx.flexzone(0)`, `ctx.flexzone(1)`, etc.
- **Zero zones** – Overload or sentinel when no flexible zone (e.g. `with_transaction<void, DataBlockT>`).

### 4.2 Slot (Datablock Unit)

- Slot obtained from iterator: `for (auto result : ctx.slots(timeout)) { ... }`
- When `result.is_ok()`: `result.slot().get()` → `DataBlockT&`
- Producer: mutable; consumer: const.

### 4.3 Trivially Copyable Requirement

- **FlexZoneT** and **DataBlockT** must be **trivially copyable** for shared-memory access.
- **Enforcement** – `static_assert(std::is_trivially_copyable_v<T>, ...)` for both template parameters.
- **Rationale** – Shared memory is accessed across process boundaries; non-trivially-copyable types (vtables, non-POD members) are unsafe.

### 4.4 Schema Enforcement

- **Compile-time** – Template parameters must match schema; static_assert/concepts.
- **Runtime** – Verify `FlexZoneT` and `DataBlockT` against stored schema when schema-aware create/find was used.

### 4.5 Checksum

- **Policy-driven** – Checksum validation follows `ChecksumPolicy` (None, Manual, Enforced).
- **Schema** – Enforced when schema is registered.
- **Checksum** – Controlled by policy, not forced.

---

## 5. Raw Access

### 5.1 Location

Raw access is on the same objects that provide typed access:

| Typed Access | Raw Access |
|--------------|------------|
| `ctx.flexzone(idx)` | `ctx.flexzone(idx).raw_access()` |
| `result.slot()` | `result.slot().raw_access()` |

### 5.2 Semantics

- **Returns** – `std::span<std::byte>` for the full usable region.
- **Availability** – Only after schema/type/checksum checks at transaction entry.
- **User's risk** – Layout and interpretation are the user's responsibility.

### 5.3 API Sketch

```cpp
ctx.flexzone(idx).raw_access()   // span<std::byte> for flexible zone idx
result.slot().raw_access()       // span<std::byte> for slot buffer (when result.is_ok())
```

---

## 6. Iterator and ctx.slots(timeout)

### 6.1 Design

- **`ctx.slots(timeout)`** – Returns a range; iterator keeps the loop running.
- **Infinite loop** – Iterator does not end on timeout or "no slot"; yields a Result each iteration.
- **User checks** – User checks `result.is_ok()` and handles Timeout/NoSlot/Error inside the loop.
- **Events** – Timeout and coordination (MessageHub, flexible zone flags) processed in loop body.

### 6.2 Semantics

Each iteration:

1. Try to acquire next slot (block up to `timeout` ms).
2. Yield `Result<SlotRef, AcquireError>` (Ok, Timeout, NoSlot, Error).
3. User handles result; loop continues until user breaks or unrecoverable error.

### 6.3 Usage Pattern

```cpp
producer.with_transaction<FlexZoneT, DataBlockT>(timeout, [](auto& ctx) {
    ctx.flexzone(0).get().status = Status::Active;
    for (auto result : ctx.slots(timeout)) {
        if (!result.is_ok()) {
            if (result.error() == AcquireError::Timeout) {
                process_events();
            }
            if (ctx.flexzone(0).get().shutdown_requested) break;
            continue;
        }
        auto& slot = result.slot();
        slot.get().payload = produce();
        ctx.commit();
    }
    ctx.flexzone(0).get().status = Status::Idle;
});

consumer.with_transaction<FlexZoneT, DataBlockT>(timeout, [](auto& ctx) {
    for (auto result : ctx.slots(timeout)) {
        if (!result.is_ok()) {
            process_events();
            continue;
        }
        if (ctx.flexzone(0).get().end_of_stream) break;
        if (!ctx.validate_read()) continue;
        process(result.slot().get());
    }
});
```

### 6.4 Iterator End Condition

- **Normal** – Iterator does not end on Timeout or NoSlot.
- **Ends on** – Unrecoverable error (producer/consumer destroyed, fatal protocol error).
- **User exit** – User breaks based on flexible zone, MessageHub, or application logic.

### 6.5 Policy Respect

- Iterator respects `DataBlockPolicy` (Single, DoubleBuffer, RingBuffer) and `ConsumerSyncPolicy`.
- Quit logic is user responsibility (signals, events, broker, flexible zone); not buffer size.

---

## 7. Result Type

### 7.1 Choice: Result over std::optional

- **Result** – Distinguishes Timeout, NoSlot, Error; user can handle each differently.
- **std::optional** – Loses reason for "no slot"; less informative.

### 7.2 Result Variants

```cpp
enum class SlotAcquireError { Timeout, NoSlot, Error };

template <typename SlotRefT>
class SlotAcquireResult {
public:
    static SlotAcquireResult ok(SlotRefT slot);
    static SlotAcquireResult timeout();
    static SlotAcquireResult no_slot();
    static SlotAcquireResult error(int code);

    bool is_ok() const;
    SlotRefT& slot();              // only when is_ok()
    SlotAcquireError error() const;
    int error_code() const;        // when Error
};
```

### 7.3 SlotRef

- **Typed** – `slot.get()` → `DataBlockT&`
- **Raw** – `slot.raw_access()` → `std::span<std::byte>`

---

## 8. Commit Semantics

### 8.1 ctx.commit()

- **Location** – On context, not on slot.
- **Meaning** – Make current slot visible to readers; protocol step (advance commit_index, checksum, release).
- **Typed** – Commits `sizeof(DataBlockT)`; no explicit byte count.
- **When** – Called by producer after writing to current slot.

### 8.2 Guard mark_success()

- **Rename** – `WriteTransactionGuard::commit()` → `mark_success()`.
- **Meaning** – Marks transaction as successfully completed for guard state; does not make data visible.
- **Data visibility** – Controlled by `ctx.commit()` (or equivalent slot commit).

---

## 9. Heartbeat

### 9.1 Hybrid Approach

Heartbeat uses a **hybrid** model: automatic on slot operations, explicit for idle keep-alive.

### 9.2 Automatic on Slot Release

- When the consumer acquires and releases slots (including via iterator `operator++`), the underlying release path already updates read position and heartbeat.
- No user action required during normal iteration.
- Applies to Sync_reader and other policies where consumer position is tracked.

### 9.3 Explicit Interface for Idle Keep-Alive (Consumer)

- When the consumer is alive but not consuming (e.g. waiting for external events, between transactions), use **`consumer.update_heartbeat()`** to signal "I'm alive."
- Call from event loop, timer, or before long waits to avoid being treated as dead by the producer.
- **Registration** – `consumer.register_heartbeat()` at startup; `consumer.unregister_heartbeat(slot)` on shutdown.

### 9.4 Producer Heartbeat

- **Storage** – Producer heartbeat (producer_id, producer_last_heartbeat_ns) in `reserved_header` at `PRODUCER_HEARTBEAT_OFFSET`.
- **Automatic** – Updated on each slot commit (`release_write_slot`).
- **Explicit** – **`producer.update_heartbeat()`** when producer is idle (e.g. waiting for work).
- **Initial** – Set on DataBlock creation (creator PID and monotonic timestamp).
- **Liveness** – **Heartbeat-first logic**: `is_process_alive(pid)` is only called when producer heartbeat is missing or stale (e.g. > 5s). If heartbeat is fresh, treat producer as alive.
- **Use case** – Avoids unnecessary PID checks in fast paths; recovery/diagnostics use `is_writer_alive(header, pid)`.

### 9.5 Summary

| Situation | Heartbeat Update |
|-----------|------------------|
| Consumer iterating over slots | **Automatic** (on slot release) |
| Consumer idle between sessions | **Explicit** `consumer.update_heartbeat()` |
| Consumer shutdown | **Explicit** `consumer.unregister_heartbeat()` |
| Producer committing slots | **Automatic** (on slot commit) |
| Producer idle (no writes) | **Explicit** `producer.update_heartbeat()` |

### 9.7 Optional ctx.update_heartbeat()

- `ctx.update_heartbeat()` may be provided as a convenience when inside a transaction.
- Forwards to `consumer.update_heartbeat()`; no change in semantics.
- Useful when the loop body wants to signal liveness without acquiring a slot (e.g. during long event processing).

---

## 10. Configuration and Layout

### 10.1 Flexible Zone and Datablock Size

- **Logical unit size** – Space assigned to flexible zone uses the same logical unit size as datablock slots.
- **Alignment** – Both regions use the same base unit for layout consistency.

### 10.2 Flexzone-Only Mode

- **Meaning** – No datablock ring buffer; only flexible zone(s).
- **ctx.slots()** – Fails immediately (empty range or first iteration yields NoSlot/Error).
- **Use case** – Producer and consumer coordinate via flexible zone only.

---

## 11. Scenarios

### 11.1 Single Flexible Zone, No Datablock

- Producer and consumer work on flexible zone only.
- `ctx.slots()` yields no slots; iteration handles this immediately.
- Coordination via flexible zone (SharedSpinLock if needed).

### 11.2 No Flexible Zone, Datablocks Only

- Single/double/ring buffer.
- Consumer sees whole block when producer commits and moves on.
- `ctx.flexzone(idx)` unavailable or returns empty/sentinel.

### 11.3 Multiple Flexible Zones + Multiple Datablocks

- `ctx.flexzone(idx)` for each zone; bounds-checked.
- `ctx.slots(timeout)` iterates over slots; one slot per iteration.
- Iterator advances to next slot; context remains valid.

### 11.4 Blocking vs Timeout

- **Value semantics** (or typed parameter):
  - `timeout = 0` – Non-blocking, fail immediately.
  - `timeout > 0` – Wait up to N ms.
  - `timeout = -1` (or sentinel) – Block until slot available.
- Typed `Timeout` parameter optional for clarity.

---

## 12. Error Handling

### 12.1 Result for Expected Failures

- **Timeout** – Return `SlotAcquireResult::timeout()`; no throw.
- **NoSlot** – Return `SlotAcquireResult::no_slot()`; no throw.
- **Error** – Return `SlotAcquireResult::error(code)`; user can log and break.

### 12.2 Exceptions for Precondition Violations

- Out-of-range `flexzone(idx)` – Throw.
- Schema/type mismatch – Throw.
- Invalid context state – Throw.

### 12.3 Unrecoverable Errors

- Producer/consumer destroyed – Throw or end iterator.
- Fatal protocol error – Throw or `Result::Error`.

---

## 13. Deprecations and Removals

### 13.1 Layer 1.75 Removed

- **SlotRWAccess** (`slot_rw_access.hpp`) – **Removed.** Redundant with C API; typed access is via `with_typed_write<T>` / `with_typed_read<T>` on Producer/Consumer.
- **Rationale** – Redundant; C API sufficient for raw-pointer diagnostics/recovery.
- **Migration** – Use C API (`slot_rw_acquire_write`, etc.) for raw-pointer use. **C API:** Lock/multithread safety is entirely user-managed.

### 13.2 Layer 2 Replaced

- **with_write_transaction**, **with_read_transaction** – Replaced by `with_transaction` member.
- **with_typed_write**, **with_typed_read** – Replaced by typed `with_transaction`.
- **with_next_slot** – Replaced by `ctx.slots()` iterator.
- **Free functions** – May remain as thin wrappers for backward compatibility during transition.

### 13.3 Guard Rename

- **WriteTransactionGuard::commit()** → **mark_success()**

---

## 14. Implementation Notes

### 14.1 C++20 Requirements

- Ranges for `ctx.slots(timeout)`.
- Concepts for lambda parameter enforcement (`std::invocable`).
- `std::optional` or custom `Result` type (C++23 `std::expected` if available).

### 14.2 ABI and pImpl

- All public types use pImpl for ABI stability.
- Template implementations in headers; non-template in .cpp.

### 14.3 Thread Safety

- **Producer and Consumer (C++ API):** Thread-safe via internal mutex. Multiple threads may share one producer or one consumer; only one slot/context is active at a time per handle.
- **C API (slot_rw_coordinator.h, recovery_api):** No internal locking; multithread safety is the caller's responsibility.

### 14.4 Testing Priorities

- Typed access (schema enforcement, size/alignment).
- Iterator (infinite loop, Result variants, event handling in loop).
- Flexzone-only and no-flexzone scenarios.
- Raw access (after checks, user's risk).
- Exception safety (context release on throw).

---

## Appendix A: API Summary

| API | Description |
|-----|-------------|
| `producer.with_transaction<FlexZoneT, DataBlockT>(timeout, func)` | Establish write context; invoke func(ctx). |
| `consumer.with_transaction<FlexZoneT, DataBlockT>(timeout, func)` | Establish read context; invoke func(ctx). |
| `ctx.flexzone(idx)` | ZoneRef with .get() and .raw_access(). |
| `ctx.slots(timeout)` | Range yielding Result<SlotRef, AcquireError>; infinite until error or user break. |
| `ctx.commit()` | Make current slot visible to readers (producer only). |
| `ctx.validate_read()` | Check slot still valid (consumer only). |
| `ctx.update_heartbeat()` | Optional convenience; forwards to consumer.update_heartbeat(). |
| `result.slot().get()` | DataBlockT& (typed). |
| `result.slot().raw_access()` | span<std::byte> (raw). |
| `ctx.flexzone(idx).raw_access()` | span<std::byte> for zone (raw). |
| `consumer.register_heartbeat()` | Register consumer in heartbeat table (startup). |
| `consumer.update_heartbeat()` | Explicit keep-alive when idle. |
| `consumer.unregister_heartbeat(slot)` | Unregister on shutdown. |

---

## Appendix B: Merge Targets

After implementation and testing, merge into:

1. **`docs/HEP/HEP-CORE-0002-DataHub-FINAL.md`** – C++ abstraction section.
2. **`docs/IMPLEMENTATION_GUIDANCE.md`** – C++ layer map, patterns, pitfalls.
3. **`docs/examples/RAII_LAYER_USAGE_EXAMPLE.md`** – Usage examples.
4. **`docs/DATAHUB_TODO.md`** – Update Phase 2 checklist.

Then move this draft to `docs/archive/transient-YYYY-MM-DD/` with README listing merge locations.

---

**Revision History:**
- v0.3 (2026-02-13): Layer 1.75 removed (slot_rw_access.hpp); Producer/Consumer made thread-safe (internal mutex); C API documented as user-managed locking; §14.3 Thread Safety.
- v0.2 (2026-02-13): Added §9 Heartbeat (hybrid: automatic on slot release, explicit for idle); §4.3 Trivially copyable requirement for FlexZoneT and DataBlockT; Appendix A heartbeat APIs.
- v0.1 (2026-02-13): Initial draft based on design discussion.
