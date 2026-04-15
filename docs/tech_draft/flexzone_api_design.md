# Tech Draft: Flexzone API Design

**Status**: DRAFT 2026-04-15 — pending user sign-off before any code changes.
**Branch**: `feature/lua-role-support`
**Anchors**: HEP-CORE-0002 §2.2 (TABLE 1 Flexible Zone), `role_unification_design.md` §2.3 / §3.1 / §3.3.
**Prerequisite**: fold L3.γ (delete `hub::Producer` / `hub::Consumer`) into this sprint — see anchors above.

---

## 1. Design premise (non-negotiable, cited from HEP-CORE-0002 §2.2)

The flexzone is **TABLE 1** of the DataBlock: user-managed metadata / state / telemetry / coordination region. Coordination is **user-managed** — "std::atomic members, ZeroMQ, external sync". Listed use cases include multi-channel synchronization counters shared by both ends of a channel.

Concrete properties:

- **One region per channel.** Every DataBlock has one flexzone region.
- **Shared across both endpoints.** Writer-side and reader-side handles into the same DataBlock both see the same physical shared-memory bytes. Proven at the hub layer by `ShmQueueFlexzoneRoundTrip` (`tests/test_layer3_datahub/workers/datahub_hub_queue_workers.cpp:441-483`) which writes via the writer-side handle and `memcmp`s identical bytes back via the reader-side handle.
- **Fully read+write on both endpoints.** There is no permission polarity between writer-side and reader-side handles. The region is bidirectional by design so user code can implement coordination primitives (atomics, spinlocks, counters) visible to both ends.
- **ZMQ has no flexzone.** The DataBlock/TABLE-1 concept is SHM-only. ZmqQueue channels report zero flexzone size; `flexzone()` returns nullptr.

The `read_flexzone()` / `write_flexzone()` verb-pair naming in current code encodes a **permission polarity that does not exist**. The only real distinction is "which channel's flexzone" (i.e., which side of a dual-channel role), and that is addressable by either the receiver object (for scripts: `rx` vs `tx`) or an explicit `ChannelSide` parameter (for C++ accessors). The verb pair is deleted.

Operations genuinely directional (checksum stamp vs verify, slot acquire vs release) keep their direction prefixes. Only the **flexzone pointer accessor** loses the verb.

---

## 2. Layer-by-layer API contract

### 2.1 Hub layer (`QueueReader`, `QueueWriter`, `ShmQueue`, `ZmqQueue`)

Permanent — these classes survive L3.γ. Base-class virtuals:

```cpp
// hub_queue.hpp — QueueReader  (input handle into a channel)
virtual void  *flexzone() noexcept { return nullptr; }        // mutable pointer
virtual size_t flexzone_size() const noexcept { return 0; }

// hub_queue.hpp — QueueWriter  (output handle into a channel)
virtual void  *flexzone() noexcept { return nullptr; }        // mutable pointer
virtual size_t flexzone_size() const noexcept { return 0; }
```

Same method name on both bases; from a single handle there is no side ambiguity — the handle itself is one side.

`ShmQueue` inherits from both (multiple inheritance). One override of `flexzone()` on `ShmQueue` satisfies both virtuals simultaneously:
```cpp
void *ShmQueue::flexzone() noexcept override;
size_t ShmQueue::flexzone_size() const noexcept override;
```

Impl: reads `pImpl->dbp->flexible_zone_span()` (writer-mode) or `pImpl->dbc->flexible_zone_span()` (reader-mode), returning `void *` either way. Guard: `pImpl->fz_sz == 0 → nullptr`. No `producer()` / `consumer()` getter; use `dbp` / `dbc` direct. `const_cast` on the reader-side span result is justified by HEP-CORE-0002 §2.2 (region is writable on both ends).

`ZmqQueue` does not override; inherits default nullptr/0.

**Deleted at this layer**:
- `QueueReader::read_flexzone()` and its `ShmQueue` override.
- `QueueWriter::write_flexzone()`, `QueueWriter::read_flexzone()`, and their `ShmQueue` overrides.
- `ShmQueueImpl::producer()` / `consumer()` getters (duplicates of `dbp.get()` / `dbc.get()`; ~20 in-file callers rewritten to use `dbp` / `dbc` directly).
- `ShmQueue::raw_producer()` / `raw_consumer()` public methods (only callers are `hub::Producer` / `hub::Consumer`, both being deleted in L3.γ).

### 2.2 `hub::Producer` / `hub::Consumer`

