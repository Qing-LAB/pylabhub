# DataHub C++ Abstraction Layer – Design

**Purpose:** Define a consistent, satisfactory C++ abstraction layer on top of the confirmed C API primitives (Slot RW, Recovery). This document is the design reference for implementation; once agreed, merge key content into `IMPLEMENTATION_GUIDANCE.md` and HEP as needed.

**Status:** Design (pre-implementation). Execution order and checklist: `docs/DATAHUB_TODO.md`. C API review: `docs/DATAHUB_DATABLOCK_CRITICAL_REVIEW.md` §7.

**Doc policy (per DOC_STRUCTURE.md):** Topic-specific design; merge into master docs by topic when stable.

---

## 1. Principles (from IMPLEMENTATION_GUIDANCE and DATAHUB_TODO)

- **C API is the stable base.** Slot RW C API and Recovery C API are complete and correct; all higher-level design builds on them or on C++ wrappers that ultimately use them.
- **C++ abstraction is the default** for application code, broker integration, and tests. Use the C API directly only when performance or flexibility (e.g. custom bindings, hot loops without exceptions) critically require it.
- **Layered API:** Three conceptual layers — (0) C API, (1) C++ primitive handles and Producer/Consumer, (1.75) typed/template helpers, (2) transaction API (guards and with_*). Design so that Layer 2 is the recommended entry point for most code.
- **Exception and error policy:** Guards and accessors that cross the shared-library boundary are `noexcept` where possible (e.g. `slot()` returns `std::optional`); acquisition failures in `with_*` can throw. Document clearly so callers know what to expect.

---

## 2. Layer map (current and target)

| Layer   | Name              | What it is                                                                 | When to use |
|---------|-------------------|-----------------------------------------------------------------------------|-------------|
| **0**   | C API             | `slot_rw_*`, `datablock_*` (recovery), `SlotAcquireResult`, `DataBlockMetrics`, `RecoveryResult`. | C bindings, minimal dependencies, or when the C++ layer cannot be used. |
| **1**   | C++ primitive     | `DataBlockProducer`, `DataBlockConsumer`, `SlotWriteHandle`, `SlotConsumeHandle`, `acquire_write_slot` / `release_write_slot`, `acquire_consume_slot` / `release_consume_slot`, `DataBlockDiagnosticHandle`, `open_datablock_for_diagnostic`. | When you need explicit control over slot lifetime or when integrating with code that cannot use exceptions. |
| **1.75**| Typed/template    | `SlotRWAccess::with_typed_write<T>`, `with_typed_read<T>` (on raw `SlotRWState*` + buffer). Optionally: Producer/Consumer-facing typed helpers (see §4). | Type-safe access when you already have `SlotRWState*` and buffer (e.g. from a handle or diagnostic path). |
| **2**   | Transaction API  | `WriteTransactionGuard`, `ReadTransactionGuard`, `with_write_transaction`, `with_read_transaction`, `with_next_slot`. | **Default** for application code: RAII, exception-safe, single pattern. |

**MessageHub** is C++ only (no C API). It sits beside the DataBlock stack: broker connect, register_producer, discover_producer. Higher-level “top structures” (broker integration, Phase C tests) should use the **Transaction API (Layer 2)** or the C++ primitive (Layer 1) with clear lifetime discipline, not raw C API.

---

## 3. Current implementation vs design (revisions)

The C API has evolved (timeout metrics split, writer/reader timeout diagnostics, policy-based slot finding). The C++ layer must align with the current C API and header layout.

### 3.1 Slot RW

- **C API:** `slot_rw_acquire_write(rw_state, timeout_ms)` returns `SlotAcquireResult`; `slot_rw_acquire_read(rw_state, out_generation)`; `slot_rw_validate_read(rw_state, generation)`; metrics via `slot_rw_get_metrics` / `slot_rw_reset_metrics` with `SharedMemoryHeader*`. No header pointer in acquire/commit/release (slot-level only); header is used in the C++ implementation for write_index/commit_index and metrics.
- **C++ primitive:** Producer/Consumer own the slot selection (write_index, commit_index, policy-based next slot). They call internal helpers that use the same protocol as the C API (or the C API itself where it fits). Handles (`SlotWriteHandle`, `SlotConsumeHandle`) hold references to producer/consumer and slot index; they do not expose raw `SlotRWState*` in the public API but use it internally for write/read/commit.
- **SlotRWAccess (1.75):** Operates on **raw** `SlotRWState*` and `void*` buffer. Does **not** take `DataBlockProducer&` or `DataBlockConsumer&`. So today it is used when you already have rw_state + buffer (e.g. from a handle’s internal state or from diagnostic attach). It is **not** the main entry point for “write/read from a Producer/Consumer” — that is the Transaction API or the primitive acquire/release.

### 3.2 Recovery

- **C API:** All `datablock_*` functions take `const char* shm_name` and optional slot index/options. Return `int` (diagnose) or `RecoveryResult` (recovery ops). Documented in `recovery_api.hpp`.
- **C++ wrappers:** `SlotDiagnostics`, `SlotRecovery`, `IntegrityValidator`, `HeartbeatManager` wrap the C API or use `open_datablock_for_diagnostic` + header/slot access. They remain the preferred way to call recovery from C++; no need for a separate “Layer 2” for recovery beyond these wrappers.

