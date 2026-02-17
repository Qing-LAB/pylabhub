# Test Audit: Complete File-by-File Analysis

**Document ID**: TEST-AUDIT-002  
**Status**: In Progress  
**Date**: 2026-02-15

---

## Summary Statistics

| Category | Count | Status |
|----------|-------|--------|
| **Total test files** | 18 | Auditing |
| **Protected C API tests** | 4 | Must preserve |
| **Broken (removed API)** | ~5-7 | Must rewrite |
| **Needs audit** | ~7-9 | Check and update |

---

## File-by-File Audit

### 🟢 PROTECTED: C API Tests (DO NOT DELETE)

#### test_layer2_service/test_slot_rw_coordinator.cpp
**API Level:** C API (slot_rw_*)  
**Status:** 🟢 PROTECTED  
**Tests:** 
- `WriterAcquireCommitReleaseSingleThread`
- `ReaderAcquireValidateReleaseSingleThread`
- `ReaderDetectsStaleGeneration`
- `MetricsGetAndReset`
- `ConcurrentWritersAndReaders`

**Audit Needed:**
- [ ] Verify against current SlotRWState structure
- [ ] Check metrics API (slot_rw_get_metrics, slot_rw_reset_metrics)
- [ ] Add missing error code tests
- [ ] Add stress parameter (env var for iteration count)
- [ ] Add strategy comments for concurrent test

**Action:** AUDIT + ENHANCE (add missing coverage, comments)

---

#### test_layer3_datahub/test_recovery_api.cpp
**API Level:** C API (extern "C" recovery_api)  
**Status:** 🟢 PROTECTED  
**Tests:**
- `DatablockIsProcessAlive_ReturnsTrueForSelf`
- `IntegrityValidator_ValidateSucceedsOnCreatedDatablock`
- `SlotDiagnostics_RefreshSucceedsOnCreatedDatablock`
- `SlotRecovery_ReleaseZombieReadersOnEmptySlot`
- `HeartbeatManager_RegistersAndPulses`
- `ProducerUpdateHeartbeat_ExplicitSucceeds`
- `ProducerHeartbeat_AndIsWriterAlive`

**Audit Needed:**
- [ ] Verify recovery_api.hpp extern "C" functions used correctly
- [ ] Check header structure assumptions
- [ ] Add error path tests
- [ ] Add strategy comments

**Action:** AUDIT (verify correctness against current impl)

---

#### test_layer3_datahub/workers/recovery_workers.cpp
**API Level:** C API (recovery_api)  
**Status:** 🟢 PROTECTED  
**Worker implementations for test_recovery_api.cpp**

**Audit Needed:**
- [ ] Same as test_recovery_api.cpp
- [ ] Verify impl calls (uses create_datablock_producer_impl correctly)

**Action:** AUDIT

---

#### test_layer3_datahub/workers/slot_protocol_workers.cpp
**API Level:** MIXED (C API slot_rw_* + C++ create/find)  
**Status:** ⚠️ PARTIAL PROTECTION  
**Lines using C API:** Protected (slot_rw_get_metrics, slot_rw_reset_metrics, slot_rw_state)  
**Lines using removed C++ API:** 🔴 BROKEN

**Audit Needed:**
- [ ] Extract C API usage into separate test file (protect)
- [ ] Rewrite C++ API usage to template API
- [ ] Add strategy comments for racing tests

**Action:** SPLIT + REWRITE
- Extract C API tests → new file `test_c_api_slot_metrics.cpp` (protect)
- Rewrite C++ tests to template API

---

### 🔴 BROKEN: Uses Removed Non-Template API

#### test_layer3_datahub/workers/phase_a_workers.cpp
**API Level:** C++ (non-template - REMOVED)  
**Status:** 🔴 BROKEN - Will not compile  
**Test Count:** 9 worker functions  
**Lines:** 37-40, 78-81, 123-126, 155-158, 190-194, 222-225, 260-263, 295-296, 324-336

**Tests:**
1. `flexible_zone_span_empty_when_no_zones()`
2. `flexible_zone_span_non_empty_when_zones_defined()`
3. `checksum_flexible_zone_fails_when_no_zone()`
4. `checksum_flexible_zone_succeeds_when_zone_exists()`
5. `consumer_without_expected_config_has_empty_flexible_zone_span()`
6. `consumer_with_expected_config_has_nonempty_flexible_zone_span()`
7. `config_agreement_data_round_trip_ok()`
8. `checksum_policy_enforced_write_succeeds()`
9. `checksum_policy_enforced_consumer_detects_corruption()`

**Action:** REWRITE to template API
- Create shared test types (TestFlexZone, TestDataBlock with schemas)
- Replace all `create_datablock_producer(hub, name, policy, config)` 
  with `create_datablock_producer<TestFlexZone, TestDataBlock>(hub, name, policy, config)`
- Same for `find_datablock_consumer`

---

#### test_layer3_datahub/workers/error_handling_workers.cpp
**API Level:** C++ (likely non-template)  
**Status:** 🔴 LIKELY BROKEN  
**Action:** AUDIT + likely REWRITE

---

#### test_layer3_datahub/workers/messagehub_workers.cpp
**API Level:** C++ (check)  
**Status:** ⚠️ CHECK  
**Action:** AUDIT - if uses removed API, rewrite

---

### ⚠️ NEEDS AUDIT

#### test_layer3_datahub/test_schema_validation.cpp
**API Level:** C++ schema  
**Status:** ⚠️ MAY USE OLD SINGLE-SCHEMA API  
**Action:** AUDIT - check for deprecated single-schema template usage

---