**Deleted entirely** (role_unification_design.md §3.1:185). State migrates into `RoleAPIBase::Impl`:
- `unique_ptr<QueueWriter> tx_queue_` (was `hub::Producer::queue_writer_`)
- `unique_ptr<QueueReader> rx_queue_` (was `hub::Consumer::queue_reader_`)
- event-callback slots (on_channel_closing, on_force_shutdown, on_consumer_died, on_channel_error)
- broker-registration lifetime state
- realtime-mode handler state (unused by the ScriptHost path; `cpp_processor_template.cpp` example updated or removed since it's the only caller of realtime handlers).

The flexzone verb-pair methods on these classes die with the classes.

### 2.3 Role API layer (`RoleAPIBase`)

`RoleAPIBase::Impl` directly owns `tx_queue_` (`unique_ptr<QueueWriter>`) and `rx_queue_` (`unique_ptr<QueueReader>`). CycleOps reaches them through the following accessor on `RoleAPIBase`:

```cpp
// Side-parameterized pointer accessor — consistent with the existing
// side-parameterized accessors in the same class (flexzone_logical_size(side),
// slot_logical_size(side), shared_spinlock(side), spinlock_count(side)).
void  *flexzone(ChannelSide side);
size_t flexzone_size(ChannelSide side) const noexcept;
```

Forwarding:
```cpp
void *RoleAPIBase::flexzone(ChannelSide s) {
    if (s == ChannelSide::Tx) return pImpl->tx_queue ? pImpl->tx_queue->flexzone() : nullptr;
    /* ChannelSide::Rx */       return pImpl->rx_queue ? pImpl->rx_queue->flexzone() : nullptr;
}
```

**Deleted at this layer**:
- `RoleAPIBase::write_flexzone()` / `read_flexzone()` (verb pair).
- `RoleAPIBase::write_flexzone_size()` / `read_flexzone_size()` (subsumed by side-parameterized form).
- `RoleAPIBase::flexzone_size()` (no-arg, legacy, ambiguous).

The three `const_cast<void *>(api_.read_flexzone())` call sites in `cycle_ops.hpp` (lines 152, 184, 232) all disappear — the new accessor returns non-const already.

### 2.4 `RoleHostCore`

Vocabulary consistency rename:
- `has_out_fz()` → `has_tx_fz()`
- `has_in_fz()`  → `has_rx_fz()`

All callers updated. "out"/"in" were local variable-naming leakage from pre-L3 code; aligned now with the Tx/Rx vocabulary used everywhere else.

### 2.5 Cycle path (`cycle_ops.hpp`)

CycleOps call sites become:
```cpp
// ProducerCycleOps ctor
fz_ptr_(c.has_tx_fz() ? api.flexzone(ChannelSide::Tx)      : nullptr),
fz_sz_ (c.has_tx_fz() ? api.flexzone_size(ChannelSide::Tx) : 0)

// ConsumerCycleOps invoke_and_commit
void  *fz_ptr = core_.has_rx_fz() ? api_.flexzone(ChannelSide::Rx)      : nullptr;
size_t fz_sz  = core_.has_rx_fz() ? api_.flexzone_size(ChannelSide::Rx) : 0;

// ProcessorCycleOps ctor + per-cycle re-read in invoke_and_commit
out_fz_ptr_(c.has_tx_fz() ? api.flexzone(ChannelSide::Tx) : nullptr),
in_fz_ptr_ (c.has_rx_fz() ? api.flexzone(ChannelSide::Rx) : nullptr),
// sizes likewise
```

No `const_cast`. No verb names.

### 2.6 Script layer

Scripts already receive `rx` and `tx` objects (channel-side envelopes) as callback arguments. The flexzone is addressed through these receivers — the receiver object IS the side.

**Decision**: method form on the channel object, `rx.flexzone()` / `tx.flexzone()`, returning a typed mutable wrapper. Attribute form `rx.fz` / `tx.fz` retained as a deprecated alias for one sprint to avoid breaking scripts, then removed. Symmetric with future `rx.flexzone_logical_size()` / `tx.flexzone_logical_size()` additions and with the C++ `flexzone(side)` accessor.

No behavior change at the engine wrapping layer — it still receives `void *fz` from `InvokeRx`/`InvokeTx` and constructs the typed mutable wrapper as today (`python_engine.cpp:937, 973-978, 1015-1022` with `read_only=false`; Lua equivalent). The method `flexzone()` simply returns the already-constructed wrapper.

**Deleted at script layer**:
- `ProducerAPI::flexzone()` method (`producer_api.cpp:31`). Never worked — `flexzone_obj_` was declared but never assigned anywhere in the tree. Python scripts always saw `None`.
- `ProcessorAPI::flexzone()` method (`processor_api.cpp:30`). Same dead state.
- `flexzone_obj_` members in `ProducerAPI` / `ProcessorAPI` header.
- pybind11 bindings `.def("flexzone", ...)` in both.
- Lua `lua_api_flexzone` (`lua_engine.cpp:2015-2043`). Was the only working version but exposed a wrong mental model: comment `"Only producer/processor have writable flexzone"` is the exact HEP-0002 §2.2 violation we're fixing; the method bypassed `RoleAPIBase` and reached directly into `hub::Producer` (which is being deleted).
- Registration of `"flexzone"` closure in `lua_engine.cpp:324, 352`.
- `test_lua_engine.cpp:2479` test that exercises `api.flexzone()`.

`ConsumerAPI` never had `flexzone()` — don't add one.

**Kept at script layer**: `api.flexzone_logical_size(std::optional<ChannelSide>)` is used for metadata queries; it stays (it already takes `ChannelSide`). `api.update_flexzone_checksum()` stays (genuinely directional op: writer stamps). `api.sync_flexzone_checksum()` stays.

---

## 3. Required new tests

Every bug in this area survived because test coverage stops at the hub-layer `ShmQueueFlexzoneRoundTrip`. Adding four tests closes the gap.

### T1 — Hub-layer bidirectional visibility

File: `tests/test_layer3_datahub/workers/datahub_hub_queue_workers.cpp`.
Scenario: single SHM channel, writer-side + reader-side ShmQueue handles. Writer writes sentinel via `write_q->flexzone()`, commits a slot; reader `memcmp`s the sentinel via `read_q->flexzone()` (already proven). **New coverage**: reader writes into its own flexzone via `read_q->flexzone()`, writer observes the value via `write_q->flexzone()` (proves `void *` on both ends, proves HEP-CORE-0002 §2.2 bidirectional at hub layer).

### T2 — Role-level producer→consumer flexzone round-trip

File: `tests/test_layer3_datahub/` (new — `test_datahub_role_flexzone.cpp`).
Spawn producer role-host + consumer role-host wired to the same SHM channel. `on_produce` writes sentinel via `tx.flexzone()`, commits slot. `on_consume` asserts `rx.flexzone()` is non-null and contents match sentinel.

### T3 — Role-level consumer-side write visibility

Same test file as T2. `on_consume` writes an ack value into its own `rx.flexzone()`. Producer's `on_produce` reads the ack back from `tx.flexzone()` (same physical region) and asserts the value. Exercises HEP-CORE-0002 §2.2 bidirectional coordination at the script layer.

### T4 — Processor dual-flexzone distinctness

Same file as T2. Upstream producer role-host → SHM channel A → processor role-host → SHM channel B → downstream consumer role-host. Processor's `on_process` writes **different** sentinels into `rx.flexzone()` and `tx.flexzone()`. Asserts the two pointers are distinct (two channels, two regions). Upstream producer reads back the `rx.flexzone()` sentinel from its own `tx.flexzone()` pointer (proving the processor's Rx-fz aliases to the upstream channel); downstream consumer reads the `tx.flexzone()` sentinel from its own `rx.flexzone()` pointer.