### 3.3 Transaction API (Layer 2)

- **WriteTransactionGuard:** Holds `DataBlockProducer*` and optional `SlotWriteHandle`; acquire in ctor, release in dtor (noexcept); move-only. `slot()` returns `std::optional<std::reference_wrapper<SlotWriteHandle>>` (noexcept). `commit()` / `abort()` for explicit control.
- **ReadTransactionGuard:** Same idea for consumer and `SlotConsumeHandle`; `slot_id` and timeout in ctor.
- **with_write_transaction(producer, timeout_ms, func):** Builds guard, gets slot(); if !slot() throws; invokes func(slot); returns func result. Exception-safe: guard dtor releases.
- **with_read_transaction(consumer, slot_id, timeout_ms, func):** Same for read.
- **with_next_slot(iterator, timeout_ms, func):** Uses `DataBlockSlotIterator::try_next`; returns `std::optional` of lambda result on timeout; invokes func with the next slot handle; exception-safe.

These match the HEP Layer 2 intent. Gaps to close in design (and then code):
- **Exception-safety tests:** Ensure that if the user’s lambda throws, the guard always releases the slot and no exception escapes the guard destructor.
- **Usage guidance:** Document “prefer with_write_transaction / with_read_transaction and guards over manual acquire/release” in IMPLEMENTATION_GUIDANCE and/or HEP.

---

## 4. Consistent patterns to adopt

### 4.1 Single recommended path for “write one slot” / “read one slot”

- **Recommended:** `with_write_transaction(producer, timeout_ms, [&](SlotWriteHandle& slot) { slot.write(...); slot.commit(...); });` and `with_read_transaction(consumer, slot_id, timeout_ms, [&](const SlotConsumeHandle& slot) { ... });` or `with_next_slot(iterator, timeout_ms, [&](const SlotConsumeHandle& slot) { ... });`.
- **Alternative when exceptions are not acceptable:** Use `WriteTransactionGuard` / `ReadTransactionGuard` and check `if (!guard.slot()) { /* handle failure */ }` then use `guard.slot()->get()` and call `commit()` or `abort()` as needed. Destructors remain noexcept and release.

### 4.2 Typed access (Layer 1.75)

- **Current:** `SlotRWAccess::with_typed_write<T>(rw_state, buffer, buffer_size, func, timeout_ms)` and `with_typed_read<T>(rw_state, buffer, buffer_size, func, validate_generation)`. Require `SlotRWState*` and buffer — so they are ideal when you have a handle’s internal rw_state and buffer, or when building tooling/diagnostic code that has raw pointers.
- **Optional extension:** Provide Producer/Consumer-facing overloads, e.g. `with_typed_write<T>(DataBlockProducer& producer, int timeout_ms, Func&& func)` that acquires a write slot, obtains rw_state and buffer from the handle, and calls the existing `SlotRWAccess::with_typed_write<T>`-style logic (or inlines it). This would give a single “typed write/read” entry point for application code without exposing raw pointers. Design choice: add these as convenience on top of the existing Layer 2, so that “typed” is just a different lambda signature (T& vs SlotWriteHandle&).

### 4.3 Slot handle lifetime (reiterated)

- All slot handles must be released or destroyed **before** destroying the Producer or Consumer. Document in API comments and IMPLEMENTATION_GUIDANCE. Guards and with_* enforce this by construction.

### 4.4 Error reporting

- **C API:** Slot RW uses `SlotAcquireResult`; recovery uses `int` (diagnose) or `RecoveryResult`. No exceptions.
- **C++ primitive:** `acquire_write_slot` / `acquire_consume_slot` return `std::unique_ptr<...>` or similar; nullptr on failure. Caller checks.
- **Transaction API:** `with_*_transaction` throws on acquire failure (so caller can catch). Guard’s `slot()` returns std::nullopt on acquire failure (no throw from accessor). Mix is intentional: acquisition failure in with_* is exceptional for “happy path” code; guard allows non-throwing check when needed.

### 4.5 MessageHub and broker

- MessageHub remains C++. No C API. Broker integration (register_producer, discover_producer, then create/find producer/consumer) will use the **C++ factory** (`create_datablock_producer`, `find_datablock_consumer`) and then **Layer 2** (with_write_transaction, with_read_transaction) or Layer 1 with explicit handles for the actual write/read. Design all Phase C (broker) tests and top structures against this C++ layer.

### 4.6 Transaction context object: one argument that exposes slot + metrics, config, flex zone, validation

**Goal:** When using `with_write_transaction` / `with_read_transaction` / `with_next_slot`, the lambda receives a **single context object** (e.g. `WriteTransactionContext` / `ReadTransactionContext`) instead of only the slot handle. The context provides:

