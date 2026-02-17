# DataBlock Checksum Architecture

**Version:** 1.0  
**Date:** 2026-02-15  
**Status:** Active

---

## 1. Overview

The DataBlock system uses a comprehensive checksum architecture with **six distinct types** of checksums, each serving a specific purpose in ensuring data integrity, version compatibility, and structure validation. This document provides a complete reference for understanding when each checksum is calculated, validated, and updated.

### 1.1 Design Principles

| Principle | Description |
|-----------|-------------|
| **Defense in Depth** | Multiple layers of integrity checks catch different classes of errors |
| **Minimal Overhead** | Checksums calculated only when necessary; optional validation paths |
| **Clear Ownership** | Each checksum has a clear owner (broker, producer, consumer) and lifecycle |
| **Protocol Clarity** | Well-defined rules for when to check, when to update, and when to fail |

### 1.2 Checksum Types Summary

| # | Checksum Type | Purpose | Lifetime | Owner | Algorithm |
|---|---------------|---------|----------|-------|-----------|
| 1 | **Magic Number** | Detect uninitialized/corrupted header | Once at creation | Producer | Constant (0x5079_4C61_6248_7562) |
| 2 | **Header Layout Hash** | Verify ABI compatibility | Once at creation | Producer | BLAKE2b (256-bit) |
| 3 | **Layout Checksum** | Validate memory layout configuration | Once at creation | Producer | BLAKE2b (256-bit) |
| 4 | **Schema Hash** | Validate flex zone/slot structure | Once at creation, updated on remap | Producer | BLAKE2b (256-bit) |
| 5 | **Flexible Zone Checksum** | Verify flex zone data integrity | Per-transaction (optional) | Producer/Consumer | BLAKE2b (256-bit) |
| 6 | **Slot Checksum** | Verify ring-buffer slot data integrity | Per-slot commit/consume | Producer/Consumer | BLAKE2b (256-bit) |

---

## 2. Checksum Details

### 2.1 Magic Number

**Purpose:** Detect uninitialized or corrupted shared memory header.

**Value:** `0x5079_4C61_6248_7562` ("PyLabHub" in hex encoding)

**Storage Location:** `SharedMemoryHeader::magic_number` (8 bytes, atomic)

**Lifecycle:**
- **Creation:** Set by producer as the **last step** in DataBlock construction (after all other initialization)
- **Validation:** Checked by consumer on **first attach** and optionally on subsequent operations
- **Update:** **Never** updated after creation

**Validation Protocol:**
```cpp
// Producer (DataBlock constructor)
std::atomic_thread_fence(std::memory_order_release);
m_header->magic_number.store(DATABLOCK_MAGIC_NUMBER, std::memory_order_release);

// Consumer (attach)
uint64_t magic = header->magic_number.load(std::memory_order_acquire);
if (magic != DATABLOCK_MAGIC_NUMBER) {
    // Fatal: memory not initialized or corrupted
    return nullptr;
}
```

**Decision Tree:**
- **Valid** → Proceed with attach
- **Invalid** → **Fatal error**, do not attach

---

### 2.2 Header Layout Hash

**Purpose:** Verify that producer and consumer use the same `SharedMemoryHeader` ABI (field order, sizes, alignments).

**Algorithm:** BLAKE2b-256 of `SharedMemoryHeader` schema (via `get_shared_memory_header_schema_info()`)

**Storage Location:** `SharedMemoryHeader::reserved_header[HEADER_LAYOUT_HASH_OFFSET]` (32 bytes)

**Lifecycle:**
- **Creation:** Computed and stored by producer during DataBlock construction
- **Validation:** Checked by consumer on **attach**
- **Update:** **Never** updated (header layout is fixed for ABI version)

