# DataHub Memory Layout and Structure Re-Mapping Design

| Property       | Value                                                |
| -------------- | ---------------------------------------------------- |
| **Document**   | DATAHUB_MEMORY_LAYOUT_AND_REMAPPING_DESIGN            |
| **Status**     | Design Proposal                                      |
| **Created**    | 2026-02-14                                           |
| **Dependencies** | HEP-CORE-0002, IMPLEMENTATION_GUIDANCE             |
| **Implementation** | Deferred until broker implementation is correct  |

---

## 1. Purpose

This document captures the design for:

1. **Simplified memory layout** — Single flex zone, compact control region, unified data region (flex zone + ring-buffer).
2. **Structure re-mapping** — Broker-controlled context update for both flex zone and ring-buffer units, allowing schema/structure changes without memory reallocation.
3. **Implementation deferral** — Re-mapping is not implemented until broker orchestration is correct; no one-sided structure changes without central control.

**Relationship to core docs:** After implementation is complete, this design will be merged into `docs/HEP/HEP-CORE-0002-DataHub-FINAL.md` and `docs/IMPLEMENTATION_GUIDANCE.md`. Until then, this document is the authoritative reference for the new layout and re-mapping protocol.

### 1.1 Memory Structure Change Only — No Mechanism Change

This design is a **memory layout/structure change only**. All control and functional mechanisms remain logically the same:

- **Control fields** — SlotRWState, SlotChecksum, metrics, heartbeats, spinlock states, schema_hash, schema_version, etc. are unchanged in meaning and usage.
- **Coordination logic** — Slot acquire/release, commit, reader/writer coordination, TOCTTOU mitigation, generation counters remain as designed.
- **Checksum logic** — Layout checksum, per-slot checksum, flex zone checksum — same algorithms and semantics.
- **Recovery/diagnostics** — Same APIs and behavior.

Only the **placement** (offsets, region order, flex zone count) and **sizing** (single flex zone N×4K, 4K-aligned data region) change. No new coordination primitives or functional protocols are introduced.

---

## 2. Memory Layout

### 2.1 Overview

```
┌─────────────────────────────────────────────────────────────────┐
│ Global Header (4K or 8K)                                         │
│  - Magic, version, schema_hash, schema_version                   │
│  - Ring buffer config and state (policy, capacity, etc.)         │
│  - Metrics, consumer heartbeats, spinlock states                 │
│  - Fingerprints: flex zone schema, ring-buffer slot schema       │
└─────────────────────────────────────────────────────────────────┘
┌─────────────────────────────────────────────────────────────────┐
│ Control/Stats Zone (compacted, padded to 4K boundary)            │
│  - SlotRWState array (48 bytes × N slots)                        │
│  - SlotChecksum array (33 bytes × N slots)                       │
└─────────────────────────────────────────────────────────────────┘
┌─────────────────────────────────────────────────────────────────┐
│ DATA Region (4K-aligned start)                                   │
│  - Flex zone: single section, size = N×4K (4K, 8K, 12K, …)       │
│  - Ring-buffer: M segments of logical_unit_size (e.g. 16K each)  │
└─────────────────────────────────────────────────────────────────┘
```

### 2.2 Design Principles

| Principle              | Description                                                                 |
| ---------------------- | --------------------------------------------------------------------------- |
| **Single flex zone**   | Exactly one flex zone (no multiple sub-zones). Simplifies layout and config. |
| **Flex zone size**     | Multiple of 4K (page size). Can differ from ring-buffer logical unit size.   |
| **Ring-buffer unit**   | Per-slot size = `logical_unit_size` (e.g. 16K), multiple of page size.       |
| **4K alignment**       | Data region start is 4K-aligned. All unit boundaries align with page size.   |
| **Compact control**    | SlotRWState and SlotChecksum arrays are compact; padded to 4K boundary.      |

### 2.3 Layout Details