- **Slot (handle) access** – so the lambda still does write/read/commit via the handle.
- **Optional metrics view** – read-only snapshot or reference to current `DataBlockMetrics` (writer/reader timeouts, race counts, etc.) so the lambda can log, branch, or pass to monitoring without calling the C API.
- **Optional config view** – read-only view of effective `DataBlockConfig` (ring capacity, slot size, checksum, policy) for validation or display.
- **Flexible zone shorthand** – e.g. `ctx.flexible_zone(index)` returning the same span as `slot().flexible_zone_span(index)`, so the lambda can use one object for both buffer and flex zone.
- **Validation helpers (read path)** – e.g. `ctx.validate_read()` that returns whether the slot is still valid (generation not overwritten), or `ctx.slot_id()` / `ctx.generation()` for the lambda to record or check.

**Mechanism:** Define small, non-owning **context structs** (or classes with reference members) that are built inside the `with_*` implementation and passed by reference to the lambda. The lambda signature becomes `Func(WriteTransactionContext&)` or `Func(ReadTransactionContext&)` instead of `Func(SlotWriteHandle&)`. Existing overloads that take `Func(SlotWriteHandle&)` can be kept for backward compatibility or deprecated in favor of the single context API.

**Example (write):**

```cpp
struct WriteTransactionContext {
    SlotWriteHandle& slot();
    std::optional<std::reference_wrapper<const DataBlockMetrics>> metrics() const;  // optional
    std::optional<std::reference_wrapper<const DataBlockConfig>> config() const;   // optional
    std::span<std::byte> flexible_zone(size_t index = 0);  // forwards to slot().flexible_zone_span(index)
    uint64_t slot_id() const;
    size_t slot_index() const;
};

template <typename Func>
auto with_write_transaction(DataBlockProducer& producer, int timeout_ms, Func&& func)
    -> std::invoke_result_t<Func, WriteTransactionContext&>;
```

**Example (read):**

```cpp
struct ReadTransactionContext {
    const SlotConsumeHandle& slot() const;
    std::optional<std::reference_wrapper<const DataBlockMetrics>> metrics() const;
    std::optional<std::reference_wrapper<const DataBlockConfig>> config() const;
    std::span<const std::byte> flexible_zone(size_t index = 0) const;
    bool validate_read() const;   // generation still valid
    uint64_t slot_id() const;
    uint64_t generation() const; // if useful to expose
};
```

**Implementation notes:**

- Context holds **references** to the slot (and optionally to producer/consumer or their internal metrics/config). No ownership; lifetime is the scope of the `with_*` call. Context must not outlive the guard.
- **Metrics:** If Producer/Consumer expose `get_metrics(DataBlockMetrics&)` or a pointer to live metrics, the context can hold a pointer or reference to that (or a snapshot struct filled at context construction). Prefer a **const** view so the lambda does not modify shared metrics from inside the transaction.
- **Config:** Similarly, a const reference or optional pointer from Producer/Consumer::config().
- **ABI:** Context types should be non-virtual, layout-stable structs (or clearly documented as “opaque view” if we ever need to add fields without breaking ABI). Keep them in the same header as the transaction API.

**Overload strategy:**

- **Option A:** Replace the current lambda signature with the context: `with_write_transaction(producer, timeout_ms, [](WriteTransactionContext& ctx) { ... })`. Single, extensible API.
- **Option B:** Add overloads: `with_write_transaction(producer, timeout_ms, func)` where `func` is invoked with `(WriteTransactionContext&)` in the new overload, and keep the existing overload that invokes `func(SlotWriteHandle&)` for backward compatibility. Call sites can migrate to the context form when they need metrics/config/flex_zone.
- **Option C:** Always pass context; provide `ctx.slot()` so code that only needs the slot still has one clear access path. No need for two overloads.

**Recommendation:** Option C — always pass a context object; the context’s `slot()` (or `handle()`) gives access to the existing handle. Then the lambda can use `ctx.slot()`, `ctx.metrics()`, `ctx.config()`, `ctx.flexible_zone(i)` as needed. One signature, extensible without further API churn.

### 4.7 Single init-time access state (detail) so context does no repeated calculation

**Goal:** All offset calculation, initial positions, and derived values (layout, base pointers, per-slot and per–flex-zone offsets) are computed **once at initialization** and stored in a **single class or structure** in a `::detail` (or `::details`) namespace. The transaction context (and slot handles, metrics view, etc.) then **hold a reference or pointer to this state** and perform only trivial access (e.g. base + offset, or index into a precomputed array). No repeated layout math or multiple indirections through Producer → DataBlock → layout() in hot paths.

**Current state:** The codebase already has `DataBlockLayout` (in data_block.cpp) computed once from config or header and stored in `DataBlock` as `m_layout`. It also caches `m_slot_rw_states_array`, `m_flexible_data_zone`, `m_structured_data_buffer`, and `m_flexible_zone_info`. So layout and base pointers are not recomputed per access. What is still done per call in some paths is indirection (e.g. ProducerImpl → dataBlock → layout(), dataBlock → flexible_data_zone()) and per-slot arithmetic (e.g. `structured_data_buffer() + slot_index * unit_block_size`).

**Proposal:** Introduce an **access state** type (e.g. `detail::DataBlockAccessState` or `detail::BlockLayoutAndBases`) that is the single place for:

- **Layout:** Offsets and sizes (slot_rw_state, slot_checksum, flexible_zone, structured_buffer, total_size). Can be the existing `DataBlockLayout` or a copy.
- **Base pointers:** Either the segment base plus the layout (so base + offset is computed once per “segment attach”) or precomputed pointers for slot_rw_states, flexible_zone, structured_buffer, and header. Filled at init when the Producer/Consumer/DiagnosticHandle is created or attached.
- **Constants:** `ring_buffer_capacity`, `unit_block_size`, `slot_count`, so that “slot buffer for index i” is a single add (e.g. `structured_buffer_base + i * unit_block_size`) without loading from header each time.
- **Flexible zone table:** Precomputed per-zone offset/size (and optionally spinlock_index) in a contiguous array or vector, so `flexible_zone_span(i)` is `flexible_zone_base + zones[i].offset` and `zones[i].size`, with no map lookup or iteration.

This type lives in an implementation-detail namespace (e.g. `pylabhub::hub::detail` or in the .cpp with a forward-declared handle if we want to keep it out of public headers). It is **updated only during initialization** (creator path: from config; attacher path: from header + optional expected_config). ProducerImpl, ConsumerImpl, and DiagnosticHandleImpl would each hold (or reference) one such state. Slot handles and the transaction context would then receive a **const reference** (or pointer) to this state and the current slot index/handle, and implement:

- `slot_buffer(slot_index)` → state->structured_buffer_base() + slot_index * state->unit_block_size(), with size state->unit_block_size().
- `flexible_zone(i)` → state->flexible_zone_base() + state->zone_info(i).offset, size state->zone_info(i).size.
- `metrics()` → state->header() (or state->metrics_ptr()) for read-only metrics view.
- `config()` → state->config_snapshot() or state->ring_capacity() / state->unit_block_size() etc., if we store a snapshot or the relevant scalars.

**Benefits:**

- **No repeated calculation:** Layout and bases are computed once at init; context and handles only do indexing and one or two adds.
- **Single source of truth:** All offset and position logic lives in one detail type, so future layout changes (e.g. new header fields, different ordering) are done in one place.
- **Clear init boundary:** Constructor or factory builds the access state; everything else is “use the state.”

**Placement:**

- **Namespace:** e.g. `namespace pylabhub::hub::detail` or an anonymous namespace in the .cpp if the type is only used inside the TU. If the transaction context is in the public header and needs to hold a reference to this state, the type may need to be forward-declared or defined in a detail header (e.g. `data_block_detail.hpp` or in the same header under `namespace detail`).
- **Ownership:** ProducerImpl/ConsumerImpl own (or exclusively reference) the DataBlock, which already owns the layout and bases; the “access state” can be a thin wrapper that holds a pointer to DataBlock plus a snapshot of scalars (ring_capacity, unit_block_size) and a reference to the flexible_zone_info vector, or it can be a struct that is filled from DataBlock at init and stored in the Impl. Preference: one struct per Impl, filled once at construction, so that context and handles do not touch DataBlock on every access.

**Summary:** Have a single detail-level class/struct that is populated at initialization with all layout, base pointers, and initial positions/values. The transaction context (and slot access paths) then take a const reference to this state and perform only trivial indexing/pointer arithmetic, with no repeated layout calculation or multiple indirections.

### 4.8 Layout checksum protection and binding to the target; validation includes layout

**Goal:** The layout (the structure used for the access state: offsets, sizes, ring_buffer_capacity, unit_block_size, flexible_zone_size, etc.) should be **checksum protected** and **linked to the specific segment**. Validation of any object that uses that layout (Producer, Consumer, access state, or integrity check) must **include validation of the associated layout** (and its checksum).

**Rationale:**

- **Checksum protected:** So we can detect corruption or tampering of the layout-defining fields (e.g. in the header). If an attacker or bug changes ring_buffer_capacity or unit_block_size, we want to detect it when we build the access state or when we validate, rather than using wrong offsets.
- **Linked to the specific target:** The layout (and its checksum) must be stored in and validated against **this** segment’s header. We must not use a layout (or checksum) from another segment or from config without verifying it matches the segment we’re attached to. Optionally, the checksum input can include a segment identity (e.g. shm name hash or creation_timestamp_ns) so the layout is cryptographically bound to that segment.
- **Validation includes layout:** Whenever we validate the “object” (Producer, Consumer, access state, or the block in `datablock_validate_integrity`), we must validate the layout: recompute the layout from the header (or from the agreed config), recompute the layout checksum, and compare to the value stored in the segment’s header. If they differ, treat as integrity failure or incompatible attach.

**Mechanism:**