**Validation Protocol:**
```cpp
// Producer (DataBlock constructor)
SchemaInfo header_schema = get_shared_memory_header_schema_info();
std::memcpy(m_header->reserved_header + HEADER_LAYOUT_HASH_OFFSET,
            header_schema.hash.data(), HEADER_LAYOUT_HASH_SIZE);

// Consumer (attach)
SchemaInfo expected_header_schema = get_shared_memory_header_schema_info();
if (std::memcmp(stored_hash, expected_header_schema.hash.data(), HEADER_LAYOUT_HASH_SIZE) != 0) {
    // Fatal: ABI mismatch, incompatible versions
    return nullptr;
}
```

**Decision Tree:**
- **Match** → Proceed
- **Mismatch** → **Fatal error**, incompatible producer/consumer versions

---

### 2.3 Layout Checksum

**Purpose:** Validate that the memory layout configuration (capacity, page size, logical unit size, flex zone size) matches expectations.

**Algorithm:** BLAKE2b-256 of layout-defining fields (see `LayoutChecksumInput`)

**Storage Location:** `SharedMemoryHeader::reserved_header[LAYOUT_CHECKSUM_OFFSET]` (32 bytes)

**Input Fields:**
```cpp
struct LayoutChecksumInput {
    uint8_t  version_major;
    uint8_t  version_minor;
    uint8_t  policy;
    uint8_t  consumer_sync_policy;
    uint32_t ring_buffer_capacity;
    uint32_t physical_page_size;
    uint32_t logical_unit_size;
    size_t   flexible_zone_size;
    uint8_t  checksum_type;
    uint8_t  checksum_policy;
};
```

**Lifecycle:**
- **Creation:** Computed and stored by producer during DataBlock construction (after header initialization)
- **Validation:** Checked by consumer on **attach**
- **Update:** **Never** updated (layout is immutable for DataBlock lifetime)

**Validation Protocol:**
```cpp
// Producer (DataBlock constructor)
store_layout_checksum(m_header);

// Consumer (attach, with expected_config)
if (expected_config != nullptr) {
    if (!validate_attach_layout_and_config(header, expected_config)) {
        // Layout mismatch
        return nullptr;
    }
}
```

**Decision Tree:**
- **Match** (or no `expected_config`) → Proceed
- **Mismatch** → **Fatal error**, layout incompatibility

---

### 2.4 Schema Hash

**Purpose:** Validate that producer and consumer agree on the **structure** of data stored in:
1. **Flexible zone** (user-defined layout)
2. **Ring-buffer slots** (user-defined per-slot structure, e.g., `T` in `with_typed_write<T>`)

**Algorithm:** BLAKE2b-256 of schema definition (type name, size, alignment, version)

**Storage Location:** `SharedMemoryHeader::schema_hash` (32 bytes)

**Lifecycle:**
- **Creation:** Stored by producer if schema validation is enabled (factory provides `schema_info`)
- **Validation:** Checked by consumer on **attach** if schema validation is enabled
- **Update:** Updated on **structure re-mapping** (broker-coordinated, future feature)

**Validation Protocol:**
```cpp
// Producer (factory: create_datablock_producer)
if (schema_info != nullptr) {
    std::memcpy(header->schema_hash, schema_info->hash.data(), CHECKSUM_BYTES);
    header->schema_version = schema_info->version.pack();
}

// Consumer (factory: find_datablock_consumer)
if (schema_info != nullptr) {
    // Check if producer stored a schema
    bool has_producer_schema = /* check if schema_hash is non-zero */;
    if (has_producer_schema) {
        if (std::memcmp(header->schema_hash, schema_info->hash.data(), CHECKSUM_BYTES) != 0) {
            // Schema mismatch
            throw SchemaMismatchException(...);
        }
    }
}
```

**Decision Tree:**
- **No schema validation** (`schema_info == nullptr`) → Skip check
- **Producer has no schema** (all zeros) → Skip check (backward compat)
- **Schemas match** → Proceed
- **Schemas mismatch** → **Fatal error**, incompatible data structures

---

### 2.5 Flexible Zone Checksum

**Purpose:** Verify data integrity of the **flexible zone** (shared auxiliary data region).

**Algorithm:** BLAKE2b-256 of flex zone memory contents