### Deleted test

- `test_lua_engine.cpp:2479` `Api_Flexzone_WithoutSHM_ReturnsNone` — `api.flexzone()` no longer exists at the script layer.

---

## 4. Implementation sequence

Each step is one commit. Intermediate builds may be incomplete but each commit itself is green (builds + existing tests pass + new tests from that commit pass).

**Commit 1 — Hub layer `flexzone()` API + internal cleanup.**
- Add `flexzone()` / `flexzone_size()` to `QueueReader` and `QueueWriter` virtual bases.
- Override in `ShmQueue` (one method per signature satisfies both parents).
- Keep existing `read_flexzone()` / `write_flexzone()` temporarily (deleted in commit 4).
- Delete `ShmQueueImpl::producer()` / `consumer()` getters; rewrite all in-file callers to `dbp` / `dbc` directly. No public API change.
- Add T1 (worker function + TEST_F).
- Build + existing 1257+ tests green + T1 green.

**Commit 2 — `RoleAPIBase::Impl` absorbs `hub::Producer` state; delete `hub::Producer`.**
- `RoleAPIBase::Impl` gains `unique_ptr<QueueWriter> tx_queue_` + event-callback slots + broker-registration lifetime.
- `RoleAPIBase` new method `flexzone(ChannelSide::Tx)` routes to `pImpl->tx_queue->flexzone()`.
- `producer_role_host.cpp` constructs `RoleAPIBase` with the queue writer directly (no intermediate `hub::Producer`).
- Delete `src/include/utils/hub_producer.hpp` + `src/utils/hub/hub_producer.cpp`.
- Delete realtime-handler plumbing (`WriteProcessorContext`, `ProducerMessagingFacade`, etc.) — move `cpp_processor_template.cpp` example or remove it if unused.
- `ShmQueue::raw_producer()` public method removed (no more callers).