1. **Layout-defining fields:** Define a canonical set of header fields (and/or config-derived values) that uniquely determine the segment layout used by the access state: e.g. `ring_buffer_capacity`, `unit_block_size`, `flexible_zone_size`, `enable_checksum`, `policy`, `consumer_sync_policy`, and any other fields that affect DataBlockLayout. Optionally include a segment identity (e.g. `creation_timestamp_ns` or a hash of the shm name) so the checksum is bound to this segment.
2. **Store layout checksum at creation:** When the producer creates the segment, after writing the header (including these fields), compute a BLAKE2b checksum over a canonical serialization of these layout-defining values (and optional segment identity). Store the result in the segment’s header (e.g. in a reserved region such as `reserved_header` at a dedicated offset, similar to the existing header layout hash). Document the offset and size (e.g. 32 bytes).
3. **Validate layout when building access state:** When attaching (Consumer or DiagnosticHandle) or when building the access state from the header, after deriving the layout from the header: compute the same layout checksum from the current header fields (and optional segment identity); compare to the stored value. If they differ, do not use the layout—treat as corrupt or incompatible (return failure, throw, or refuse attach).
4. **Include layout in integrity validation:** In `datablock_validate_integrity` (and any higher-level “validate this block” API), include a step that verifies the layout checksum: recompute from the header and compare to the stored value. If the checksum fails, report an integrity failure (and optionally do not proceed with slot/checksum validation until layout is trusted).
5. **Object validation:** Any API that “validates” a Producer, Consumer, or the access state (e.g. “is this handle still valid?”, “validate integrity”) should perform layout validation as above: ensure the layout checksum in the segment’s header matches the value computed from the current layout-defining fields. So validation of the object implies validation of the associated layout object.

**Relationship to existing header layout hash:**

- The existing **header layout hash** (in `reserved_header`) protects the **ABI** of `SharedMemoryHeader` (field layout and types)—so producer and consumer share the same in-memory layout of the header struct. It does not protect the **values** of the fields that imply the segment layout (ring_buffer_capacity, unit_block_size, etc.).
- The **layout checksum** proposed here protects those **values**: a canonical fingerprint of the layout-defining fields (and optionally segment identity). So we have two checks: (1) header layout hash = same header struct ABI; (2) layout checksum = same segment layout (offsets/sizes) and bound to this segment. Both should be validated when attaching and when running integrity validation.

**Summary:** The layout used for the access state should be checksum protected (BLAKE2b of layout-defining fields, stored in the segment header) and linked to the specific target (stored in that segment; optionally include segment identity in the checksum input). Validation of any object that uses the layout (Producer/Consumer/access state / integrity) must include validation of the associated layout (recompute checksum, compare to stored value; on mismatch, treat as corrupt or incompatible).

### 4.9 Flexible zone: structure consistent with original claim; access only through provided layout

**Goal:** In the C++ abstraction, the flexible zone is **enforced** to have a structure (layout) that is **consistent with the original claim** (the producer’s config and the consumer’s agreed expected_config). The user is **forced to access the flexible zone only through the structure provided by the API** — i.e. by zone index, with offset/size coming from the agreed layout — and cannot bypass it (e.g. no raw pointer to the whole flexible region for arbitrary offset math).

**Enforced structure (original claim):**

- The producer creates the block with `DataBlockConfig::flexible_zone_configs` (per zone: name, size, spinlock_index). That defines the **claimed** flexible zone layout (number of zones, size and order of each).
- The consumer attaches with `expected_config`; the implementation validates that the header matches (e.g. `flexible_zone_size`, `ring_buffer_capacity`, `unit_block_size`, `enable_checksum`) and then builds the flexible zone table from `expected_config->flexible_zone_configs`. So both sides use the same layout. With §4.8, the layout (including flexible zone size and the implied per-zone layout) is also checksum-protected and validated on attach and integrity check.
- **Result:** The flexible zone layout (zone count, per-zone size, order) is consistent with the original claim and enforced by config agreement and (when implemented) layout checksum.

**Access only through provided structure:**

- The public API does **not** expose a raw pointer or span to the entire flexible region. It exposes **zone-by-index** access only: e.g. `ctx.flexible_zone(i)` or `slot().flexible_zone_span(i)`, which return a span for zone `i` with **offset and size from the precomputed zone table** (the agreed layout). So the “provided structure” is exactly that: the indexed zones (0 .. num_zones-1), each with its agreed offset and size.
- Callers cannot compute their own offsets into the flexible region; they must use the index-based API. Thus the user is forced to access the flexible zone only through the structure provided by the abstraction (zone index → span for that zone).
- **Contents of each zone:** The API currently returns `std::span<std::byte>` (or `std::span<const std::byte>`) per zone. So the **layout** (which zones exist, their sizes) is enforced and access is only via that layout; the **interpretation** of the bytes inside each zone (e.g. as a specific C++ type or schema) is left to the caller. An optional future extension could add **typed** access per zone (e.g. `with_typed_flexible_zone<T>(i, func)`) so that the contents of a zone are accessed only through a specific type, enforcing both layout and type; that would be an additional layer on top of the current “span per zone” API.

**Summary:** Yes — the flexible zone layout is enforced to be consistent with the original claim (producer config, consumer expected_config, layout checksum). The user can access the flexible zone only through the provided structure: zone-by-index API (e.g. `ctx.flexible_zone(i)`) with offset/size from the agreed layout, with no raw access that bypasses the zone table.

### 4.10 Memory model: single control surface for access and validation

**Goal:** The memory model and implementation follow a **single control surface**: one place for layout/structure, one place for access to that information, and one place for validation. No scattered layout, size, or validation logic.

**Structure (internal):**