**Storage Location:** `SharedMemoryHeader::flexible_zone_checksums[0]` (64-byte aligned entry, 32-byte hash)

**Lifecycle:**
- **Update:** Called explicitly by producer or consumer (via `update_checksum_flexible_zone()`)
- **Validation:** Called explicitly by producer or consumer (via `verify_checksum_flexible_zone()`)
- **Policy:** Controlled by `ChecksumPolicy`:
  - `None`: Never validated
  - `Manual`: Application calls update/verify explicitly
  - `Enforced`: System calls update/verify at specific points (not automatic for flex zone in current design)

**Validation Protocol:**
```cpp
// Producer/Consumer (manual checksum update)
bool success = producer.update_checksum_flexible_zone(zone_index);

// Producer/Consumer (manual checksum verification)
bool valid = consumer.verify_checksum_flexible_zone(zone_index);
```

**Decision Tree:**
- **Policy::None** → Skip
- **Policy::Manual** → Application responsibility
- **Policy::Enforced** → Future: automatic at transaction boundaries
- **Validation fails** → Application decision (log, retry, abort)

**Special Case:** Flex zone checksums are only meaningful if `has_any_commits()` returns true (at least one slot has been written). Validating before any commits will produce incorrect results.

---

### 2.6 Slot Checksum

**Purpose:** Verify data integrity of individual ring-buffer slots.

**Algorithm:** BLAKE2b-256 of slot memory contents

**Storage Location:** Per-slot checksum array in control zone:
- Offset: `layout.slot_checksum_offset + (slot_index * SLOT_CHECKSUM_ENTRY_SIZE)`
- Size: 33 bytes per slot (1 byte generation + 32 bytes hash)

**Lifecycle:**
- **Update:** Automatic on `release_write_slot()` if `ChecksumPolicy::Enforced`
- **Validation:** Automatic on `release_consume_slot()` if `ChecksumPolicy::Enforced`
- **Policy:** Controlled by `ChecksumPolicy`:
  - `None`: No validation
  - `Manual`: Call `update_checksum_slot()` / `verify_checksum_slot()` explicitly
  - `Enforced`: Automatic on slot release

**Validation Protocol:**
```cpp
// Producer (ChecksumPolicy::Enforced)
// Automatic in release_write_slot():
if (checksum_policy == ChecksumPolicy::Enforced) {
    update_checksum_slot(slot_index);
}

// Consumer (ChecksumPolicy::Enforced)
// Automatic in acquire_consume_slot() validation:
if (checksum_policy == ChecksumPolicy::Enforced) {
    if (!verify_checksum_slot(slot_index)) {
        increment_metric_reader_validation_failed(header);
        return SlotAcquireError::ChecksumMismatch;
    }
}
```

**Decision Tree:**
- **Policy::None** → Skip
- **Policy::Manual** → Application calls `update_checksum_slot()` / `verify_checksum_slot()`
- **Policy::Enforced** → Automatic; validation failure increments `reader_validation_failed` metric
- **Validation fails** → Slot is **not** consumed, returns `SlotAcquireError::ChecksumMismatch`

**Special Case:** Use `has_any_commits()` lightweight API to check if any slots have been written before attempting validation.

---

## 3. Checksum Policy

The `ChecksumPolicy` enum controls **when** checksums are updated and verified:

```cpp
enum class ChecksumPolicy {
    None,    // No checksum enforcement (checksums stored but not validated)
    Manual,  // User explicitly calls update/verify functions
    Enforced // System automatically updates on write, validates on consume
};
```

### 3.1 Policy Behavior Matrix

| Checksum Type | Policy::None | Policy::Manual | Policy::Enforced |
|---------------|--------------|----------------|------------------|
| **Magic Number** | Always validated | Always validated | Always validated |
| **Header Layout Hash** | Always validated | Always validated | Always validated |
| **Layout Checksum** | Always validated | Always validated | Always validated |
| **Schema Hash** | Validated if provided | Validated if provided | Validated if provided |
| **Flexible Zone** | Skipped | Application calls | Future: automatic |
| **Slot Checksum** | Skipped | Application calls | Automatic on release |