| Region           | Size / Formula                               | Alignment     |
| ---------------- | --------------------------------------------- | ------------- |
| **Global Header**| 4K or 8K (reserved)                           | 4K            |
| **Control Zone** | `slot_count × (48 + 33)` bytes, padded to 4K  | 4K            |
| **Flex Zone**    | `flex_zone_size` = N×4096 (N ≥ 1)             | 4K            |
| **Ring-buffer**  | `ring_buffer_capacity × logical_unit_size`    | page_size     |

**Offsets:**

- `control_offset = header_size` (4K or 8K)
- `data_region_offset = align_up(control_offset + control_size, 4096)`
- `flex_zone_offset = data_region_offset`
- `ring_buffer_offset = data_region_offset + flex_zone_size`
- `slot_i_offset = ring_buffer_offset + i × logical_unit_size`

### 2.4 Size Constraints

- **Page size:** 4K, 4M, or 16M (DataBlockPageSize).
- **Flex zone size:** Must be a multiple of 4096.
- **Logical unit size:** Must be ≥ page size and a multiple of page size.
- **Total size:** Computed at creation; fixed for the lifetime of the block.

### 2.5 Cache-Line Alignment (Guaranteed for Atomic Operations)

Structures used for atomic operations **must** be cache-line aligned to avoid false sharing and to ensure correct memory ordering on architectures with non-uniform cache behavior. This design **guarantees** cache-line alignment for all control structures that contain atomics.

**Cache-line size assumption:** 64 bytes (typical on x86-64, ARM64, and most modern architectures).

| Structure                     | Alignment / Size           | Guarantee |
| ----------------------------- | -------------------------- | --------- |
| **SlotRWState**               | `alignas(64)`, 48 bytes    | Type alignment ≥ 64. Array base must be 64-byte aligned so first element is cache-line isolated. |
| **SlotRWState array base**    | 64-byte aligned            | Control zone starts at `header_size`; header is 4K-aligned, so control zone base is 64-byte aligned. |
| **ConsumerHeartbeat**         | 64 bytes total             | Each entry 64 bytes (consumer_id, last_heartbeat_ns, padding[48]); cache-line isolated. |
| **FlexibleZoneChecksumEntry** | 64 bytes total             | Each entry 64 bytes; cache-line isolated. |
| **SharedSpinLockState**       | 32 bytes each              | In header; spinlock_states[] packed. Header is 4K-aligned. |
| **Metrics sections**          | Grouped in header          | Header layout groups atomics to minimize false sharing. |

**Layout rules (must be preserved in any layout change):**

1. **SlotRWState array base** — The start of the SlotRWState array must be 64-byte aligned. Current: `slot_rw_state_offset = sizeof(SharedMemoryHeader)` (4096), which is a multiple of 64. When compacting or changing the control zone, the SlotRWState array base offset must remain a multiple of 64.
2. **Header alignment** — `SharedMemoryHeader` is `alignas(4096)`; the segment base is page-aligned from the OS. All region boundaries (header end, control zone, data region) must be multiples of 64 (and 4K for data region).
3. **SlotRWState stride** — Current: `sizeof(SlotRWState) == 48`; elements are packed. The first SlotRWState is 64-byte aligned; subsequent elements may share cache lines. For stronger isolation, a 64-byte stride (padding) could be used; this would be an optional refinement, not required for correctness.
4. **Data region** — Flex zone and ring-buffer payload do not require cache-line alignment for system atomics. User-defined atomics within flex zone are the application's responsibility.

**Verification:** `static_assert(alignof(SlotRWState) >= 64)` and `static_assert(raw_size_SlotRWState == 48)` in `data_block.hpp` must remain. Any new layout calculation must ensure:
- Control zone base (SlotRWState array start) is 64-byte aligned.
- Header size and data region offset are multiples of 64 (data region also 4K for page alignment).

---

## 3. Structure Re-Mapping Protocol

### 3.1 Scope

Re-mapping allows updating the **structure/schema** of:

1. **Flex zone** — User-defined layout within the flex zone (e.g. different struct layout).
2. **Ring-buffer units** — User-defined layout within each slot (e.g. different `T` for `with_typed_write<T>`).

### 3.2 Invariants (Safety)