- **DataBlockLayout** (implementation detail in `data_block.cpp`) is the **single source** for all layout and derived access:
  - Offsets and sizes: slot_rw_state, slot_checksum, flexible_zone, structured_buffer, total_size.
  - **logical_unit_size** (slot stride): the only value used for slot buffer pointer/size; set from config (creator) or header (attacher).
  - Populated once at init via `from_config()` or `from_header()`; all slot and region access uses `block->layout()` (e.g. `layout().slot_stride()`, `layout().slot_checksum_base()`) — no repeated `get_logical_unit_size(header)` at access sites that have a DataBlock.
- **Header** is the persistence and checksum input: layout-defining fields live in `SharedMemoryHeader`; `get_logical_unit_size(header)` is used only where no layout exists yet (e.g. building layout from header, store/validate_layout_checksum, config comparison during attach).

**Access to information:**

- **Application / handle code** does not touch layout math: it uses slot handles and transaction context (`ctx.slot()`, `ctx.flexible_zone(i)`), which are built from the internal layout and base pointers.
- **Internal code** that has a `DataBlock*` or `DataBlockLayout` uses **layout only** for slot stride and region offsets; code that only has a `SharedMemoryHeader*` (e.g. checksum, attach validation) uses header fields and `detail::get_logical_unit_size(header)`.

**Validation API — one logical place:**

- All **layout and segment validation** is exposed and grouped in one place in the public API (`data_block.hpp`):
  - **Header ABI:** `get_shared_memory_header_schema_info()`, `validate_header_layout_hash(header)` — same `SharedMemoryHeader` layout on both sides. The schema is the **canonical source for header field names**: the schema field list is defined next to the struct in data_block.hpp (PYLABHUB_SHARED_MEMORY_HEADER_SCHEMA_FIELDS) so field names, order, and types are correlated in one place; struct and schema stay consistent when adding or renaming fields.
  - **Segment layout (values):** `store_layout_checksum(header)` (at creation), `validate_layout_checksum(header)` — layout-defining field values match and are checksum-protected.
- **Attach flow** uses these in a single internal path: `validate_header_layout_hash(header)` then `validate_attach_layout_and_config(header, expected_config)` (which runs layout checksum + optional config match). Recovery/integrity uses the same layout checksum validation. So “what we validate” and “where we validate” are defined in one control surface (layout checksum + optional config), and the **public** entry points for layout/validation are the four functions above, documented together.

**Summary:** Layout is the single source for access (internal); header is for persistence and checksum input. All public layout/validation APIs live in one section of the header; attach and integrity use a single validation path (header hash + layout checksum + config). This keeps the memory model and validation framework unified and easy to reason about.

---

## 5. Options, parameters, and flexibility for higher-level code

To avoid higher-level code (applications, broker integration, tests, tooling, monitoring) from repeatedly dropping to the C API, the C++ abstraction should expose the following **options, parameters, and observability** so that one layer can satisfy their requirements without going back to the lower level.

### 5.1 Acquisition and error detail

| Need | Current gap | Recommendation |
|------|-------------|----------------|
| **Why did acquire fail?** | Primitive returns `nullptr`; guards return `std::nullopt`; with_* throws a generic message. Callers cannot distinguish timeout vs not-ready vs locked without parsing strings or calling C. | **Expose structured failure reason** at the C++ layer: e.g. an enum equivalent to `SlotAcquireResult` (or use `SlotAcquireResult` in a result type). Provide it from: (1) `acquire_write_slot` / `acquire_consume_slot` via an optional out-parameter or a `Result<Handle, AcquireError>`; (2) `WriteTransactionGuard` / `ReadTransactionGuard`: e.g. `acquire_result() const noexcept` returning the enum when `!slot()`; (3) `NextResult` (iterator): already has `error_code` (int) — **document** that it matches `SlotAcquireResult` (or use the enum type) so callers can switch on it. |
| **Timeout choice** | Already have `timeout_ms` on acquire, guards, with_*, iterator. | Keep; optional per-call override where it matters (e.g. MessageHub send timeout). |
| **Non-throwing acquire** | Guards allow `if (!guard.slot()) { ... }` without throw. | Keep; document as the pattern when exceptions are not acceptable. |

### 5.2 Metrics and observability

| Need | Current gap | Recommendation |
|------|-------------|----------------|
| **Slot/producer metrics** | C API has `slot_rw_get_metrics(header, &DataBlockMetrics)`. Producer/Consumer do not expose metrics; callers would need the raw header. | **Add `DataBlockProducer::get_metrics(DataBlockMetrics&)` and/or `DataBlockConsumer::get_metrics(DataBlockMetrics&)`** (or return a struct) that fills from the shared header. So monitoring, health checks, and tests can read writer_timeout_count, reader_race_detected, etc. without including `slot_rw_coordinator.h` or calling C. |
| **Reset metrics** | C API has `slot_rw_reset_metrics(header)`. | **Add `Producer::reset_metrics()` / `Consumer::reset_metrics()`** (or one side, since they share the same header) for tests and admin. |
| **Per-slot diagnostics** | Recovery C API: `datablock_diagnose_slot`. C++ has `SlotDiagnostics` (by shm_name + slot_index). | Keep C++ wrappers as the way to get diagnostics from higher-level code. Optionally: **Producer/Consumer::get_slot_diagnostic(slot_index)** that returns a `SlotDiagnostic` (or a C++ struct mirroring it) using the existing diagnostic handle or internal header/slot access, so callers do not need shm_name or recovery C API. |