**Key Points:**
- **Structural checksums** (magic, header layout, layout, schema) are **always** validated regardless of policy
- **Data checksums** (flex zone, slot) behavior is controlled by the policy
- Policy is set in `DataBlockConfig` at creation time and cannot be changed

---

## 4. Validation Decision Trees

### 4.1 Consumer Attach Validation

```
Consumer attach sequence:
1. Load magic_number (acquire semantics)
   ├─ magic != DATABLOCK_MAGIC_NUMBER → FATAL: corrupted/uninitialized
   └─ magic == DATABLOCK_MAGIC_NUMBER → Continue

2. Validate header layout hash
   ├─ Mismatch → FATAL: ABI incompatibility
   └─ Match → Continue

3. Validate layout checksum (if expected_config provided)
   ├─ Mismatch → FATAL: layout incompatibility
   └─ Match → Continue

4. Validate schema hash (if schema_info provided)
   ├─ Producer has no schema → Continue (backward compat)
   ├─ Schemas match → Continue
   └─ Schemas mismatch → FATAL: data structure incompatibility

5. Attach successful
```

### 4.2 Slot Consume Validation

```
Consumer acquire_consume_slot():
1. Check has_any_commits()
   └─ false → No slots written yet, cannot validate

2. Acquire slot (standard protocol)
   ├─ Timeout → SlotAcquireError::Timeout
   ├─ No data → SlotAcquireError::NoData
   └─ Slot available → Continue

3. Validate slot checksum (if ChecksumPolicy::Enforced)
   ├─ Policy::None or Policy::Manual → Skip validation
   ├─ Policy::Enforced:
   │  ├─ Checksum valid → SlotAcquireSuccess
   │  └─ Checksum invalid → SlotAcquireError::ChecksumMismatch, increment metric
   └─ Return result
```

### 4.3 Flexible Zone Validation

```
Flexible zone checksum verification:
1. Check has_any_commits()
   ├─ false → Skip validation (no data written)
   └─ true → Continue

2. Check ChecksumPolicy
   ├─ Policy::None → Skip validation
   ├─ Policy::Manual → Application decides when to verify
   └─ Policy::Enforced → Verify at designated checkpoints (future)

3. If validation performed:
   ├─ Checksum valid → Continue
   └─ Checksum invalid → Application decision (log, retry, abort)
```

---

## 5. Broker, Producer, Consumer Responsibilities

### 5.1 Producer Responsibilities

| Phase | Action | Checksums Involved |
|-------|--------|-------------------|
| **Creation** | Initialize DataBlock | Magic, Header Layout Hash, Layout Checksum, Schema Hash (optional) |
| **Write Transaction** | Update slot data | Slot Checksum (if Policy::Enforced or Manual) |
| **Flex Zone Update** | Update flex zone data | Flexible Zone Checksum (if Policy::Manual) |
| **Re-mapping** | Update schema | Schema Hash (broker-coordinated, future) |

### 5.2 Consumer Responsibilities

| Phase | Action | Checksums Involved |
|-------|--------|-------------------|
| **Attach** | Validate structure | Magic, Header Layout Hash, Layout Checksum, Schema Hash (if provided) |
| **Read Transaction** | Validate slot data | Slot Checksum (if Policy::Enforced) |
| **Flex Zone Read** | Validate flex zone | Flexible Zone Checksum (if Policy::Manual) |
| **Re-attach** | Revalidate after remap | Schema Hash (broker-coordinated, future) |

### 5.3 Broker Responsibilities (Future)

| Phase | Action | Checksums Involved |
|-------|--------|-------------------|
| **Re-mapping Coordination** | Orchestrate structure update | Schema Hash |
| **Diagnostic Access** | Authorize metrics/checksum queries | All (via `DataBlockDiagnosticHandle`) |

---

## 6. Lightweight Query API

To support checksum validation logic, the system provides lightweight query functions:

```cpp
// Check if any slots have been written (for checksum validation)
inline bool has_any_commits(const SharedMemoryHeader* header) noexcept {
    return (header != nullptr) && 
           (header->total_slots_written.load(std::memory_order_acquire) > 0);
}

// Get total commit count
inline uint64_t get_total_commits(const SharedMemoryHeader* header) noexcept {
    return (header != nullptr) ? 
           header->total_slots_written.load(std::memory_order_acquire) : 0;
}
```

**Usage:**
- **Before validating** flex zone or slot checksums, call `has_any_commits()` to ensure data has been written
- Prevents false validation failures on uninitialized memory

---

## 7. Future Enhancements

### 7.1 Structure Re-Mapping

**Feature:** Allow updating flex zone/slot structure while DataBlock is live

**Checksums Involved:**
- **Schema Hash**: Updated to new structure hash after broker-coordinated remap
- **Layout Checksum**: Remains unchanged (memory layout size/offsets are immutable)
- **Slot/Flex Zone Checksums**: Recomputed after structure change

**Protocol:**
1. Producer requests remap (provides new schema)
2. Broker coordinates: all consumers release, producer updates
3. Producer updates `schema_hash` and `schema_version`
4. Consumers reattach, validate new schema
5. Normal operations resume

### 7.2 Incremental Checksums

**Feature:** Reduce checksum computation overhead for large slots/flex zones

**Approach:**
- Use rolling/incremental checksum algorithms (e.g., CRC32C with SSE4.2)
- Update checksums on partial writes rather than full recomputation
- Requires careful design to maintain atomicity and consistency

### 7.3 Selective Checksum Validation

**Feature:** Validate specific slots/regions on demand

**Use Cases:**
- Recovery/diagnostic tools
- Periodic integrity scans
- Targeted validation after detecting errors

**API:**
```cpp
// Check specific slot without consuming
bool verify_slot_integrity(size_t slot_index) const;

// Scan all slots and report corrupted indices
std::vector<size_t> scan_all_slots() const;
```

---

## 8. Troubleshooting Guide

### 8.1 Common Validation Failures

| Error | Checksum | Cause | Resolution |
|-------|----------|-------|------------|
| "Magic number mismatch" | Magic Number | Shared memory not initialized or corrupted | Recreate DataBlock, check permissions |
| "Header layout hash mismatch" | Header Layout Hash | Producer/consumer version mismatch | Update to matching versions |
| "Layout checksum mismatch" | Layout Checksum | Config mismatch (capacity, page size, etc.) | Ensure consistent `DataBlockConfig` |
| "Schema hash mismatch" | Schema Hash | Structure type mismatch | Use matching struct definitions or disable schema validation |
| "Slot checksum validation failed" | Slot Checksum | Data corruption or concurrent write | Check for bugs, enable write synchronization |
| "Flexible zone checksum invalid" | Flexible Zone Checksum | Data corruption or incomplete write | Verify write atomicity, use transactions |

### 8.2 Debugging Checklist

1. **Enable verbose logging** to trace checksum computation and validation
2. **Check `has_any_commits()`** before validating data checksums
3. **Verify ChecksumPolicy** matches expectations (None/Manual/Enforced)
4. **Inspect metrics** (`reader_validation_failed`, `writer_timeout_count`) for patterns
5. **Use recovery tools** (`DataBlockDiagnosticHandle`) to dump header and checksums
6. **Compare schema hashes** manually if schema validation fails

---

## 9. References

- **HEP-CORE-0002** — Data Exchange Hub specification
- **IMPLEMENTATION_GUIDANCE.md** — Coding standards and best practices
- **DATAHUB_MEMORY_LAYOUT_AND_REMAPPING_DESIGN.md** — Memory layout details
- **METRICS_API_SUMMARY.md** — Metrics and diagnostics
- **REFACTORING_PLAN_2026-02-15.md** — Phase 2 refactoring details

---

## 10. Revision History

| Version | Date | Change |
|---------|------|--------|
| 1.0 | 2026-02-15 | Initial comprehensive checksum architecture documentation (Phase 2 refactoring) |
