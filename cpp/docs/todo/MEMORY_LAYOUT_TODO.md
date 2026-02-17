# Memory Layout TODO

**Purpose:** Track memory layout redesign tasks, including single flex zone implementation, alignment fixes, and validation improvements.

**Master TODO:** `docs/TODO_MASTER.md`  
**Design Document:** `docs/DATAHUB_MEMORY_LAYOUT_AND_REMAPPING_DESIGN.md` (active)  
**HEP Reference:** `docs/HEP/HEP-CORE-0002-DataHub-FINAL.md` §3

---

## Current Sprint

### Single Memory Structure ✅
**Status**: Complete

- [x] Remove layout version switch
- [x] Remove `flexible_zone_configs` array (use single `flex_zone_size`)
- [x] Remove `FlexibleZoneConfig` struct
- [x] Single flex zone at index 0 only
- [x] Update `DataBlockLayout::from_config` and `from_header`
- [x] Remove legacy paths

### Structured Buffer Alignment
**Status**: 🟡 In Progress

**Problem**: Structured data region was not aligned, causing misalignment for types with `alignof(T) == 8` (e.g., `uint64_t`).

Tasks:
- [ ] Implement 8-byte alignment for structured buffer start
- [ ] Update `DataBlockLayout::from_config` with alignment padding
- [ ] Update `DataBlockLayout::from_header` to match
- [ ] Verify slot pointers satisfy `alignof(T)` requirements
- [ ] Test with various struct types (uint64_t, double, custom structs)
- [ ] Document alignment guarantees in API

**Implementation Notes:**
- Formula: `structured_buffer_offset = align_up(after_flexible, 8)`
- Add 0-7 bytes padding before structured region
- No change to usable space: `structured_buffer_size = slot_count * slot_stride_bytes`
- Old packed layouts (no padding) are incompatible

### Layout Validation and Checksum
**Status**: 🟢 Ready

- [ ] Update layout checksum calculation to include alignment padding
- [ ] Verify integrity validator handles new layout
- [ ] Test attach with mismatched layout (should fail)
- [ ] Document layout version compatibility

---

## Backlog

### Memory Layout Enhancements
- [ ] **Compact control region** – Optimize SharedMemoryHeader layout for cache efficiency
- [ ] **Flexible zone grow/shrink** – Support runtime flex zone size changes (requires broker coordination)
- [ ] **Multiple flex zones** – Re-introduce support for multiple zones (deferred until use case confirmed)

### Broker-Controlled Remapping
**Status**: 🔵 Deferred (broker not ready)

Per `DATAHUB_MEMORY_LAYOUT_AND_REMAPPING_DESIGN.md`:
- [ ] Define broker protocol for structure remapping
- [ ] Implement flex zone reconfiguration
- [ ] Implement ring buffer capacity changes
- [ ] Test producer/consumer coordination during remap
- [ ] Document remapping guarantees and limitations

### Performance Optimizations
- [ ] **4K-aligned flex zones** – Align each flex zone to page boundary
- [ ] **Huge pages support** – Optional huge pages for large blocks
- [ ] **NUMA-aware allocation** – Consider NUMA topology on multi-socket systems

---

## Validation and Testing

### Layout Tests
- [x] LayoutWithChecksumAndFlexibleZoneSucceeds (basic)
- [ ] Structured buffer alignment test (8-byte, 16-byte types)
- [ ] Layout checksum stability across runs
- [ ] Attach with incompatible layout (different padding)
- [ ] Large flex zone (multi-page)
- [ ] Zero flex zone (slots only)

### Integrity Tests
- [x] Integrity validator with flex_zone_size
- [ ] Integrity check on aligned layout
- [ ] Repair corrupted layout checksum
- [ ] Detect misaligned structured region

---

## Design Decisions

### Alignment Strategy
**Decision**: Use fixed 8-byte alignment for structured buffer start.

**Rationale**:
- Covers most common types (int64_t, double, pointers on 64-bit)
- Simple, predictable, no runtime branches
- Small overhead (max 7 bytes padding)

**Alternatives considered**:
- Dynamic alignment based on max type in slot: too complex, not worth it
- 16-byte alignment: unnecessary overhead for most use cases
- No alignment: breaks typed access for uint64_t and similar

### Single Flex Zone
**Decision**: Support only one flex zone (index 0) initially.

**Rationale**:
- Simpler implementation and validation
- Covers 95% of use cases (shared config/state)
- Can add multiple zones later if needed

**Future**: Multiple zones deferred until concrete use case emerges.

---

## Related Work

- **RAII Layer** (`docs/todo/RAII_LAYER_TODO.md`) – Transaction API uses layout
- **Testing** (`docs/todo/TESTING_TODO.md`) – Layout tests in Phase B/C
- **Recovery** (`docs/todo/RECOVERY_TODO.md`) – Integrity validator uses layout

---

## Recent Completions

### 2026-02-14
- ✅ Integrity validator handles `flex_zone_size` path
- ✅ Unified metrics API includes layout state snapshot

### 2026-02-13
- ✅ Single memory structure only (removed legacy paths)
- ✅ Config validation before memory creation

### 2026-02-11
- ✅ DataBlockLayout centralized (from_config, from_header)
- ✅ Fixed slot checksum region overlap

---

## Notes

- **Layout compatibility**: Changes to layout structure require version bump and migration path
- **Alignment requirements**: Document minimum alignment guarantees for typed access
- **Performance impact**: Alignment padding is minimal (0-7 bytes), no hot path cost