### 5.3 Config, schema, and identity

| Need | Current gap | Recommendation |
|------|-------------|----------------|
| **Effective config** | Create/find take `DataBlockConfig`; no way to query “what config does this producer/consumer have?” after construction. | **Add `DataBlockProducer::config() const` and `DataBlockConsumer::config() const`** returning a read-only view or copy of the effective config (ring_buffer_capacity, unit_block_size, enable_checksum, consumer_sync_policy, etc.) so higher-level code can adapt (display, buffer sizing, policy checks) without re-attaching or calling C. |
| **Slot buffer size** | Handles expose `buffer_span()`; size comes from config. | Either expose **`Producer::slot_buffer_size() const`** / **`Consumer::slot_buffer_size() const`** (single slot size in bytes) or rely on `config().unit_block_size` / `config().structured_data_size()` once config() exists. |
| **Channel / shm name** | Used at create/find; not necessarily stored on Producer/Consumer. | **Add `Producer::name() const` and `Consumer::name() const`** (or channel_name/shm_name) so logging, recovery, and broker code can identify the block without passing names around separately. |
| **Schema info** | ConsumerInfo from discover_producer has schema_hash, schema_version. Create/find with template store/validate schema. | Already exposed in ConsumerInfo. Optionally: **Consumer::schema_hash() / schema_version()** (from header) for validation or display without re-discovery. |

### 5.4 MessageHub and broker

| Need | Current gap | Recommendation |
|------|-------------|----------------|
| **Connection state** | Internally `m_is_connected`; not exposed. | **Add `MessageHub::is_connected() const`** so callers can check before send/register/discover without relying on failure returns. |
| **Per-call timeout for broker** | `send_message` has `timeout_ms`; `register_producer` and `discover_producer` use default. | **Add optional `timeout_ms` to `register_producer` and `discover_producer`** (e.g. default 5000, overridable) so tests and high-latency environments can tune without touching C. |
| **Structured broker errors** | register_producer returns bool; discover_producer returns optional. Reason for failure is only in logs. | **Optional:** return or out-parameter for a small enum or string (e.g. timeout, parse_error, broker_error) so higher-level code can retry or branch without scraping logs. Lower priority if logging is sufficient. |

### 5.5 Recovery and diagnostics

| Need | Current gap | Recommendation |
|------|-------------|----------------|
| **Recovery options** | C API has `force`, slot_index, etc. C++ wrappers (SlotRecovery, IntegrityValidator) wrap them. | Ensure **C++ wrappers expose the same knobs** (e.g. force_reset(force), release_zombie_readers(force)). Consider a small **RecoveryOptions** struct (force, slot_index, etc.) for future extensibility without new function overloads. |
| **Integrity result** | datablock_validate_integrity returns RecoveryResult; IntegrityValidator returns it. | Keep; document RecoveryResult in one place (recovery_api.hpp) and reference from C++ wrappers. |

### 5.6 Buffers and typed access

| Need | Current gap | Recommendation |
|------|-------------|----------------|
| **Zero-copy / raw buffer** | Handles already have **buffer_span()** (std::span<std::byte> / const). | No change; document that higher-level code can use buffer_span() for custom serialization or zero-copy. |
| **Typed write/read from Producer/Consumer** | SlotRWAccess works on raw SlotRWState* + buffer. Application code must acquire handle, then get buffer from handle, then either use handle.write() or SlotRWAccess with raw pointers. | **Optional:** **Producer/Consumer-facing `with_typed_write<T>(producer, timeout_ms, func)` and `with_typed_read<T>(consumer, slot_id, timeout_ms, func)`** that acquire the slot, obtain buffer from the handle, and invoke func(T&) / func(const T&) with type and size checks. Reduces boilerplate and keeps “why did it fail” on the existing acquire path (optional acquire_result). |

### 5.7 Summary table: what to add so higher-level code stays off C API

| Area | Add / expose | Purpose |
|------|--------------|---------|
| Acquisition | Structured acquire failure (enum / SlotAcquireResult) from acquire_*, guards, NextResult | Branch on timeout vs not_ready vs locked without C API or string parsing. |
| Metrics | Producer/Consumer::get_metrics(), reset_metrics() | Monitoring, health, tests without slot_rw_get_metrics C API. |
| Diagnostics | Optional: Producer/Consumer::get_slot_diagnostic(slot_index) | Per-slot state without datablock_diagnose_slot C API. |
| Config | Producer/Consumer::config() const, name() const, slot_buffer_size() const | Display, buffer sizing, policy checks without re-open or C. |
| Schema | Optional: Consumer::schema_hash() / schema_version() from header | Validation/display without re-discovery. |
| MessageHub | is_connected(), optional timeout_ms for register_producer/discover_producer | Avoid “try and see” and tune timeouts. |
| Recovery | RecoveryOptions struct (optional); ensure wrappers expose force/slot_index | Extensibility and consistency. |
| Typed | Optional: with_typed_write<T>(producer, ...), with_typed_read<T>(consumer, ...) | One-step typed access without raw pointers. |
| **Transaction context** | **§4.6:** Pass `WriteTransactionContext` / `ReadTransactionContext` to the lambda instead of only the slot handle. Context exposes `slot()`, `metrics()`, `config()`, `flexible_zone(i)`, and (read) `validate_read()`. | Single object for lambda to access state, metrics, config, flex zone, validation without dropping to C API or extra parameters. |