| Invariant                 | Description                                                                 |
| ------------------------- | --------------------------------------------------------------------------- |
| **No size change**        | Re-mapping does **not** change flex zone size, ring-buffer capacity, or logical unit size. |
| **Structure fits**        | New structure (schema) must be verified to fit within current memory layout. |
| **Broker control**        | Re-mapping is only allowed under broker orchestration; no one-sided changes. |

### 3.3 Protocol (Broker-Orchestrated)

1. **Producer requests re-map** — Producer sends request to broker with new structure (schema fingerprint).
2. **Broker validates** — Broker verifies new structure fits current memory (size constraints).
3. **Broker coordinates** — Broker signals consumers to quit and wait.
4. **Consumers disconnect** — Consumers release handles and disconnect from the DataBlock.
5. **Producer performs re-map** — Producer updates schema_hash, schema_version, and any structure-related fingerprints in the header.
6. **Broker allows reconnection** — Broker signals that the new structure is active.
7. **Consumers reconnect** — Consumers reconnect with the new structure; their request must include the new schema/fingerprint.

### 3.4 Context Update

- **Schema hash** — BLAKE2b hash of the new structure (flex zone or ring-buffer slot).
- **Schema version** — Version number for the new structure.
- **Checksum** — Layout checksum and per-slot/flex checksums are recalculated as needed after re-map.

The old context is invalidated; the new context is active after the producer updates fingerprints. Consumers must not use old schema after re-map.

### 3.5 Implementation Deferral

- Re-mapping is **not implemented** until broker implementation is correct.
- No one-sided structure change without central control.
- This operation cannot be done within the same context; context is updated with new fingerprints.

---

## 4. Examination Against Existing Code

### 4.1 Current Layout (data_block.cpp)

**Current order:** Header | SlotRWState | SlotChecksum | FlexibleZone | StructuredBuffer

**Differences from proposed layout:**

| Aspect              | Current                          | Proposed                               |
| ------------------- | -------------------------------- | -------------------------------------- |
| Flex zone count     | Multiple (FlexibleZoneConfig[])  | Single flex zone                       |
| Flex zone size      | Sum of config sizes              | N×4K (multiple of 4K)                  |
| Structured buffer   | After flex zone                  | After flex zone (same)                 |
| Data alignment      | 8-byte (structured_buffer_offset)| 4K-aligned data region                 |
| Control zone        | Packed after header              | Compact + padded to 4K                 |

### 4.2 Code Impact

| Component                    | Current Behavior                          | Change Required                                    |
| ---------------------------- | ------------------------------------------ | -------------------------------------------------- |
| `DataBlockConfig`            | `flexible_zone_configs` (vector)           | Replace with single `flex_zone_size` (N×4K)        |
| `FlexibleZoneInfo`           | Per-zone offset, size, spinlock_index      | Single zone: offset=0, size=flex_zone_size         |
| `DataBlockLayout::from_config` | Computes flexible_zone_size from configs  | Use single flex_zone_size (N×4K)                   |
| `DataBlockLayout::from_header` | Reads flexible_zone_size from header      | Same; header stores flex_zone_size                 |
| `flexible_zone_span(index)`  | Index-based access to multiple zones       | Remove index or use 0 only (single zone)           |
| `SharedMemoryHeader`         | flexible_zone_size                         | Ensure stored as N×4096                            |
| Schema/fingerprint           | schema_hash, schema_version in header      | Add flex/slot structure fingerprints if needed     |

### 4.3 Compatibility

- **Producer/Consumer API** — `flexible_zone_span()`, `flexible_zone<T>(index)` would change: single zone implies `index` always 0 or removed.
- **Recovery/Integrity** — Layout checksum, slot checksum, flex zone checksum logic remains; layout formula changes.
- **MessageHub/Broker** — Discovery and schema_hash/schema_version exchange exist; re-mapping protocol is additive.

### 4.4 Migration Path

1. Introduce new config fields (`flex_zone_size` as N×4K) alongside `flexible_zone_configs`.
2. Add layout version or flag to distinguish old (multi-zone) vs new (single zone) layout.
3. Deprecate `flexible_zone_configs` for new creation; support both during transition.
4. Implement re-mapping protocol once broker supports coordination.