#### test_layer3_datahub/test_schema_blds.cpp
**API Level:** Schema generation (BLDS)  
**Status:** ⚠️ LIKELY OK  
**Action:** AUDIT - verify against current generate_schema_info API

---

#### test_layer3_datahub/test_transaction_api.cpp
**API Level:** C++ RAII  
**Status:** ⚠️ CHECK TEMPLATE USAGE  
**Action:** AUDIT - verify uses current dual-schema template API

---

#### test_layer3_datahub/test_slot_protocol.cpp
**API Level:** MIXED  
**Status:** ⚠️ UNCLEAR - generic name  
**Action:** AUDIT - determine what it tests, split if needed

---

#### test_layer3_datahub/test_datablock_mutex.cpp
**API Level:** Unknown  
**Status:** ⚠️ UNCLEAR - generic name  
**Action:** AUDIT - what mutex? Rename or remove if obsolete

---

#### test_layer3_datahub/test_message_hub.cpp
**API Level:** MessageHub integration  
**Status:** ⚠️ LIKELY OK  
**Action:** AUDIT - verify

---

#### test_layer3_datahub/test_error_handling.cpp
**API Level:** Error paths  
**Status:** ⚠️ CHECK  
**Action:** AUDIT - verify API usage

---

## Immediate Action Plan

### Step 1: Create Shared Test Types (Foundation)
**File:** `tests/test_framework/test_datahub_types.h`

```cpp
#pragma once
#include "plh_datahub.hpp"

namespace pylabhub::tests {

// Empty flex zone (use when no flex zone needed)
using NoFlexZone = std::monostate;

// Simple test flex zone
struct TestFlexZone {
    uint64_t counter;
    uint64_t timestamp_ns;
};

// Simple test data block
struct TestDataBlock {
    uint64_t sequence;
    uint64_t value;
    char label[16];
};

// Larger test data for stress
struct LargeTestData {
    uint64_t id;
    char payload[1024];
};

} // namespace pylabhub::tests

// Schema definitions
PYLABHUB_SCHEMA_BEGIN(pylabhub::tests::TestFlexZone)
    PYLABHUB_SCHEMA_MEMBER(counter)
    PYLABHUB_SCHEMA_MEMBER(timestamp_ns)
PYLABHUB_SCHEMA_END(pylabhub::tests::TestFlexZone)

PYLABHUB_SCHEMA_BEGIN(pylabhub::tests::TestDataBlock)
    PYLABHUB_SCHEMA_MEMBER(sequence)
    PYLABHUB_SCHEMA_MEMBER(value)
    PYLABHUB_SCHEMA_MEMBER(label)
PYLABHUB_SCHEMA_END(pylabhub::tests::TestDataBlock)

PYLABHUB_SCHEMA_BEGIN(pylabhub::tests::LargeTestData)
    PYLABHUB_SCHEMA_MEMBER(id)
    PYLABHUB_SCHEMA_MEMBER(payload)
PYLABHUB_SCHEMA_END(pylabhub::tests::LargeTestData)
```

### Step 2: Rewrite phase_a_workers.cpp
**Priority:** HIGH (breaks build)  
**Effort:** Medium (9 tests, straightforward pattern)  
**Template:** Replace all calls with template versions using test types above

### Step 3: Audit and Fix slot_protocol_workers.cpp
**Priority:** HIGH (C API tests must be protected)  
**Action:**
1. Extract C API metric tests → new `test_c_api_slot_metrics.cpp`
2. Rewrite C++ API tests to template

### Step 4: Audit Remaining Test Files
Follow checklist for each file in "NEEDS AUDIT" section

### Step 5: Reorganize
Once all tests work:
1. Create directory structure (c_api/, cpp_primitive/, cpp_schema/, cpp_raii/, facility/)
2. Move/rename tests
3. Update CMakeLists.txt

---

## Test Strategy Comment Template

For racing/multithread/multiprocess tests:

```cpp
/**
 * TEST STRATEGY: <What we're testing>
 * 
 * SETUP:
 * - <Initial conditions>
 * 
 * SEQUENCE:
 * 1. Thread A: <action> - <expected state>
 * 2. Thread B: <action> - <expected state>
 * 3. <synchronization point>
 * 4. <validation>
 * 
 * EXPECTED RESULT:
 * - <What should happen>
 * 
 * TUNING:
 * - Stress level: <env var or parameter>
 * - Iteration count: <how to adjust>
 */
```

---

## Coverage Gaps (Need New Tests)

### Facility Tests (Low-Level, Isolated)
- [ ] Checksum generation and validation (separate from datablock)
- [ ] Header layout schema validation
- [ ] Metrics aggregation
- [ ] Config validation (sizes, alignment)

### Dual-Schema Validation Tests
- [ ] FlexZone mismatch, DataBlock match → returns nullptr
- [ ] FlexZone match, DataBlock mismatch → returns nullptr
- [ ] Both mismatch → returns nullptr
- [ ] Producer no schema, consumer expects schema → returns nullptr
- [ ] Major version mismatch → returns nullptr

### Error Path Coverage
- [ ] All SlotAcquireResult error codes
- [ ] All recovery API error codes
- [ ] Invalid config (sizes, alignment)
- [ ] Lifecycle not initialized

### Racing/Stress Tests (Tunable)
- [ ] Multiple writers, single slot (WRITER_LOCK timeout)
- [ ] Single writer, multiple readers (reader count)
- [ ] Rapid commit/consume (throughput stress)
- [ ] Heartbeat liveness under load

---

## Next Document

Creating detailed rewrite guide for phase_a_workers.cpp...