Implementing these in the C++ layer keeps higher-level code on a single, stable abstraction and avoids frequent fallback to the C API.

---

## 6. Revisions from earlier C API changes (implementation alignment)

- **Memory model: single control surface (2026-02-12):** §4.10 documents the single control surface: DataBlockLayout as the single source for access (slot stride, offsets); all validation entry points (header hash, layout checksum, config match) grouped in one place in the public API; attach uses `validate_attach_layout_and_config`. Implementation: layout has `logical_unit_size`/`slot_stride()`; access uses `block->layout().slot_stride()`; `data_block.hpp` has a single “Memory model: layout and validation API” section.
- **Layout checksum and validation (2026-02-12):** The layout (access state) is checksum protected and linked to the specific segment (§4.8). At creation we store a layout checksum in the segment header; when attaching or validating we verify it. Validation of any object (Producer/Consumer/access state/integrity) must include validation of the associated layout. Implementation: reserved_header offset for layout checksum; store at creation; validate in attach (via validate_attach_layout_and_config) and in datablock_validate_integrity.
- **Writer timeout split:** Metrics and diagnostics now distinguish “timeout waiting for write_lock” vs “timeout waiting for readers to drain”. C++ layer does not need to change signature; it already uses the same internal protocol. Documentation and any logging in the C++ layer should refer to the two cases where relevant.
- **Policy-based slot finding:** Consumer and iterator use `get_next_slot_to_read(...)`. No change to the Transaction API surface; behavior is internal.
- **ConsumerSyncPolicy:** Config and header carry the policy; C++ API for create/find and acquire_consume_slot already take config/timeout. No new C++ abstraction needed for the policy itself.

---

## 7. Summary: what to implement / document next

1. **Documentation:** Add “C++ abstraction layers” summary and “Recommended usage” (prefer Layer 2, handle lifetime, when to use C API) to IMPLEMENTATION_GUIDANCE. Reference this document for the full layer map and patterns.
2. **Options and flexibility (§5), transaction context (§4.6), init-time access state (§4.7), and layout checksum/validation (§4.8):** Implement the **transaction context object** (§4.6) so with_* lambdas receive `WriteTransactionContext` / `ReadTransactionContext` with `ctx.slot()`, `ctx.metrics()`, `ctx.config()`, `ctx.flexible_zone(i)`, and (read) `ctx.validate_read()`. Implement a **single detail-level access state** (§4.7): one class/struct (e.g. in `::detail`) populated at initialization with all layout, base pointers, and initial positions/values, so that context and slot access do no repeated offset calculation—they only hold a const reference to this state and do trivial indexing/pointer arithmetic. Implement **layout checksum protection and validation** (§4.8): at creation store a BLAKE2b checksum of layout-defining header fields (and optional segment identity) in the segment header; when building access state or running integrity validation, recompute and compare—validation of any object (Producer/Consumer/access state/integrity) must include validation of the associated layout. **Flexible zone** (§4.9): structure (zone count, sizes, order) is enforced to match the original claim (config/expected_config and layout checksum); access is only via zone-by-index API (e.g. `ctx.flexible_zone(i)`), with no raw access that bypasses the zone table. Then implement the rest of §5 (structured acquire failure, get_metrics/reset_metrics, config()/name()/slot_buffer_size(), MessageHub is_connected(), optional timeout, optional typed with_typed_*). Prioritize by DATAHUB_TODO and Phase C needs.
3. **Exception-safety:** Add tests that with_write_transaction / with_read_transaction and guards always release the slot when the user lambda throws; no exception from guard destructors.
4. **Optional:** Producer/Consumer-facing `with_typed_write<T>` / `with_typed_read<T>` overloads that acquire a slot and run the typed lambda (so application code can use T& without touching raw rw_state/buffer). Defer if the current “use SlotWriteHandle / SlotConsumeHandle and copy to T” is sufficient.
5. **Broker and top structures:** Design and implement Phase C (MessageHub + broker) and any further “top” layers using Layer 2 (and Layer 1 where needed) only; do not add new code paths that bypass the C++ abstraction unless justified by performance or flexibility.

---

## 8. References

- `docs/DATAHUB_TODO.md` – Next steps, Phase 2.2/2.3, design strategy.
- `docs/IMPLEMENTATION_GUIDANCE.md` – Patterns, ABI, error handling, testing.
- `docs/DATAHUB_DATABLOCK_CRITICAL_REVIEW.md` – §7 C API review; §2 API design.
- `docs/hep/HEP-CORE-0002-DataHub-FINAL.md` – Layer 1.75 and Layer 2 intent (§5.3, §5.4).
- `docs/DOC_STRUCTURE.md` – Where to merge this design (topic-based master docs).
