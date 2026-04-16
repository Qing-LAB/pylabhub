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

**Decision** (updated 2026-04-15, supersedes prior "method on channel object" plan): `api.flexzone(side=…)` — a side-parameterized method on the `api` object, mirroring the existing `api.spinlock(index, side)` / `api.flexzone_logical_size(side)` / `api.slot_logical_size(side)` / `api.shared_spinlock(side)` side-parameterized family.

**Rationale**:
- Consistency with the rest of the `api.*(side)` family (single selector convention across spinlock / logical-size / etc.).
- Init-stage-only cost for the typed-wrapper construction (see below). The per-cycle hot path loses one ctypes/ffi view construction per cycle on each flexzone-using role; Python/Lua scripts that call `api.flexzone(side)` get a cached object.
- `rx.fz` / `tx.fz` and the `InvokeRx::fz` / `InvokeTx::fz` struct fields — the per-cycle wrapping path — are **deleted**, not deprecated, since scripts now reach the flexzone exclusively through `api.flexzone(side)`. The `InvokeRx` / `InvokeTx` structs carry only the slot pointer and size.

**Signature**:

```cpp
// Producer / Consumer / Processor APIs (Python + Lua)
py::object api.flexzone(std::optional<int> side = std::nullopt);
uint64_t   api.flexzone_logical_size(std::optional<int> side = std::nullopt);  // already exists
uint32_t   api.flexzone_physical_size(std::optional<int> side = std::nullopt); // add for symmetry
```

- `side` is `std::optional<int>` mapped to `ChannelSide::Tx=0` / `ChannelSide::Rx=1`.
- Omitted for single-side roles (auto-selects the only wired side).
- Required for processor — `api.flexzone()` without a `side` throws `"side parameter required for processor"`, same failure mode as `api.spinlock()` without a side.
- Returns `None` / nil when the requested side has no flexzone configured (matches `api.flexzone_logical_size(side)` behavior).

**Init-stage cache**:

At `build_api()` time (after `RoleAPIBase` has its `tx_queue` / `rx_queue` wired), each API class pre-constructs the typed view once:

```cpp
// Python side (pybind11) — in ProducerAPI / ConsumerAPI / ProcessorAPI::build_*():
if (base_->flexzone(ChannelSide::Tx) && base_->flexzone_size(ChannelSide::Tx) > 0
    && !out_fz_type_.is_none())
    tx_flexzone_obj_ = make_typed_view(out_fz_type_,
                                       base_->flexzone(ChannelSide::Tx),
                                       base_->flexzone_size(ChannelSide::Tx),
                                       /*read_only=*/false);
// ... same for Rx via in_fz_type_ / ChannelSide::Rx.
```

`tx_flexzone_obj_` / `rx_flexzone_obj_` are cached `py::object` (Python) / ffi cdata (Lua) members on the API class. `api.flexzone(side)` just returns the cached object for the requested side. Zero per-cycle allocations.

Lua uses the same pattern with pre-built ffi typeof references and cached cdata.

**Pointer stability caveat**:

The init-only cache is valid while the underlying SHM flexzone pointer is stable. For the DataBlock lifecycle (one fixed mapping from queue start through queue stop), the pointer is constant — `flexible_zone_span().data()` returns the same offset-into-header every call. The per-cycle re-read in `cycle_ops.hpp:79-81, 156-158, 258-261` ("Re-read flexzone pointer each cycle — ShmQueue may move it") is a defensive comment from an earlier design; in steady state it reads the same pointer every time.

The cache **must be invalidated** if:
- SHM is remapped (recovery path). The role host must rebuild `RoleAPIBase` + the API instance on remap, so the cache is discarded with the old API object. Confirmed pattern: role hosts today already construct `RoleAPIBase` after SHM wiring and don't reattach to a new DataBlock in the middle of a role's lifetime. Recovery = full restart.
- Queue is stopped and restarted with a fresh DataBlock. Again: new API instance, new cache.

**Consequence — Commit C scope change**:

- Delete `InvokeRx::fz` and `InvokeTx::fz` struct fields (and `fz_size`).
- Engine stops populating `rx_ch.fz` / `tx_ch.fz`. `make_slot_view_(…, fz, …)` calls in `python_engine.cpp:937, 973-978, 1015-1022` removed. Lua equivalent removed.
- `cycle_ops.hpp` stops passing flexzone pointer + size into `InvokeTx` / `InvokeRx`. The `tx_fz_ptr_` / `out_fz_ptr_` / `in_fz_ptr_` / `fz_ptr_` fields in `*CycleOps` all get deleted — CycleOps no longer needs to carry flexzone state at all. `api.flexzone(side)` is the sole script-facing path.
- `ProducerAPI::flexzone()` and `ProcessorAPI::flexzone()` **are kept and implemented correctly** (currently return `None` because `flexzone_obj_` is never assigned — now assigned at `build_api()`). `ConsumerAPI::flexzone(side=...)` is added (doesn't exist today).
- Lua `lua_api_flexzone` stays but re-implemented: takes optional side arg, returns the cached ffi cdata for that side. No more `producer->write_flexzone()` direct-access; everything routes through `api_->flexzone(side)`.
- Old `api.flexzone()` test at `test_lua_engine.cpp:2479` (returns nil without SHM) becomes the **trivial edge-case assertion** — still valid, still passes; gets paired with new tests that assert `api.flexzone(side)` returns a typed object when SHM is wired.
- Processor scripts that previously used `rx.fz` / `tx.fz` now use `api.flexzone(Rx)` / `api.flexzone(Tx)`. This is a breaking change for any existing script using `rx.fz` / `tx.fz` — update all in-tree scripts (producer / consumer / processor example scripts) in the same commit.

**Kept at script layer** (unchanged): `api.flexzone_logical_size(std::optional<ChannelSide>)`, `api.slot_logical_size(side)`, `api.shared_spinlock(side)`, `api.spinlock(index, side)`, `api.spinlock_count(side)`, `api.update_flexzone_checksum()`, `api.sync_flexzone_checksum()`.

### 2.7 Native engine (C/C++)

C/C++ native plugins access flexzone through `plh_tx_t.fz` / `plh_rx_t.fz` fields on the invoke struct — zero-cost stack copy from the bridge's init-time cache. No separate `ctx->flexzone(side)` function is needed; the invoke struct IS the access path. This is consistent with how slots are accessed (`tx->slot` / `rx->slot`).

The bridge (`native_engine.cpp`) caches flexzone pointers + sizes once at `wire()` time on `NativeContextStorage`, then copies them into `plh_tx_t.fz` / `plh_rx_t.fz` at each invoke call. Since the SHM flexzone region is at a fixed offset in the DataBlock header, the cached pointers are stable for the role's lifetime.

`PLH_EXPORT_PRODUCE(SlotType, FlexType, func)` and related C++ export macros continue to wrap `tx->fz` / `rx->fz` into typed `SlotRef<FlexType>` — no change needed.

**Cross-engine consistency summary:**
- **Python/Lua**: `api.flexzone(side)` → init-time cached typed view (avoids per-cycle ctypes/ffi reconstruction)
- **C/C++**: `tx->fz` / `rx->fz` on invoke struct → zero-cost pointer from bridge cache (no typed-view reconstruction needed in C++)
- Both paths read from the same underlying cached pointer, populated once from `api->flexzone(ChannelSide)` at init time.

---

## 3. Required new tests

Every bug in this area survived because test coverage stops at the hub-layer `ShmQueueFlexzoneRoundTrip`. Adding four tests closes the gap.

### T1 — Hub-layer bidirectional visibility

File: `tests/test_layer3_datahub/workers/datahub_hub_queue_workers.cpp`.
Scenario: single SHM channel, writer-side + reader-side ShmQueue handles. Writer writes sentinel via `write_q->flexzone()`, commits a slot; reader `memcmp`s the sentinel via `read_q->flexzone()` (already proven). **New coverage**: reader writes into its own flexzone via `read_q->flexzone()`, writer observes the value via `write_q->flexzone()` (proves `void *` on both ends, proves HEP-CORE-0002 §2.2 bidirectional at hub layer).

### T2 — Role-level producer→consumer flexzone round-trip

File: `tests/test_layer3_datahub/` (new — `test_datahub_role_flexzone.cpp`).
Spawn producer role-host + consumer role-host wired to the same SHM channel. `on_produce` writes sentinel via `api.flexzone()` (auto-selects Tx for producer), commits slot. `on_consume` asserts `api.flexzone()` (auto-selects Rx for consumer) is non-None and contents match sentinel.

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
- Delete realtime-handler plumbing (`WriteProcessorContext`, `ProducerMessagingFacade`, `set_process_handler<>()`, HEP-CORE-0002 §6.9.1 ABI-frozen facade structs) — no production binary uses it; only `examples/cpp_processor_template.cpp` does. Delete that example along with the API it depends on.

  **Replacement**: after the full refactor (Commits A–F) lands, rewrite `examples/cpp_processor_template.cpp` against the surviving C++ RAII framework — `with_transaction<F,D>()` + `SlotIterator` + the unified `ZoneRef<F>` + the surviving `RoleAPIBase` / `QueueWriter` / `QueueReader` surface. The rewrite is a new example file, not a salvage of the realtime-handler path, and belongs in the L3.ζ doc sprint (same phase as HEP-CORE-0011 rewrite and design-doc archival).
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

**Commit F — RAII template layer (`zone_ref.hpp` + `transaction_context.hpp`).**

The RAII layer at `src/include/utils/zone_ref.hpp` has the same
HEP-CORE-0002 §2.2 violation as the hub/role layers did pre-cleanup:
`ZoneRef<FlexZoneT, IsMutable>` with `WriteZoneRef<T> = ZoneRef<T,true>`
(producer, mutable) and `ReadZoneRef<T> = ZoneRef<T,false>` (consumer,
const). `get()` returns `const FlexZoneT &` on the reader side; `raw_access()`
returns `std::span<const std::byte>`. Consumer-side coordination writes
(e.g., shared counters for frame-ID matching per §2.2) cannot compile
through this API without a `const_cast` — the same permission fiction we
removed from the hub and role layers.

`TransactionContext<F, D, IsWrite>::flexzone()` picks the ZoneRef
specialization via `std::conditional_t<IsWrite, WriteZoneRef<F>, ReadZoneRef<F>>`
(`transaction_context.hpp:93`). Same fiction, same source.

HEP-CORE-0002 §6.3 documents the existing table:

    | ctx.flexzone() | WriteZoneRef<F> (mutable) | ReadZoneRef<F> (const) |

That row is the bug, not a spec.

**Fix**:

- Drop `IsMutable` parameter from `ZoneRef`. Single class `ZoneRef<FlexZoneT>`.
- Both constructors allowed: `explicit ZoneRef(DataBlockProducer *)` AND
  `explicit ZoneRef(DataBlockConsumer *)`. The impl stores a
  `std::span<std::byte>` obtained from whichever handle was passed in;
  the consumer-side span comes via `const_cast<std::byte *>` at the
  boundary (justified by HEP-0002 §2.2 bidirectional design).
- `get()` returns `FlexZoneT &` (mutable). The separate
  `get() const requires(!std::is_void_v<FlexZoneT>)` overload returning
  `const FlexZoneT &` is kept for callers who hold a const ZoneRef (it's
  not about permission, it's about the handle's constness).
- `raw_access()` returns `std::span<std::byte>` regardless of side.
- `WriteZoneRef<T>` and `ReadZoneRef<T>` aliases both become
  `using WriteZoneRef<T> = ZoneRef<T>;` / `using ReadZoneRef<T> = ZoneRef<T>;`
  for one sprint (existing user code compiles), then deprecated and
  removed in the L3.ζ doc sprint.
- `TransactionContext::flexzone()` returns `ZoneRef<F>` unconditionally;
  remove the `std::conditional_t` dispatch and the
  `flexzone() const requires(!IsWrite)` overload (no longer needed).

**Callers to audit**:

- `tests/test_layer3_datahub/workers/datahub_transaction_api_workers.cpp`
  (extensive `with_transaction` + flexzone tests).
- `tests/test_layer3_datahub/workers/datahub_stress_raii_workers.cpp`.
- `examples/` — any C++ example using `with_transaction`.

**New test for Commit F**: add a `TransactionContext`-layer bidirectional
test at the RAII layer (parallel to `ShmQueueFlexzoneBidirectional` added
in `b616d7b` at the hub layer). Consumer `with_transaction<F,D>(read)`
writes a counter into `ctx.flexzone().get()`; producer
`with_transaction<F,D>(write)` reads the counter back from its own
`ctx.flexzone().get()`; `EXPECT_EQ` on the value. Proves the RAII layer
is HEP-0002 §2.2 compliant end-to-end.

**HEP doc update**:

- HEP-CORE-0002 §6.3 table row for `ctx.flexzone()` becomes:
  `| ctx.flexzone() | ZoneRef<F> (mutable) | ZoneRef<F> (mutable) |`
- HEP-CORE-0002 §6.5 `ReadZoneRef` / `WriteZoneRef` section collapses to
  a single `ZoneRef<F>` paragraph; the const claim is deleted.

**Ordering**: Commit F is independent of hub::Producer/Consumer deletion
(Commits A/B). Can land before, alongside, or after them. Recommend
scheduling it after A/B so the hub/role layer cleanup is fully in place
first — any surprise caller dependency between RAII and hub layers shows
up as a clean compile failure in one direction only.

**A5b–A5g — introduce `utils::ThreadManager`**
See `docs/tech_draft/thread_manager_design.md` for the full spec. Summary:
per-owner value-composed utility class that provides bounded-join + detach-on-timeout
+ ERROR-log diagnostic. Each instance auto-registers as a dynamic lifecycle module
`"ThreadManager:" + owner_tag` so it integrates with the existing `LifecycleGuard`
shutdown ordering + reuses the `timedShutdown` safety net.

Migration order: A5b RoleAPIBase → A5c BrokerRequestComm → A5d BrokerService
→ A5e ZmqQueue → A5f InboxQueue/InboxClient → A5g Logger. Each a separate commit,
each with at least one new test verifying the bounded-shutdown contract.

After A5g, every background thread in the tree runs under a ThreadManager. The
pre-existing 60s silent-hang pattern (observed today in `BrokerSchemaTest.*`,
`ConsumerCliTest.Validate_ExitZero`, `NativeEngineTest.FullStartup_*`) becomes a
5s logged event that identifies the stuck thread by owner tag.

**Commit 7 — Docs + example rewrite (L3.ζ).**
- Update `HEP-CORE-0011` (ScriptHost Abstraction) to reflect new API shape.
- Update `HEP-CORE-0002` §6.3 and §6.5 per Commit F.
- Rewrite `examples/cpp_processor_template.cpp` against the new C++ RAII
  framework: `with_transaction<F,D>()` + `SlotIterator` + unified `ZoneRef<F>`
  + surviving `RoleAPIBase` / `QueueWriter` / `QueueReader`. New file; not a
  salvage of the old realtime-handler implementation.
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