**Commit 3 — Same for `hub::Consumer`.**
- `RoleAPIBase::Impl` gains `unique_ptr<QueueReader> rx_queue_` + Rx-side event-callback slots + broker-discovery lifetime.
- `RoleAPIBase::flexzone(ChannelSide::Rx)` routes to `pImpl->rx_queue->flexzone()`.
- `consumer_role_host.cpp` + `processor_role_host.cpp` migrated.
- Delete `src/include/utils/hub_consumer.hpp` + `src/utils/hub/hub_consumer.cpp`.
- `ShmQueue::raw_consumer()` public method removed.

**Commit 4 — Delete the verb-pair API and the script-layer dead code.**
- Delete deprecated `read_flexzone()` / `write_flexzone()` from `QueueReader` / `QueueWriter` bases + `ShmQueue` overrides.
- Delete `RoleAPIBase::write_flexzone()` / `read_flexzone()` / `flexzone_size()` / `write_flexzone_size()` / `read_flexzone_size()`.
- `RoleHostCore::has_out_fz()` → `has_tx_fz()`, `has_in_fz()` → `has_rx_fz()`, all callers updated.
- `cycle_ops.hpp`: migrate to `api.flexzone(side)`. Drop three `const_cast`s.
- Delete `ProducerAPI::flexzone()` / `ProcessorAPI::flexzone()` + `flexzone_obj_` members + pybind11 bindings.
- Delete `lua_api_flexzone` + its closure registration.
- Delete `test_lua_engine.cpp:2479` test.
- Build + full suite green.

**Commit 5 — Script-layer `rx.flexzone()` / `tx.flexzone()` methods (Option B).**
- Add `flexzone()` method on `PyRxChannel` / `PyTxChannel` returning the already-wrapped `fz` typed object (no engine change — just a method that returns the same typed wrapper currently exposed as attribute `fz`).
- Lua equivalent on the rx/tx tables.
- If keeping both method AND attribute for compatibility: fine. If collapsing to method-only: update any existing test/doc using `.fz` attribute.

**Commit 6 — Tests T2, T3, T4.**
- New test file `test_datahub_role_flexzone.cpp` with three role-level scenarios.
- Workers use the `IsolatedProcessTest` pattern (L3 style).

**Commit 7 — Docs.**
- Update `HEP-CORE-0011` (ScriptHost Abstraction) to reflect new API shape.
- Archive this design doc per `docs/DOC_STRUCTURE.md`.

---

## 5. What this design does NOT change

- `InvokeTx.fz` / `InvokeRx.fz` struct fields: already `void *fz` + `size_t fz_size`. No change.
- Engine cycle path (`python_engine.cpp`, `lua_engine.cpp` `invoke_produce/consume/process`): already wraps `tx.fz` / `rx.fz` as mutable typed objects. No change.
- Checksum operations: stay directional. `update_flexzone_checksum` (Tx-side stamp), `sync_flexzone_checksum` (Tx-side initial stamp), `verify_flexzone_checksum` (Rx-side verify). These are genuine direction-scoped operations per HEP-CORE-0002.
- `ChannelSide` enum: unchanged. Already `{Tx, Rx}`. Used throughout the side-parameterized accessor family.
- Flexzone schema configuration, logical vs physical size distinction, `flexzone_logical_size(side)`: unchanged.
- ZMQ transport: unchanged (no flexzone, `flexzone()` returns nullptr via base class default).

---

## 6. Decisions locked

All decisions above are locked against HEP-CORE-0002 §2.2, `role_unification_design.md` §2.3 / §3.1, and the existing engine behavior (`python_engine.cpp:975`, `lua_engine.cpp` fz-alias cache with `readonly=false`). This document is the plan of record for execution; no further sign-off gates between commits.