### 4.5 Detailed Code Inventory

**Files and functions that reference flexible zones (for migration):**

| File | Location | Current Usage | Proposed Change |
|------|----------|---------------|-----------------|
| `data_block.hpp` | `FlexibleZoneConfig` | Per-zone name, size, spinlock_index | Replace with single `flex_zone_size` (N×4K) |
| `data_block.hpp` | `FlexibleZoneInfo` | offset, size, spinlock_index per zone | Single zone: offset=0, size=flex_zone_size |
| `data_block.hpp` | `DataBlockConfig::flexible_zone_configs` | Vector of configs | Replace with `flex_zone_size` |
| `data_block.hpp` | `DataBlockConfig::total_flexible_zone_size()` | Sum of config sizes | Return flex_zone_size directly |
| `data_block.hpp` | `SharedMemoryHeader::flexible_zone_checksums` | Per-zone checksums (8 entries) | Single zone → 1 entry or simplify |
| `data_block.hpp` | `flexible_zone_span(index)`, `flexible_zone<T>(index)` | Index-based multi-zone | index=0 only or remove |
| `data_block.cpp` | `build_flexible_zone_info()` | Builds vector from configs | Return single FlexibleZoneInfo |
| `data_block.cpp` | `DataBlockLayout::from_config` | Uses total_flexible_zone_size() | Use flex_zone_size (N×4K) |
| `data_block.cpp` | `DataBlockImpl::m_flexible_zone_info` | Map name → FlexibleZoneInfo | Single entry or remove map |
| `data_block.cpp` | `DataBlockImpl::set_flexible_zone_info_for_attach` | Populates from configs | Populate single zone |
| `data_block.cpp` | `update_checksum_flexible_zone_impl` | Iterates by flexible_zone_idx | Single zone (idx=0) |
| `data_block.cpp` | `verify_checksum_flexible_zone_impl` | Iterates by flexible_zone_idx | Single zone (idx=0) |
| `data_block.cpp` | `SlotWriteHandleImpl::flexible_zones_info` | Vector from config | Single zone |
| `data_block.cpp` | `SlotConsumeHandleImpl::flexible_zones_info` | Vector from config | Single zone |
| `data_block.cpp` | `validate_attach_layout_and_config` | Compares total_flexible_zone_size | Compare flex_zone_size |
| `data_block.cpp` | `LayoutChecksumInput` / layout hash | Includes flexible_zone_size | Same; ensure N×4K |
| `data_block_recovery.cpp` | Integrity validator | Loops flexible_zone_configs | Single zone loop (1 iteration) |
| `IMPLEMENTATION_GUIDANCE.md` | Dual-chain, flexible zone | References multi-zone | Update to single zone |
| Tests | `LayoutWithChecksumAndFlexibleZoneSucceeds`, `flexible_zone_span` | Multi-zone configs | Update to single zone |

**Ring-buffer re-mapping (schema/structure change only, no size change):**

- `schema_hash`, `schema_version` in SharedMemoryHeader — already present; broker updates on re-map.
- `generate_schema_info`, `validate_schema_match` — used for attach validation; re-map flow would update schema then allow reconnection with new schema.
- No ring-buffer-specific structure fingerprint today; schema_hash covers header layout. Per-slot structure (e.g. `T` in `with_typed_write<T>`) is application-defined; re-map would update schema_hash to reflect new slot layout.

---

## 5. References

- **HEP-CORE-0002** — Data Exchange Hub specification
- **IMPLEMENTATION_GUIDANCE** — Architecture, layout, alignment, facility access
- **DATAHUB_TODO** — Execution plan and priorities
- **DOC_STRUCTURE** — Documentation layout and merge policy

---

## 6. Revision History

| Version | Date       | Change                                              |
| ------- | ---------- | --------------------------------------------------- |
| 0.1     | 2026-02-14 | Initial design: layout, re-mapping protocol, code examination |
| 0.2     | 2026-02-14 | §1.1 Memory structure change only (no mechanism change); §2.5 Cache-line alignment guaranteed and documented |
