# DataHub Refactoring Plan - 2026-02-15

**Status:** Planning Phase  
**Created:** 2026-02-15  
**Context:** Post-rollback recovery and implementation of memory layout + RAII layer designs

---

## 1. Executive Summary

### 1.1 Situation

Our codebase was partially rolled back due to tool errors during previous refactoring attempts. We need to implement two major design changes:

1. **Memory Layout Redesign** (`DATAHUB_MEMORY_LAYOUT_AND_REMAPPING_DESIGN.md`)
2. **RAII Layer Improvements** (`DATAHUB_CPP_RAII_LAYER_DESIGN_DRAFT.md`)

### 1.2 Critical Requirements

Per user's directives:

1. **C-API Level Correctness** 
   - All structures (metrics, config, schema, etc.) must be accessed only through well-organized access functions
   - No direct pointer manipulation outside designated access APIs
   - Example issue: Checksums were calculated before commits → need `total_slots_written` check via lightweight API

2. **C++ Abstraction Level**
   - **ABANDON Layer 1.75** (currently doesn't exist - good!)
   - Implement NEW C++ abstraction based on type-mapping, iterators, and transaction contexts
   - Based on `DATAHUB_CPP_RAII_LAYER_DESIGN_DRAFT.md`

3. **Memory Structure Redesign**
   - **Replace (not layer on)** old memory design
   - Single flexible zone (N×4K pages)
   - Consistent alignment boundaries
   - Simpler offset calculation
   - 4K-aligned data region
   - Support for future re-mapping (deferred until broker ready)

### 1.3 Scope Boundaries

**IN SCOPE:**
- C-API access function cleanup and consolidation
- Memory layout structure changes (single flex zone, alignment)
- New C++ RAII layer (`with_transaction`, context-centric API)
- Removal of multi-flex-zone code paths
- Validation and integrity checks for new layout

**OUT OF SCOPE:**
- Broker implementation (deferred)
- Structure re-mapping protocol (deferred until broker ready)
- MessageHub changes (unless blocking)
- Performance optimizations beyond alignment fixes

---

## 2. Current State Assessment

### 2.1 What Works (Keep/Build On)

✅ **C-API (Layer 0)** - `slot_rw_coordinator.h`:
- `slot_rw_acquire_write`, `slot_rw_commit`, `slot_rw_release_write`
- `slot_rw_acquire_read`, `slot_rw_validate_read`, `slot_rw_release_read`
- `slot_rw_get_metrics`, `slot_rw_reset_metrics`
- **Status:** Stable, well-tested, correct

✅ **Primitive C++ API (Layer 1)**:
- `DataBlockProducer`, `DataBlockConsumer`
- `SlotWriteHandle`, `SlotConsumeHandle`
- Manual `acquire_write_slot()`, `release_write_slot()`
- **Status:** Working, needs access function cleanup

✅ **Transaction API (Layer 2)** - Partially implemented:
- `with_write_transaction`, `with_read_transaction`
- `with_typed_write<T>`, `with_typed_read<T>`
- `WriteTransactionGuard`, `ReadTransactionGuard`
- **Status:** Good foundation, needs enhancement per new design

✅ **Testing Infrastructure**:
- Phase A/B/C tests in place
- Multi-process worker tests
- Recovery API tests
- **Status:** Comprehensive, recently fixed (Pitfall 10, FileLock barrier)

### 2.2 What Needs Change

🔴 **Problem Area 1: Inconsistent Structure Access**

**Current Issues:**
- Direct pointer access to `header->commit_index`, `header->total_slots_written`, etc.
- Scattered throughout `data_block.cpp` (60+ direct accesses found)
- Metrics updated inline without centralized validation
- No lightweight access API for runtime maintenance checks

**Example Bad Pattern:**
```cpp
// Scattered direct access
header->commit_index.fetch_add(1, std::memory_order_release);
header->total_slots_written.fetch_add(1, std::memory_order_release);
// ... many lines later, different function ...
uint64_t commits = header->total_slots_written.load(std::memory_order_acquire);
```

**Required:**
- Centralized access functions for all header fields
- Validation hooks for critical operations
- Consistent memory ordering
- Lightweight getters for runtime checks (e.g., `has_any_commits()`)

🔴 **Problem Area 2: Multi-Flex-Zone Legacy**

**Current Code:**
- `FlexibleZoneConfig` struct and `flexible_zone_configs` vector
- `build_flexible_zone_info()` function
- Per-zone offset calculation
- Multi-zone checksum arrays
- **Found:** ~12 locations in `data_block.cpp`, plus header definitions

**Required:**
- Remove `flexible_zone_configs` → single `flex_zone_size` (N×4K)
- Remove `FlexibleZoneInfo` map → single zone at offset 0
- Update `flexible_zone_span(index)` → assert index == 0 or remove parameter
- Update tests using multiple zones

🔴 **Problem Area 3: Memory Layout Misalignment**

**Current:**
- Structured buffer starts immediately after flex zone (no padding)
- Example: `structured_buffer_offset = flexible_zone_offset + flexible_zone_size` (e.g., 4258 bytes)
- Not 8-byte aligned for types with `alignof(T) == 8`

**Required:**
- 4K-aligned data region start
- 8-byte aligned structured buffer within data region
- Updated layout checksum calculation
- Padding between control zone and data region

🔴 **Problem Area 4: C++ Layer Design Gap**

**Current Implementation:**
- Transaction API exists but doesn't match new design
- No context-centric iteration (`ctx.slots(timeout)`)
- No `Result<SlotRef, AcquireError>` type
- No separation of flexzone and slot access via context
- No heartbeat hybrid model

**Required:** Full implementation per `DATAHUB_CPP_RAII_LAYER_DESIGN_DRAFT.md`

---

## 3. Implementation Strategy

### 3.1 Phased Approach

**Phase 1: C-API Access Function Cleanup** (Foundation)
- Create centralized access functions for all header fields
- Update all call sites to use access functions
- Add validation and lightweight query APIs
- **Goal:** Make Phase 2-3 changes safe

**Phase 2: Memory Layout Refactoring** (Structure)
- Single flex zone implementation
- Alignment fixes (4K data region, 8-byte structured buffer)
- Layout checksum updates
- **Goal:** Correct memory structure

**Phase 3: C++ RAII Layer Implementation** (Abstraction)
- Context-centric transaction API
- Iterator with Result type
- Typed and raw access
- **Goal:** Usable, type-safe API

**Phase 4: Integration and Testing** (Validation)
- Update all tests
- Verify multi-process scenarios
- Performance validation
- **Goal:** Production-ready

### 3.2 Risk Mitigation

**Risk 1: Breaking Existing Tests**
- **Mitigation:** Run test suite after each sub-phase
- Keep old API alongside new during transition
- Mark old API as deprecated, remove after validation

**Risk 2: ABI Compatibility**
- **Mitigation:** Layout changes break compatibility - this is acceptable
- Document version bump (major version increment)
- Old and new segments cannot coexist

**Risk 3: Incomplete Refactoring**
- **Mitigation:** Use compiler errors as checklist
- Grep for old patterns before marking complete
- Lint checks for direct header access

---

## 4. Detailed Implementation Plan

### Phase 1: C-API Access Function Cleanup

**Duration Estimate:** 3-4 hours  
**Prerequisites:** None  
**Deliverables:** Centralized access API, updated call sites

#### 1.1 Design Access Function Categories

Create helper namespace in `data_block.cpp`:

```cpp
namespace detail {

// === Metrics Access ===
inline void increment_metric_writer_timeout(SharedMemoryHeader* h) {
    if (h) h->writer_timeout_count.fetch_add(1, std::memory_order_relaxed);
}

inline void increment_metric_commit_count(SharedMemoryHeader* h) {
    if (h) h->total_slots_written.fetch_add(1, std::memory_order_release);
}

inline uint64_t get_total_commits(const SharedMemoryHeader* h) {
    return h ? h->total_slots_written.load(std::memory_order_acquire) : 0;
}

inline bool has_any_commits(const SharedMemoryHeader* h) {
    return get_total_commits(h) > 0;
}

// === Index Access ===
inline uint64_t get_commit_index(const SharedMemoryHeader* h) {
    return h ? h->commit_index.load(std::memory_order_acquire) : INVALID_SLOT_ID;
}

inline void increment_commit_index(SharedMemoryHeader* h) {
    if (h) h->commit_index.fetch_add(1, std::memory_order_release);
}

inline uint64_t get_write_index(const SharedMemoryHeader* h) {
    return h ? h->write_index.load(std::memory_order_acquire) : 0;
}

// === Config Access (read-only) ===
inline DataBlockPolicy get_policy(const SharedMemoryHeader* h) {
    return h ? h->policy : DataBlockPolicy::Unset;
}

inline ConsumerSyncPolicy get_consumer_sync_policy(const SharedMemoryHeader* h) {
    return h ? h->consumer_sync_policy : ConsumerSyncPolicy::Unset;
}

inline uint32_t get_ring_buffer_capacity(const SharedMemoryHeader* h) {
    return h ? h->ring_buffer_capacity : 0;
}

// === Heartbeat Access ===
inline std::atomic<uint64_t>* producer_heartbeat_id_ptr(SharedMemoryHeader* h);
inline std::atomic<uint64_t>* producer_heartbeat_ns_ptr(SharedMemoryHeader* h);
// ... existing heartbeat functions ...

// === Schema Access ===
inline bool validate_schema_hash(const SharedMemoryHeader* h, const uint8_t* expected_hash);
inline uint32_t get_schema_version(const SharedMemoryHeader* h);

} // namespace detail
```

**Categories:**
1. **Metrics** - increment_*, get_* for counters
2. **Indices** - commit_index, write_index, read_index access
3. **Config** - read-only policy/capacity queries
4. **Heartbeat** - producer/consumer liveness
5. **Schema** - validation and versioning
6. **Checksum** - layout/slot/flexzone checksum access

#### 1.2 Systematic Replacement

**Strategy:**
1. Implement one category at a time
2. Use compiler to find all `header->field` access
3. Replace with `detail::get_field(header)` or `detail::increment_field(header)`
4. Run tests after each category

**Automation:**
```bash
# Find all direct header field access
rg 'header->(commit_index|total_slots_written|writer_timeout_count)' cpp/src/utils/data_block.cpp

# After replacement, verify no direct access remains
rg 'header->(commit_index|total_slots_written)(?!.*// ACCESS_FUNCTION_OK)' cpp/src/utils/data_block.cpp
```

#### 1.3 Validation Hooks

Add validation to critical operations:

```cpp
inline void commit_write_impl(SharedMemoryHeader* h, SlotRWState* rw_state) {
    // Validate pre-conditions
    assert(rw_state->slot_state.load(std::memory_order_acquire) == SlotState::WRITING);
    
    // Perform commit
    rw_state->slot_state.store(SlotState::COMMITTED, std::memory_order_release);
    
    // Update indices and metrics atomically
    increment_commit_index(h);
    increment_metric_commit_count(h);
    
    // Validate post-conditions
    assert(get_commit_index(h) != INVALID_SLOT_ID);
}
```

#### 1.4 Export Lightweight Query API

For validation and maintenance (like checksum decision logic):

```cpp
// In data_block.hpp - public API
class DataBlockProducer {
public:
    bool has_any_commits() const noexcept;
    uint64_t get_total_commits() const noexcept;
    // ...
};
```

**Use case from user's example:**
```cpp
// In checksum validation logic:
if (!producer->has_any_commits()) {
    // Skip checksum validation - no commits yet
    return true;
}
// Otherwise, validate checksum
```

---

### Phase 2: Memory Layout Refactoring + Checksum Architecture

**Duration Estimate:** 5-7 hours  
**Prerequisites:** Phase 1 complete  
**Deliverables:** Single flex zone, aligned layout, comprehensive checksum architecture documentation, updated tests

#### 2.1 Remove Multi-Flex-Zone Support

**Step 1:** Update Configuration Structures

**File:** `data_block.hpp`

**Remove:**
```cpp
struct FlexibleZoneConfig {
    std::string name;
    size_t size;
    size_t spinlock_index;
};
std::vector<FlexibleZoneConfig> flexible_zone_configs;
```

**Add:**
```cpp
// Single flex zone configuration
size_t flex_zone_size = 0;  // Must be multiple of 4096, or 0 for no flex zone
```

**Step 2:** Update Layout Calculation

**File:** `data_block.cpp`

**Current:**
```cpp
size_t total_flexible_zone_size() const {
    size_t sum = 0;
    for (const auto& cfg : flexible_zone_configs) {
        sum += cfg.size;
    }
    return sum;
}
```

**New:**
```cpp
// No helper needed - use flex_zone_size directly
layout.flexible_zone_size = config.flex_zone_size;
// Validate: must be 0 or multiple of 4096
if (layout.flexible_zone_size % 4096 != 0) {
    throw std::invalid_argument("flex_zone_size must be multiple of 4096");
}
```

**Step 3:** Remove FlexibleZoneInfo Infrastructure

**Current:** Map-based multi-zone tracking
```cpp
std::unordered_map<std::string, FlexibleZoneInfo> m_flexible_zone_info;
```

**New:** Single zone, no name lookup needed
```cpp
// No per-zone tracking; zone always at offset flexible_zone_offset, size flexible_zone_size
```

**Step 4:** Update flexible_zone_span() API

**Current:**
```cpp
std::span<std::byte> flexible_zone_span(const std::string& name);
```

**New (two options):**

**Option A:** Keep index for future expansion, validate == 0
```cpp
std::span<std::byte> flexible_zone_span(size_t index = 0) {
    if (index != 0) throw std::out_of_range("Only flex zone index 0 supported");
    // Return span
}
```

**Option B:** Remove index entirely
```cpp
std::span<std::byte> flexible_zone_span() {
    // Return the single flex zone
}
```

**Recommendation:** Option A for API stability

**Step 5:** Grep and Update All Call Sites

```bash
# Find all flexible_zone_configs usage
rg 'flexible_zone_configs' cpp/src/

# Find all FlexibleZoneInfo usage  
rg 'FlexibleZoneInfo|flexible_zone_info' cpp/src/

# Find all build_flexible_zone_info calls
rg 'build_flexible_zone_info' cpp/src/

# Update each location
```

**Expected locations:** ~12 in data_block.cpp, ~3 in tests

#### 2.2 Implement Alignment Requirements

**Current Layout:**
```
Header (4K) | SlotRWState[] | SlotChecksum[] | FlexZone | StructuredBuffer
                                                ^--- NOT aligned
```

**New Layout:**
```
Header (4K) | SlotRWState[] | SlotChecksum[] | [padding] | DATA_REGION (4K-aligned)
                                                ^-padding   ^--- FlexZone | StructuredBuffer
                                                                           ^--- 8-byte aligned
```

**Implementation:**

```cpp
// In DataBlockLayout::from_config
layout.slot_checksum_offset = layout.slot_rw_state_offset + layout.slot_rw_state_size;
layout.control_zone_end = layout.slot_checksum_offset + layout.slot_checksum_size;

// Align data region to 4K boundary
layout.data_region_offset = align_up(layout.control_zone_end, 4096);

// Flex zone starts at data region (already 4K-aligned)
layout.flexible_zone_offset = layout.data_region_offset;
layout.flexible_zone_size = config.flex_zone_size;  // Already multiple of 4K

// Structured buffer starts after flex zone, aligned to 8 bytes
size_t after_flex = layout.flexible_zone_offset + layout.flexible_zone_size;
layout.structured_buffer_offset = align_up(after_flex, 8);

// Structured buffer size unchanged
layout.structured_buffer_size = layout.slot_count * layout.slot_stride_bytes_;

// Total size
layout.total_size = layout.structured_buffer_offset + layout.structured_buffer_size;
```

**Helper function:**
```cpp
inline constexpr size_t align_up(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}
```

#### 2.3 Update Layout Checksum

**Current checksum includes:** All layout-defining fields  
**Add:** Control zone padding size, data region alignment

```cpp
struct LayoutChecksumInput {
    // ... existing fields ...
    size_t control_zone_padding;      // NEW: bytes between control end and data region
    size_t structured_buffer_padding; // NEW: bytes between flex zone and structured buffer
    // ...
};
```

**Rationale:** Different padding = different layout = should fail validation

#### 2.4 Update from_header() to Match

Ensure `DataBlockLayout::from_header()` uses same logic as `from_config()`:

```cpp
// Reconstruct offsets with same alignment rules
layout.data_region_offset = align_up(layout.control_zone_end, 4096);
layout.flexible_zone_offset = layout.data_region_offset;
size_t after_flex = layout.flexible_zone_offset + header->flexible_zone_size;
layout.structured_buffer_offset = align_up(after_flex, 8);
```

#### 2.5 Checksum Architecture Analysis and Documentation

**CRITICAL ADDITION:** Before proceeding with code changes, we need to establish a comprehensive understanding of the checksum landscape.

**Current Checksum Types Identified:**

1. **Magic Number** (`header->magic_number`)
   - **Purpose:** Header validity marker, indicates segment is initialized
   - **Lifetime:** Set once at creation, never changes
   - **Validation:** Consumer checks on attach
   - **Protects:** Against uninitialized or corrupted segment base

2. **Header Layout Hash** (`reserved_header[HEADER_LAYOUT_HASH_OFFSET]`)
   - **Purpose:** ABI compatibility check (SharedMemoryHeader structure)
   - **Lifetime:** Set at creation, never changes
   - **Validation:** Consumer checks on attach
   - **Protects:** Against ABI mismatch between producer/consumer versions

3. **Layout Checksum** (`reserved_header[LAYOUT_CHECKSUM_OFFSET]`)
   - **Purpose:** Memory segment layout validation (offsets, sizes, alignment)
   - **Lifetime:** Set at creation, could change on remapping (deferred)
   - **Validation:** Consumer checks on attach, integrity validator
   - **Protects:** Against layout corruption, ensures offset calculations correct

4. **Schema Hash** (`header->schema_hash`)
   - **Purpose:** Data structure compatibility (FlexZoneT, DataBlockT)
   - **Lifetime:** Set at creation with schema, could change on remapping
   - **Validation:** Consumer checks on attach if schema-aware
   - **Protects:** Against type mismatch in typed access

5. **Flexible Zone Checksum** (`header->flexible_zone_checksums[]`)
   - **Purpose:** Flexible zone data integrity
   - **Lifetime:** Updated on explicit user request or Enforced policy
   - **Validation:** On read if ChecksumPolicy != None
   - **Protects:** Against corruption of shared metadata/config in flex zone

6. **Slot Checksum** (SlotChecksum array, per-slot)
   - **Purpose:** Ring buffer slot data integrity
   - **Lifetime:** Updated on commit (release_write_slot)
   - **Validation:** On read if ChecksumPolicy != None
   - **Protects:** Against corruption of data payload

**Checksum Lifecycle Matrix:**

| Checksum Type | Set At | Updated When | Validated When | Change Triggers Version Bump? |
|---------------|--------|--------------|----------------|-------------------------------|
| Magic Number | Creation | Never | Attach | N/A (not versioned) |
| Header Layout Hash | Creation | Never (ABI frozen per version) | Attach | Yes (ABI change) |
| Layout Checksum | Creation | Remapping (future) | Attach, Integrity check | No (layout change, not ABI) |
| Schema Hash | Creation w/ schema | Remapping (future) | Attach (if schema-aware) | No (data schema, not code) |
| Flexible Zone Checksum | First write to zone | User/Policy driven | Read (if policy) | No (data content) |
| Slot Checksum | Slot commit | Every commit | Read (if policy) | No (data content) |

**Validation Decision Tree:**

```
Attach (Consumer):
  1. Check Magic Number → fail fast if invalid
  2. Check Header Layout Hash → fail if ABI mismatch
  3. Check Layout Checksum → fail if segment corrupted
  4. Check Schema Hash (if schema-aware) → fail if type mismatch
  5. Proceed to normal operation

Read Operation:
  1. Acquire slot/zone
  2. If ChecksumPolicy != None:
     a. If has_any_commits() == false → SKIP checksum (no data yet)
     b. Otherwise → verify checksum, fail if mismatch
  3. Access data
  
Write Operation:
  1. Acquire slot/zone
  2. Write data
  3. Commit/Release:
     a. If ChecksumPolicy == Enforced → compute and store checksum
     b. If ChecksumPolicy == Manual → user must call update_checksum_*
     c. If ChecksumPolicy == None → skip checksum
```

**Key Insight for User's Concern:**

The `has_any_commits()` check is critical:
- **Problem:** Checksum validation was attempted before any data written
- **Solution:** Use `total_slots_written` (accessible via Phase 1 access function)
- **Location:** In `verify_checksum_*` functions, add:
  ```cpp
  if (!detail::has_any_commits(header)) {
      return true; // No commits yet, checksum meaningless
  }
  ```

**Protocol for Broker/Producer/Consumer:**

**Creation (Producer):**
1. Set Magic Number (last, atomic)
2. Set Header Layout Hash
3. Compute and store Layout Checksum
4. Set Schema Hash (if schema-aware)
5. Initialize flex zone/slot checksums to "invalid" state

**Attach (Consumer):**
1. Wait for Magic Number
2. Validate Header Layout Hash (ABI compatibility)
3. Validate Layout Checksum (segment integrity)
4. Validate Schema Hash (if expected_schema provided)
5. Register with broker (future)

**Normal Operation:**
- Slot checksums: Updated on commit, validated on read (policy-dependent)
- Flex zone checksums: Updated explicitly or on Enforced policy
- Always check `has_any_commits()` before validating content checksums

**Remapping (Future with Broker - API designed in Phase 2):**
1. Producer calls `request_structure_remap()` with new schema
2. Broker validates new structure fits existing memory
3. Broker signals consumers to call `release_for_remap()`
4. Consumers detach and wait
5. Producer calls `commit_structure_remap()` to update schema_hash, schema_version, checksums
6. Broker signals remapping complete
7. Consumers call `reattach_after_remap()` with new schema
8. Producer/consumers continue with `with_transaction()` using new types

**Documentation Deliverable:**

Create `docs/CHECKSUM_ARCHITECTURE.md` with:
- Full checksum type catalog
- Lifecycle and validation rules
- Decision trees for validation
- Protocol for broker/producer/consumer
- Examples of each checksum use case
- **Remapping protocol** - how schema_hash and checksums update during remap
- **Placeholder API documentation** - future remapping support

#### 2.6 Structure Re-Mapping Placeholder API

**CRITICAL: Design API hooks now for future broker-coordinated remapping**

Per `DATAHUB_MEMORY_LAYOUT_AND_REMAPPING_DESIGN.md` §3: Structure re-mapping allows updating the schema of flex zone and/or ring buffer slots **without changing memory size**, coordinated by broker.

**Remapping Protocol (Future):**
1. Producer/Consumer request remap via broker
2. Broker validates new structure fits existing memory
3. Broker signals all consumers to release context
4. Consumers detach, wait for broker signal
5. Producer updates schema_hash, schema_version, checksums
6. Broker signals new structure active
7. Producer/Consumer reattach with new schema

**Placeholder API to Design NOW:**

```cpp
// In DataBlockProducer (placeholder - will throw "not implemented" until broker ready)
class DataBlockProducer {
public:
    /**
     * @brief Request structure remapping (requires broker coordination).
     * @param new_flexzone_schema New flex zone structure info
     * @param new_datablock_schema New ring buffer slot structure info
     * @return RequestId for broker coordination
     * @throws std::runtime_error("Remapping requires broker - not implemented")
     * 
     * NOTE: Placeholder API. Implementation deferred until broker is ready.
     * This API ensures our design doesn't block future remapping.
     */
    [[nodiscard]] uint64_t request_structure_remap(
        std::optional<SchemaInfo> new_flexzone_schema,
        std::optional<SchemaInfo> new_datablock_schema
    );
    
    /**
     * @brief Commit structure remapping (after broker approval).
     * @param request_id From request_structure_remap()
     * @param new_flexzone_schema New flex zone structure (if remapping flex zone)
     * @param new_datablock_schema New slot structure (if remapping slots)
     * @throws std::runtime_error if not coordinator, or broker hasn't approved
     * 
     * Updates schema_hash, schema_version, and recomputes checksums.
     * Must be called with all consumers detached (broker-coordinated).
     */
    void commit_structure_remap(
        uint64_t request_id,
        std::optional<SchemaInfo> new_flexzone_schema,
        std::optional<SchemaInfo> new_datablock_schema
    );
};

// In DataBlockConsumer
class DataBlockConsumer {
public:
    /**
     * @brief Release context for structure remapping.
     * Called in response to broker signal.
     * Consumer waits for broker approval before reattaching.
     */
    void release_for_remap();
    
    /**
     * @brief Reattach after structure remapping.
     * @param new_flexzone_schema Expected flex zone structure
     * @param new_datablock_schema Expected slot structure
     * @throws SchemaMismatchException if reattach with wrong schema
     */
    void reattach_after_remap(
        std::optional<SchemaInfo> new_flexzone_schema,
        std::optional<SchemaInfo> new_datablock_schema
    );
};
```

**Validation Hooks to Add NOW:**

```cpp
// In with_transaction entry validation
template <typename FlexZoneT, typename DataBlockT>
void validate_context_entry() {
    // ... existing validation ...
    
    // Check if remapping is in progress
    if (is_remap_in_progress()) {
        throw RemapInProgressException(
            "Cannot acquire context - structure remapping in progress. "
            "Wait for broker signal."
        );
    }
    
    // Validate schema hasn't changed since last access
    // (catches case where remap happened but client didn't reattach)
    auto expected_hash = compute_schema_hash<FlexZoneT, DataBlockT>();
    if (!validate_schema_hash(header_, expected_hash)) {
        throw SchemaChangedException(
            "Structure has been remapped. Call reattach_after_remap() with new schema."
        );
    }
}
```

**Reserved Header Fields for Remapping State:**

```cpp
// In reserved_header[] - allocate space now for future use
inline constexpr size_t REMAP_STATE_OFFSET = 160;  // After producer heartbeat
inline constexpr size_t REMAP_REQUEST_ID_OFFSET = 168;

// Remap state flags (for future implementation)
enum class RemapState : uint8_t {
    None = 0,
    Requested = 1,      // Producer requested remap
    BrokerApproved = 2, // Broker validated, waiting for consumers to detach
    InProgress = 3,     // Producer updating schema/checksums
    Complete = 4        // Remap done, consumers can reattach
};
```

**Phase 2 Deliverables for Remapping:**

1. ✅ Placeholder API defined in header (throws "not implemented")
2. ✅ Validation hooks check remapping state
3. ✅ Reserved header space allocated
4. ✅ Schema versioning integrated in validation
5. ✅ Documentation in `CHECKSUM_ARCHITECTURE.md` explains remapping impact

**Why Design This NOW:**

- **Memory layout** - ensure no hardcoded assumptions about schema immutability
- **Validation logic** - need hooks to detect schema changes
- **API surface** - easier to add now than retrofit later
- **Testing** - can mock broker signals in tests
- **Documentation** - users know remapping will be supported

**Implementation Timeline:**

- **Phase 2 (NOW):** Design placeholder API, validation hooks, reserved space
- **Phase 4 (NOW):** Document remapping protocol in CHECKSUM_ARCHITECTURE.md
- **Future (Broker Ready):** Implement actual remapping logic, remove "not implemented" throws

#### 2.7 Update Tests

**Tests requiring changes:**
- `LayoutWithChecksumAndFlexibleZoneSucceeds` - update to single flex zone
- Any test using multiple flex zones - remove or convert to single zone
- Alignment tests - add explicit checks for 4K and 8-byte boundaries
- **NEW:** Checksum validation test with `has_any_commits()` check
- **NEW:** Test attach with each checksum type mismatch (should fail appropriately)
- **NEW:** Test placeholder remapping API throws "not implemented"
- **NEW:** Test validation detects schema change (mock scenario)

---

### Phase 3: C++ RAII Layer Implementation

**Duration Estimate:** 7-9 hours  
**Prerequisites:** Phase 1 & 2 complete  
**Deliverables:** Context-centric transaction API with non-terminating iterator per design document

This phase implements `DATAHUB_CPP_RAII_LAYER_DESIGN_DRAFT.md` fully, with clarifications on iterator behavior.

#### 3.1 Result Type

**File:** `data_block.hpp` (or new `transaction_api.hpp`)

```cpp
enum class SlotAcquireError {
    Timeout,
    NoSlot,
    Error
};

template <typename SlotRefT>
class SlotAcquireResult {
    std::variant<SlotRefT, SlotAcquireError> value_;
    int error_code_ = 0;  // Only valid when Error

public:
    static SlotAcquireResult ok(SlotRefT slot) {
        return SlotAcquireResult(std::move(slot));
    }
    
    static SlotAcquireResult timeout() {
        return SlotAcquireResult(SlotAcquireError::Timeout);
    }
    
    static SlotAcquireResult no_slot() {
        return SlotAcquireResult(SlotAcquireError::NoSlot);
    }
    
    static SlotAcquireResult error(int code) {
        SlotAcquireResult r(SlotAcquireError::Error);
        r.error_code_ = code;
        return r;
    }
    
    bool is_ok() const {
        return std::holds_alternative<SlotRefT>(value_);
    }
    
    SlotRefT& slot() {
        return std::get<SlotRefT>(value_);
    }
    
    SlotAcquireError error() const {
        return std::get<SlotAcquireError>(value_);
    }
    
    int error_code() const { return error_code_; }
};
```

#### 3.2 Context Types

**CRITICAL DESIGN CLARIFICATION:**

Per user feedback, the iterator is accessed via `ctx.datablock()` (not `ctx.slots()`), and operates as follows:

- **Iterator does NOT terminate on timeout/unavailable** - it blocks according to timeout policy
- **Iterator ONLY terminates on:**
  - Unrecoverable error (exception thrown)
  - Broker-signaled controlled termination
  - User explicit break
- **Iterator yields actual data blocks**, not Result types for normal iteration
- Context is aware of producer/consumer role and respects policy

**WriteTransactionContext:**
```cpp
class WriteTransactionContext {
    DataBlockProducer* producer_;
    // ... impl details ...

public:
    // Flexzone typed access (validated at transaction entry)
    template <typename T>
    T& flexzone(size_t index = 0);
    
    // Flexzone raw access (user's responsibility after validation)
    std::span<std::byte> raw_flexzone_access(size_t index = 0);
    
    // Datablock iterator (NON-TERMINATING on timeout)
    auto datablock();  // Returns range/iterator
    
    // Commit current block
    void commit();
    
    // Validate current block (read check)
    bool validate_read();  // Consumer only
    
    // Heartbeat update
    void update_heartbeat();
};
```

**ReadTransactionContext:** Similar with const-qualified flexzone access and validate_read()

**Key API Changes from Design Doc:**
- `ctx.slots()` → `ctx.datablock()` (more intuitive naming)
- `ctx.flexzone_raw()` → `ctx.raw_flexzone_access()` (explicit "access" suffix)
- Iterator behavior: **blocks and retries** instead of yielding timeout Results

#### 3.3 Datablock Iterator (Non-Terminating Design)

**CRITICAL: Iterator does NOT quit on timeout/unavailable**

The iterator blocks and retries according to timeout policy. It only terminates on:
1. Unrecoverable error (throws exception)
2. Broker-signaled termination (checks message/signal)
3. User explicitly breaks

```cpp
class DataBlockIterator {
    WriteTransactionContext* ctx_;
    int timeout_ms_;
    bool ended_ = false;  // Only set on unrecoverable error or broker signal

public:
    // Iterator yields direct reference to datablock (not Result)
    using value_type = DataBlockRef;  // Wraps typed or raw access
    
    DataBlockIterator& operator++() {
        while (true) {  // NON-TERMINATING LOOP
            auto acquire_result = try_acquire_next();
            
            if (acquire_result.success()) {
                current_block_ = acquire_result.block();
                return *this;  // Got a block
            }
            
            // Handle non-success cases
            switch (acquire_result.error()) {
                case AcquireError::Timeout:
                    // KEEP TRYING - don't terminate
                    continue;
                    
                case AcquireError::NoSlot:
                    // KEEP TRYING - don't terminate
                    std::this_thread::yield();
                    continue;
                    
                case AcquireError::BrokerTermination:
                    // Controlled shutdown
                    ended_ = true;
                    return *this;
                    
                case AcquireError::UnrecoverableError:
                    // Throw exception
                    throw DataBlockException("Unrecoverable error");
                    
                default:
                    continue;  // Retry for other cases
            }
        }
    }
    
    DataBlockRef& operator*() {
        return current_block_;
    }
    
    bool operator==(const DataBlockIterator& other) const {
        return ended_ == other.ended_;
    }
};

// DataBlockRef provides typed and raw access
class DataBlockRef {
    void* raw_ptr_;
    size_t size_;

public:
    // Typed access (validated at transaction entry)
    template <typename T>
    T& get() {
        return *static_cast<T*>(raw_ptr_);
    }
    
    // Raw access (user's responsibility)
    std::span<std::byte> raw_datablock_access() {
        return std::span<std::byte>(static_cast<std::byte*>(raw_ptr_), size_);
    }
};

class DataBlockRange {
    WriteTransactionContext* ctx_;

public:
    DataBlockIterator begin() { 
        return DataBlockIterator(ctx_, ctx_->timeout_ms_); 
    }
    
    DataBlockIterator end() { 
        return DataBlockIterator::end_sentinel(); 
    }
};
```

**Usage Pattern:**
```cpp
producer.with_transaction<FlexZoneT, DataBlockT>(timeout, [](auto& ctx) {
    // Flexzone access
    ctx.flexzone<FlexZoneT>().status = Status::Active;
    
    // Non-terminating iterator - only quits on error or broker signal
    for (auto& block : ctx.datablock()) {
        // Check termination condition via flexzone or MessageHub
        if (ctx.flexzone<FlexZoneT>().shutdown_requested) {
            break;  // User-controlled exit
        }
        
        // Write data (typed access)
        block.get<DataBlockT>().timestamp = now();
        block.get<DataBlockT>().payload = produce();
        
        // Or raw access if needed
        // auto raw = block.raw_datablock_access();
        // std::memcpy(raw.data(), src, size);
        
        ctx.commit();
    }
    
    ctx.flexzone<FlexZoneT>().status = Status::Idle;
});
```

**Consumer similar but const-qualified and with validate_read():**
```cpp
consumer.with_transaction<FlexZoneT, DataBlockT>(timeout, [](auto& ctx) {
    for (const auto& block : ctx.datablock()) {
        if (ctx.flexzone<FlexZoneT>().end_of_stream) {
            break;
        }
        
        if (!ctx.validate_read()) {
            continue;  // TOCTTOU race, retry
        }
        
        // Process data
        process(block.get<DataBlockT>());
    }
});
```

#### 3.4 with_transaction Member Functions with Schema Validation

**CRITICAL: Two-layer type safety - compile-time AND runtime**

```cpp
// In DataBlockProducer
template <typename FlexZoneT, typename DataBlockT, typename Func>
auto with_transaction(int timeout_ms, Func&& func) {
    // === COMPILE-TIME VALIDATION ===
    static_assert(std::is_trivially_copyable_v<FlexZoneT>,
                  "FlexZoneT must be trivially copyable for shared memory");
    static_assert(std::is_trivially_copyable_v<DataBlockT>,
                  "DataBlockT must be trivially copyable for shared memory");
    static_assert(sizeof(FlexZoneT) <= max_flexzone_size(),
                  "FlexZoneT size exceeds configured flex zone size");
    static_assert(sizeof(DataBlockT) <= max_datablock_size(),
                  "DataBlockT size exceeds configured slot size");
    
    // === RUNTIME VALIDATION ===
    // 1. Generate schema info for template types
    auto flexzone_schema = generate_schema_info<FlexZoneT>();
    auto datablock_schema = generate_schema_info<DataBlockT>();
    
    // 2. Validate against stored schema in header
    validate_schema_at_entry(flexzone_schema, datablock_schema);
    
    // 3. Create context
    WriteTransactionContext ctx(this, timeout_ms);
    
    // 4. Invoke lambda
    return std::invoke(std::forward<Func>(func), ctx);
}

private:
    // Validate schema matches header (throws on mismatch)
    void validate_schema_at_entry(
        const SchemaInfo& flexzone_schema,
        const SchemaInfo& datablock_schema
    ) {
        // Check if DataBlock has stored schema
        if (!has_stored_schema()) {
            // Schema-unaware DataBlock - skip validation
            // (Created without schema, or old version)
            return;
        }
        
        // Validate flex zone schema
        if (!validate_flexzone_schema(flexzone_schema)) {
            throw SchemaMismatchException(fmt::format(
                "Flex zone schema mismatch.\n"
                "Expected: {} (hash: {})\n"
                "Got: {} (hash: {})\n"
                "Hint: DataBlock was created with different FlexZoneT type",
                get_stored_flexzone_schema_name(),
                format_hash(get_stored_flexzone_hash()),
                flexzone_schema.name,
                format_hash(flexzone_schema.hash)
            ));
        }
        
        // Validate datablock schema
        if (!validate_datablock_schema(datablock_schema)) {
            throw SchemaMismatchException(fmt::format(
                "DataBlock schema mismatch.\n"
                "Expected: {} (hash: {})\n"
                "Got: {} (hash: {})\n"
                "Hint: DataBlock was created with different DataBlockT type",
                get_stored_datablock_schema_name(),
                format_hash(get_stored_datablock_hash()),
                datablock_schema.name,
                format_hash(datablock_schema.hash)
            ));
        }
    }
};
```

#### 3.5 Public Schema Generation API

**Users need these APIs to create schema-aware DataBlocks:**

```cpp
// In schema_blds.hpp (already exists, extend it)

/**
 * @brief Generate schema information for a type T.
 * @tparam T Type to generate schema for (must be trivially copyable)
 * @return SchemaInfo with name, size, alignment, and BLAKE2b hash
 * 
 * This is used internally by with_transaction() and publicly by users
 * to create schema-aware DataBlocks.
 */
template <typename T>
SchemaInfo generate_schema_info() {
    static_assert(std::is_trivially_copyable_v<T>,
                  "T must be trivially copyable for schema generation");
    
    SchemaInfo info;
    info.name = typeid(T).name();  // Or demangled name
    info.size = sizeof(T);
    info.alignment = alignof(T);
    
    // Generate BLAKE2b hash of type layout
    // Includes: name, size, alignment, and field offsets if possible
    info.hash = compute_type_hash<T>();
    
    return info;
}

/**
 * @brief Validate that a type T matches stored schema.
 * @tparam T Type to validate
 * @param stored_schema Schema info from DataBlock header
 * @return true if type matches schema, false otherwise
 */
template <typename T>
bool validate_type_against_schema(const SchemaInfo& stored_schema) {
    auto current_schema = generate_schema_info<T>();
    return (current_schema.hash == stored_schema.hash &&
            current_schema.size == stored_schema.size &&
            current_schema.alignment == stored_schema.alignment);
}

/**
 * @brief Compare two schema infos for compatibility.
 * @return true if schemas are compatible (same hash/size/alignment)
 */
inline bool schemas_compatible(const SchemaInfo& a, const SchemaInfo& b) {
    return (a.hash == b.hash &&
            a.size == b.size &&
            a.alignment == b.alignment);
}
```

**Factory Functions Extended with Schema:**

```cpp
// In DataBlock factory functions (data_block.hpp)

/**
 * @brief Create schema-aware DataBlock producer.
 * @tparam FlexZoneT Flexible zone structure type
 * @tparam DataBlockT Ring buffer slot structure type
 * @param name DataBlock name
 * @param config Configuration
 * @return Unique pointer to producer
 * 
 * The producer stores schema hashes in the header for runtime validation.
 * Consumers using with_transaction<FlexZoneT, DataBlockT>() will validate
 * their types match at transaction entry.
 */
template <typename FlexZoneT, typename DataBlockT>
std::unique_ptr<DataBlockProducer> create_datablock_producer_with_schema(
    const std::string& name,
    const DataBlockConfig& config
) {
    // Compile-time checks
    static_assert(std::is_trivially_copyable_v<FlexZoneT>);
    static_assert(std::is_trivially_copyable_v<DataBlockT>);
    
    // Generate schemas
    auto flexzone_schema = generate_schema_info<FlexZoneT>();
    auto datablock_schema = generate_schema_info<DataBlockT>();
    
    // Validate sizes fit config
    if (flexzone_schema.size > config.flex_zone_size) {
        throw std::invalid_argument(fmt::format(
            "FlexZoneT size ({}) exceeds configured flex_zone_size ({})",
            flexzone_schema.size, config.flex_zone_size
        ));
    }
    
    if (datablock_schema.size > config.logical_unit_size) {
        throw std::invalid_argument(fmt::format(
            "DataBlockT size ({}) exceeds configured logical_unit_size ({})",
            datablock_schema.size, config.logical_unit_size
        ));
    }
    
    // Create producer with schema info
    return create_datablock_producer_impl(name, config, 
                                         &flexzone_schema, 
                                         &datablock_schema);
}

/**
 * @brief Find and attach to schema-aware DataBlock as consumer.
 * @tparam FlexZoneT Expected flexible zone structure type
 * @tparam DataBlockT Expected ring buffer slot structure type
 * @param name DataBlock name
 * @param expected_config Optional config for validation
 * @return Unique pointer to consumer
 * @throws SchemaMismatchException if types don't match stored schema
 */
template <typename FlexZoneT, typename DataBlockT>
std::unique_ptr<DataBlockConsumer> find_datablock_consumer_with_schema(
    const std::string& name,
    const DataBlockConfig* expected_config = nullptr
) {
    // Compile-time checks
    static_assert(std::is_trivially_copyable_v<FlexZoneT>);
    static_assert(std::is_trivially_copyable_v<DataBlockT>);
    
    // Generate expected schemas
    auto flexzone_schema = generate_schema_info<FlexZoneT>();
    auto datablock_schema = generate_schema_info<DataBlockT>();
    
    // Attach and validate schema
    return find_datablock_consumer_impl(name, expected_config,
                                       &flexzone_schema,
                                       &datablock_schema);
}
```

**User-Facing Example:**

```cpp
// Define data structures (trivially copyable)
struct MyFlexZone {
    std::atomic<uint64_t> producer_count;
    std::atomic<bool> shutdown_flag;
    char status[64];
};

struct MyDataBlock {
    uint64_t timestamp;
    uint32_t sequence;
    uint32_t payload_size;
    char payload[1024];
};

// Create producer with schema validation
auto config = DataBlockConfig{...};
config.flex_zone_size = 4096;  // Must fit MyFlexZone

auto producer = create_datablock_producer_with_schema<MyFlexZone, MyDataBlock>(
    "my_datablock",
    config
);

// Schema info is stored in header automatically
// Consumer MUST use matching types or get runtime error

// Consumer - type-safe attach
auto consumer = find_datablock_consumer_with_schema<MyFlexZone, MyDataBlock>(
    "my_datablock"
);

// Use with transaction - runtime validates types match
producer->with_transaction<MyFlexZone, MyDataBlock>(1000, [](auto& ctx) {
    // Compile-time: types are trivially copyable
    // Runtime: types match stored schema (validated at entry)
    
    ctx.flexzone<MyFlexZone>().producer_count.fetch_add(1);
    
    for (auto& block : ctx.datablock()) {
        auto& data = block.get<MyDataBlock>();  // Type-safe!
        data.timestamp = now();
        ctx.commit();
    }
});
```

**Schema Storage in Header:**

```cpp
// Extend SharedMemoryHeader (in reserved_header or new fields)
struct SharedMemoryHeader {
    // ... existing fields ...
    
    // Schema information (for runtime validation)
    uint8_t flexzone_schema_hash[32];   // BLAKE2b hash of FlexZoneT layout
    uint8_t datablock_schema_hash[32];  // BLAKE2b hash of DataBlockT layout
    uint32_t flexzone_schema_version;   // For remapping
    uint32_t datablock_schema_version;  // For remapping
    char flexzone_schema_name[64];      // Type name for diagnostics
    char datablock_schema_name[64];     // Type name for diagnostics
    
    // ... or store in reserved_header[] if we run out of space ...
};
```

**Phase 3 Deliverables for Schema Validation:**

1. ✅ Extend `generate_schema_info<T>()` to compute type hash
2. ✅ Add compile-time checks (trivially copyable, size fits)
3. ✅ Add runtime validation in `with_transaction()` entry
4. ✅ Provide factory functions with schema: `create_datablock_producer_with_schema<T1, T2>()`
5. ✅ Store schema hashes in header
6. ✅ Validate on consumer attach
7. ✅ Clear error messages on schema mismatch
8. ✅ Documentation and examples

#### 3.5 Complete Removal of Old Layer 2 API

**USER DIRECTIVE: NO gradual migration - remove old API immediately**

Per user feedback: "NO, we don't want this messed up layer which was the reason we are in our current stage."

**Old API to Remove:**
- `with_write_transaction` (free function)
- `with_read_transaction` (free function)
- `with_typed_write` (free function)
- `with_typed_read` (free function)
- `with_next_slot` (free function)
- Old-style `WriteTransactionContext` / `ReadTransactionContext` (if incompatible with new design)

**Strategy:**
1. Implement new API completely in Phase 3
2. Remove old free functions immediately (don't mark deprecated)
3. Update all call sites in same commit
4. Tests will break - that's expected and desired
5. Fix tests in Phase 4 to use new API only

**Rationale:**
- Clean break prevents confusion
- Forces complete migration
- No mixed API usage in codebase
- Easier to reason about correctness

---

### Phase 4: Integration and Testing

**Duration Estimate:** 4-6 hours  
**Prerequisites:** Phase 1, 2, 3 complete  
**Deliverables:** All tests passing, documentation updated

#### 4.1 Test Updates

**Categories:**

1. **Layout Tests** - Already mostly updated in Phase 2
   - Verify single flex zone
   - Verify alignment guarantees
   - Test attach with mismatched layout

2. **Transaction API Tests**
   - Update to new `with_transaction` API
   - Test iterator timeout handling
   - Test Result type error cases
   - Exception safety with new context

3. **Multi-Process Tests**
   - Ensure worker processes use new API
   - Verify heartbeat logic
   - Test graceful shutdown with context

4. **Recovery and Integrity**
   - Validate new layout checksums
   - Test integrity validator with aligned layout
   - Verify recovery APIs still work

#### 4.2 Example Code Updates

**Files:**
- `examples/datahub_producer_example.cpp`
- `examples/datahub_consumer_example.cpp`
- `examples/RAII_LAYER_USAGE_EXAMPLE.md`

**Update to showcase new API:**
```cpp
// New producer example - non-terminating iterator
producer.with_transaction<MyFlexZone, MyDataBlock>(1000, [](auto& ctx) {
    ctx.flexzone<MyFlexZone>().status = Status::Active;
    
    // Iterator blocks and retries - doesn't quit on timeout
    for (auto& block : ctx.datablock()) {
        // Check user-controlled termination
        if (ctx.flexzone<MyFlexZone>().shutdown_requested) {
            break;
        }
        
        // Typed access
        auto& data = block.get<MyDataBlock>();
        data.timestamp = get_timestamp();
        data.payload = produce();
        
        // Or raw access if needed
        // auto raw = block.raw_datablock_access();
        
        ctx.commit();
    }
    
    ctx.flexzone<MyFlexZone>().status = Status::Idle;
});

// Consumer example
consumer.with_transaction<MyFlexZone, MyDataBlock>(1000, [](auto& ctx) {
    for (const auto& block : ctx.datablock()) {
        // Check termination
        if (ctx.flexzone<MyFlexZone>().end_of_stream) {
            break;
        }
        
        // TOCTTOU check
        if (!ctx.validate_read()) {
            continue;
        }
        
        // Process data
        const auto& data = block.get<MyDataBlock>();
        process(data);
    }
});
```

#### 4.6 Test Strategy per Phase

**User Directive:** "tests should be created at the end of each phase - but if existing tests from old design interferes, we can temporarily disable that test."

**Phase 1 Tests:**
- Create: Access function unit tests
- Keep: All existing tests (should still pass with access functions)

**Phase 2 Tests:**
- Create: Single flex zone tests, alignment tests, checksum architecture tests
- Disable: Multi-flex-zone tests (mark with `DISABLED_` prefix)
- Keep: Other layout-independent tests

**Phase 3 Tests:**
- Create: New transaction API tests (context, iterator, typed/raw access)
- Disable: Old Layer 2 API tests (will be updated in Phase 4)
- Keep: Layer 0 (C-API) tests, Layer 1 (primitive) tests

**Phase 4 Tests:**
- Re-enable: All disabled tests, updated to new API
- Full suite: Phase A, B, C, D all passing

**Test Disabling Convention:**
```cpp
// Temporarily disabled - to be updated in Phase 4
TEST(DataBlockTest, DISABLED_OldStyleWithWriteTransaction) {
    // ...
}
```

#### 4.7 Documentation Updates

**Files requiring updates:**
1. `docs/HEP/HEP-CORE-0002-DataHub-FINAL.md`
   - Update C++ abstraction section
   - Update memory layout section

2. `docs/IMPLEMENTATION_GUIDANCE.md`
   - Update layer map
   - Update access function guidance
   - Remove multi-flex-zone references

3. `docs/TODO_MASTER.md` and subtopic TODOs
   - Mark completed tasks
   - Update current focus

4. **Merge design docs:**
   - `DATAHUB_MEMORY_LAYOUT_AND_REMAPPING_DESIGN.md` → HEP + IMPLEMENTATION_GUIDANCE
   - `DATAHUB_CPP_RAII_LAYER_DESIGN_DRAFT.md` → HEP + examples
   - Archive in `docs/archive/transient-2026-02-15/`

#### 4.4 Validation Checklist

Before marking complete:

- [ ] All tests passing (Phase A, B, C, D)
- [ ] No compiler warnings
- [ ] No lint errors (variable naming, braces)
- [ ] Grep confirms no direct header access (except approved access functions)
- [ ] Grep confirms no FlexibleZoneConfig remnants
- [ ] Layout alignment verified (4K data region, 8-byte structured buffer)
- [ ] Examples compile and run
- [ ] Documentation merged and archived
- [ ] Code review completed (use CODE_REVIEW_GUIDANCE.md checklist)

---

## 5. Implementation Order and Dependencies

### 5.1 Dependency Graph

```
Phase 1: C-API Access Functions (3-4h)
    ↓
Phase 2: Memory Layout (4-6h)
    ↓
Phase 3: RAII Layer (6-8h)
    ↓
Phase 4: Integration (4-6h)
```

**Total Estimated Time:** 17-24 hours of focused work

### 5.2 Incremental Checkpoints

Within each phase:

**Phase 1 Checkpoints:**
1. ✓ Metrics access functions implemented
2. ✓ Index access functions implemented
3. ✓ Config access functions implemented
4. ✓ All call sites updated
5. ✓ Tests passing

**Phase 2 Checkpoints:**
1. ✓ FlexibleZoneConfig removed from config
2. ✓ build_flexible_zone_info() removed
3. ✓ Layout calculation updated with alignment
4. ✓ from_header() matches from_config()
5. ✓ Layout tests passing

**Phase 3 Checkpoints:**
1. ✓ Result type implemented
2. ✓ Context types implemented
3. ✓ Iterator implemented
4. ✓ with_transaction() member functions added
5. ✓ Basic transaction test passing

**Phase 4 Checkpoints:**
1. ✓ All tests updated to new API
2. ✓ Examples updated
3. ✓ Documentation merged
4. ✓ Full test suite passing
5. ✓ Code review complete

---

## 6. Risk Assessment and Mitigation

### 6.1 Technical Risks

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| **Access function refactoring introduces bugs** | Medium | High | Systematic replacement with compiler assistance; test after each category |
| **Alignment breaks existing data** | Low | Critical | ABI version bump; incompatible with old segments (acceptable) |
| **New API too different, adoption friction** | Low | Medium | Provide migration guide; keep old API temporarily |
| **Iterator complexity causes bugs** | Medium | Medium | Extensive unit tests; follow C++20 range patterns |
| **Performance regression** | Low | Medium | Benchmark hot paths; zero-cost abstractions verified |

### 6.2 Process Risks

| Risk | Probability | Impact | Mitigation |
|------|-------------|--------|------------|
| **Incomplete refactoring** | Medium | High | Use grep/TODO checklist; compiler errors as guidance |
| **Test coverage gaps** | Low | High | Run full Phase A-D suite; add alignment-specific tests |
| **Documentation drift** | Medium | Low | Update docs in Phase 4; merge design docs immediately |
| **Rollback needed again** | Low | Critical | Commit after each checkpoint; detailed plan (this doc) |

### 6.3 Rollback Strategy

If issues arise:

**After Phase 1:** Can rollback cleanly - access functions are additive  
**After Phase 2:** Cannot rollback - layout incompatible (version bump required)  
**After Phase 3:** Can keep both APIs temporarily if needed  
**After Phase 4:** Should be production-ready; rollback = major version revert

**Recommendation:** Complete Phases 1-3 in one session if possible to minimize partial-state risk.

---

## 7. Success Criteria

### 7.1 Functional Requirements

✅ **C-API Level:**
- All structure access via functions ✓
- Lightweight query API for runtime checks ✓
- Example: `has_any_commits()` used in checksum logic ✓

✅ **Memory Layout:**
- Single flex zone (N×4K) ✓
- 4K-aligned data region ✓
- 8-byte aligned structured buffer ✓
- Layout checksum includes padding ✓

✅ **C++ RAII Layer:**
- Context-centric transaction API ✓
- Iterator with Result type ✓
- Typed and raw access ✓
- Exception-safe, RAII guarantees ✓

✅ **No Layer 1.75:**
- No intermediate abstraction exists ✓
- Clean separation: C-API → Primitive C++ → Transaction API ✓

### 7.2 Quality Requirements

- **Test Coverage:** All Phase A-D tests passing
- **Code Quality:** No lint warnings, all variables ≥3 chars, all statements in braces
- **Documentation:** Design docs merged, examples updated
- **Performance:** No regression in hot paths (acquire/release/commit)

### 7.3 Non-Functional Requirements

- **Maintainability:** Access functions centralized, easy to add/modify
- **Type Safety:** Template API catches errors at compile time
- **ABI Stability:** pImpl pattern maintained, version bump documented
- **Portability:** Works on Linux/Windows, x86/ARM

---

## 8. Open Questions for User

Before proceeding, please confirm:

1. **Phasing:** Do you agree with the 4-phase approach, or would you prefer different breakdown?

2. **API Compatibility:** Should we keep old `with_write_transaction` functions during transition, or replace immediately?

3. **Flex Zone API:** Keep `flexible_zone_span(index)` with index parameter (validate == 0), or remove index entirely?

4. **Access Functions:** Should all metrics access functions be public API (in header), or only exposed internally (in .cpp)?

5. **Testing Priority:** Should we run full test suite after each phase, or only after Phase 4?

6. **Timeline:** Is 17-24 hours of work acceptable, or do we need to find shortcuts?

7. **Checksum Logic Example:** You mentioned checksums calculated when no commits done. Can you point to specific code location so we ensure fix is included?

8. **Current State:** Has any work from the previous (rolled-back) attempt survived? Should we audit for partial changes?

---

## 9. Next Steps

Once you approve this plan (with any modifications):

1. **Start Phase 1:** C-API access function cleanup
   - Create detail namespace with access functions
   - Systematically replace direct header access
   - Add lightweight query APIs
   - Run tests

2. **Proceed to Phase 2:** Memory layout refactoring
   - Single flex zone implementation
   - Alignment fixes
   - Update layout checksum
   - Run tests

3. **Continue through Phases 3-4** as outlined above

**Estimated completion:** 
- Focused: 3-4 working days (8h/day)
- Part-time: 1-2 weeks (4h/day)
- Updated total: ~20-26 hours (added checksum analysis, clarified iterator)

---

## 10. References

- **Design Documents:**
  - `docs/DATAHUB_MEMORY_LAYOUT_AND_REMAPPING_DESIGN.md`
  - `docs/DATAHUB_CPP_RAII_LAYER_DESIGN_DRAFT.md`
  - `docs/HEP/HEP-CORE-0002-DataHub-FINAL.md`

- **Current State:**
  - `docs/IMPLEMENTATION_GUIDANCE.md`
  - `docs/TODO_MASTER.md`
  - `docs/todo/MEMORY_LAYOUT_TODO.md`
  - `docs/todo/RAII_LAYER_TODO.md`

- **Testing:**
  - `cpp/tests/test_layer2_service/`
  - `cpp/tests/test_layer3_datahub/`
  - `docs/README/README_testing.md`

---

**Document Status:** ✅ User Reviewed and Updated  
**Last Updated:** 2026-02-15  
**Next Action:** Begin Phase 1 implementation

## 11. User Feedback Integration Summary

**Phase 2 Additions:**
- ✅ Comprehensive checksum architecture analysis (6 types identified)
- ✅ Lifecycle matrix and validation decision trees
- ✅ Protocol for broker/producer/consumer checksum awareness
- ✅ `has_any_commits()` integration for validation logic
- ✅ Create `CHECKSUM_ARCHITECTURE.md` documentation
- ✅ **Structure re-mapping placeholder API** - designed now for future broker coordination
- ✅ **Validation hooks** - detect schema changes and remapping in progress
- ✅ **Reserved header space** - allocated for remapping state (future use)

**Phase 3 Clarifications:**
- ✅ Iterator accessed via `ctx.datablock()` not `ctx.slots()`
- ✅ Iterator does NOT terminate on timeout/unavailable - blocks and retries
- ✅ Iterator ONLY terminates on error or broker termination
- ✅ `ctx.raw_flexzone_access()` and `block.raw_datablock_access()` for raw memory
- ✅ Complete removal of old Layer 2 API (no gradual migration)
- ✅ **Two-layer type safety**: compile-time (trivially copyable) + runtime (schema validation)
- ✅ **Public schema API**: `generate_schema_info<T>()`, `validate_type_against_schema<T>()`
- ✅ **Schema-aware factories**: `create_datablock_producer_with_schema<T1, T2>()`
- ✅ **Clear error messages** on schema mismatch with type names and hashes

**Metrics and Authorization:**
- ✅ Public metrics snapshot via diagnostic handle
- ✅ Authorization via shared_secret (already in diagnostic handle)
- ✅ Potential future: MessageHub-mediated or broker audit

**Versioning:**
- ✅ Integrated validation at transaction entry
- ✅ ABI version, schema version, layout checksum checks
- ✅ Foundation for future negotiation protocol

**Testing Strategy:**
- ✅ Tests created at end of each phase
- ✅ Disable interfering old tests temporarily
- ✅ Re-enable and update in Phase 4

**Current State Awareness:**
- ✅ Code likely at "Layer 1.75" state (primitive API)
- ✅ Documents (HEP) may have residues from revisions
- ✅ Will detect inconsistencies and confirm with user

**Document Status:** ✅ Ready for Implementation  
**Next Action:** Begin Phase 1 - C-API Access Functions
