# DataBlock: Explicit Policy, Compact Schema, and Mapping Cache — Rational Analysis

## 1. Making "mode" (policy) explicitly required

**Current state:** `DataBlockConfig` has defaults:
- `policy = DataBlockPolicy::RingBuffer`
- `consumer_sync_policy = ConsumerSyncPolicy::Latest_only`

**Problem:** Callers can omit these and get semantics they didn’t intend (e.g. test expected FIFO but got latest-only). Defaults hide an important choice.

**Options:**

| Approach | Pros | Cons |
|----------|------|------|
| **A. Remove defaults** (no `= RingBuffer` / `= Latest_only`) | Forces every caller to set; no silent wrong mode. | All existing call sites must be updated. |
| **B. Sentinel "Unset"** (e.g. `DataBlockPolicy::Unset`) | Can keep defaults for backward compat; strict API can require != Unset. | Two ways to express “not set”; more enum values. |
| **C. `std::optional`** for policy fields | “Not set” is explicit; factory can fail if nullopt. | API change; optional in config struct. |
| **D. Separate “strict” factory** (e.g. `create_datablock_producer_explicit`) | Backward compatible; new code can use strict API. | Two APIs to maintain. |

**Recommendation:** **A or C.** Prefer **A** (no defaults) for simplicity: require `config.policy` and `config.consumer_sync_policy` to be set at every call site.

**Implemented:** Defaults were removed. `DataBlockConfig` now uses sentinel values `DataBlockPolicy::Unset` and `ConsumerSyncPolicy::Unset` (value 255; never stored in the header). Stored policy values remain 0/1/2 for backward compatibility. The DataBlock creator constructor validates and throws `std::invalid_argument` if either is `Unset`. All tests and the example were updated to set `config.policy` and `config.consumer_sync_policy` explicitly.

**Failure behavior:** When either `config.policy` or `config.consumer_sync_policy` is `Unset`, **producer creation fails explicitly**: the `DataBlock` constructor (creator path) throws `std::invalid_argument` before any shared memory is created. Consumer attach does not take a “mode” from config for creation (the header was written by the producer); the consumer only validates that the header matches `expected_config` when one is provided. So the “must set” check applies at **producer creation** only.

### Other parameters: should they “fail if not set”?

| Parameter | Current default | Layout / semantics impact | Recommendation |
|-----------|------------------|---------------------------|----------------|
| **policy** | Unset (required) | Defines buffer strategy (Single/Double/Ring). | ✓ **Fail if unset** (implemented). |
| **consumer_sync_policy** | Unset (required) | Defines read order and blocking. | ✓ **Fail if unset** (implemented). |
| **physical_page_size** | Unset (required) | Allocation granularity; affects segment size and layout. | Layout-critical; wrong value → corruption. ✓ **Fail if unset** (implemented). |
| **ring_buffer_capacity** | 0 = unset (required ≥ 1) | Slot count (1/2/N); affects layout and semantics. | Layout-critical; wrong value → wrong slot index → corruption. ✓ **Fail if unset** (implemented). |
| **logical_unit_size** | `0` | 0 means “use physical”; resolved at create. | **Keep default.** 0 is a defined sentinel; no need to require. |
| **shared_secret** | `0` | 0 = discovery/generate; non-zero = capability. | **Keep default.** 0 has clear meaning. |
| **enable_checksum** | `false` | Affects layout (checksum region size). | **Keep default.** false is a safe default. |
| **checksum_policy** | `Enforced` | When to update/verify checksums. | **Keep default.** Less critical than policy; default is reasonable. |
| **flexible_zone_configs** | empty | Flexible zone size/layout. | **Keep default.** Empty = no flexible zone. |

**Summary:** **policy**, **consumer_sync_policy**, **physical_page_size**, and **ring_buffer_capacity** all **fail at producer create** if not set (to avoid memory corruption and sync bugs). Consumer attach validates layout when expected_config is provided. The rest keep defaults or sentinel semantics.

---

## 2. Helper for schema / compact view (alignment-independent)

**What we have today:**
- **Header layout hash:** Built from a *descriptor* (field names + types), not from raw struct memory. So it’s already alignment-independent.
- **Layout checksum:** We build a small blob with `append_le_*` and hash it; that’s our “compact view” for layout-defining fields.
- **`deterministic_checksum.hpp`:** Primitives only (`append_le_u32`, `append_le_u64`, `append_u8`).

**What “schema for a structure without alignment impact” can mean:**

- **Schema descriptor (already alignment-independent):** `PYLABHUB_SCHEMA_*` + `SchemaInfo` describe *logical* layout (name + type). The hash is over that descriptor, not over struct bytes. No change needed for alignment.
- **Serializing a struct instance to a stable blob:** For a given C++ struct, we want the same logical values to always produce the same bytes (e.g. for hashing or wire format). That’s the same pattern as the layout checksum: use a fixed order and fixed types (LE u32/u64/u8, etc.) and never hash raw struct memory.

**Helper idea:** A macro or small DSL that, for a list of (member_name, type_kind), generates code that appends those members in order using `append_le_*` / `append_u8`. Example:

```cpp
// Conceptual: generate serialization for a "view" of a struct
PYLABHUB_SERIALIZE_VIEW(MyStruct, buf, off,
    (ring_buffer_capacity, u32),
    (physical_page_size, u32),
    (logical_unit_size, u32),
    (flexible_zone_size, u64),
    (enable_checksum, u8),
    (policy, u8)
);
```

So the “compact memory view” is defined by the macro’s (name, type) list; no struct layout or padding is used. Useful for:
- Any new “layout checksum”-like blob.
- Future schema-based payload serialization (struct → blob when writing, blob → struct when reading).

**Recommendation:** Yes, a small helper is useful. Options:
- **Minimal:** Document the pattern (sequence of `append_*` calls) and keep one reference implementation (e.g. layout_checksum_fill). No macro.
- **Macro:** Add something like `PYLABHUB_APPEND_VIEW(Struct, buf, off, (field, type)...)` that expands to the right append calls. Reduces duplication and keeps one place that defines the view.

We should not auto-derive from the struct (no reflection); the view is explicitly listed so alignment/layout never sneaks in.

---

## 3. Saving “compact” view in a zone

**“Zone”** = shared memory regions (header, flexible zone, structured buffer, etc.).

**What could be stored:**
- The **compact blob** itself (e.g. the 24-byte layout checksum *input*). We don’t need to: we only need its *hash*, and we already store that in `reserved_header`. Recomputing the 24-byte blob on attach is cheap and rare.
- A **schema descriptor** (field names + types + maybe offsets in the compact format). Today we store schema *hash* + version in the header; the full descriptor is not in shared memory. Storing the full descriptor would only help if we had a *generic* reader that doesn’t share the same C++ struct and needs to interpret slots from the descriptor. We don’t have that yet.

**Conclusion:** For current use (layout checksum, existing schema hash), we don’t need to store the compact view or descriptor in a zone. We already store the minimal info (layout-defining header fields, schema hash). Recomputing the small blob or layout from the header is done once per attach and is cheap.

---

## 4. How often is “mapping” done at runtime?

**Today:**
- **Layout:** `DataBlockLayout::from_header()` is run **once per process** when the DataBlock is created or attached. Not per slot.
- **Layout checksum:** The 24-byte blob is built and hashed only when **validating** (consumer attach, or integrity/recovery). Infrequent.
- **Schema:** Schema hash is compared on consumer attach (once). No per-slot “mapping.”
- **Slot read/write:** Pointer = `structured_data_buffer() + slot_index * slot_stride_bytes`. No extra “mapping” step; just arithmetic.

So there is **no repeated “mapping” in the hot path**. Any “view” or “layout” is derived once per block (or once per validation).

**If we added** compact payload serialization (struct ↔ blob using a fixed view):
- **Write:** One serialization per slot write (producer).
- **Read:** One deserialization per slot read (consumer).
- The “mapping” (which field is at which offset in the blob) could be computed **once per process** from the schema/descriptor and cached in process memory. No need to store it in shared memory for performance.

**Conclusion:** Mapping is not a high-frequency operation today. If we add schema-based serialization, the natural place to cache the mapping is process-local (e.g. per SchemaInfo or per type), not in the control zone.

---

## 5. Reserving a region in the control zone for mapping cache?

**What we already do:** We store layout-*defining* values in the header (physical_page_size, logical_unit_size, ring_buffer_capacity, etc.) and **derive** everything else (e.g. `DataBlockLayout`) from them. We don’t store the derived layout in shared memory; each side computes it once. We do store the **layout checksum** (hash of the defining blob) in `reserved_header` for validation.

**When would a “mapping cache” region help?**
- If **multiple processes** needed the same derived info (e.g. a compact descriptor or offset table) and we wanted to compute it once and share it. Example: a generic reader that reads a “schema descriptor” from the block and builds an offset table; if the descriptor were stored in a reserved header region, every reader could use it without recomputing from a schema library. That’s a **future** feature (generic/heterogeneous readers).
- If we had a **very expensive** derivation and did it often. We don’t; derivation is cheap and rare.

**Recommendation:** **Do not** add a reserved region for “mapping cache” with the current design. Reasons:
1. No hot-path mapping; layout is derived once per process.
2. Layout checksum input is recomputed only on validation; storing the hash is enough.
3. A reserved region for “schema descriptor” or “offset table” only becomes useful if we introduce generic readers that interpret slots from a descriptor stored in the block. We can add that later without changing the current layout.

If we ever add that feature, we can reserve a small area (e.g. in `reserved_header`) for “schema descriptor blob” and document it; until then, keeping the control zone as-is avoids complexity and leaves room for future use.

---

## Summary

| Question | Conclusion |
|----------|------------|
| **(1) Explicit mode** | Make policy and consumer_sync_policy explicitly required (no defaults). Update call sites; optionally add a temporary run-time check. |
| **(2) Helper for compact view** | Yes: keep using the append_* pattern; add a macro or documented pattern so “view” is a fixed (name, type) list and never raw struct memory. |
| **(3) Save compact view in a zone** | Not needed now. We store the layout checksum *hash*; recomputing the small input blob on attach is cheap and rare. |
| **(4) Mapping frequency** | Mapping is not done repeatedly; layout/schema are used once per block or per validation. |
| **(5) Reserve region for mapping cache** | No. Only consider a “schema descriptor” region if we add generic readers that interpret slots from a descriptor in shared memory. |
