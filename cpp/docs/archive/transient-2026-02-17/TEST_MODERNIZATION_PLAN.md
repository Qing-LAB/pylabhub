# DataHub Test Suite Modernization Plan

**Document ID**: PLAN-TEST-001  
**Version**: 1.0.0  
**Date**: 2026-02-15  
**Status**: In Progress

## Executive Summary

The DataHub test suite requires modernization to align with v1.0.0 architecture changes:
- Dual-schema validation (`flexzone_schema_hash` + `datablock_schema_hash`)
- New RAII transaction API (`with_transaction<FlexZoneT, DataBlockT>()`)
- Unified access patterns for checksum, validation, and metrics

##Overview

Test files reviewed: 17 (9 main + 8 workers)  
**Status Breakdown**:
- ✅ OK (foundation level): 5 files
- ⚠️ NEEDS_UPDATE (dual-schema): 4 files
- 🔴 REWRITE (deprecated API): 2 files
- ➕ NEW TESTS NEEDED: Dual-schema validation, RAII layer

---

## Modernization Priorities

### Phase 1: Critical (Blocking Issues) - HIGH PRIORITY

#### 1.1 Rewrite Transaction API Tests
**Files**: `test_transaction_api.cpp`, `workers/transaction_api_workers.cpp`  
**Issue**: Uses deprecated `with_write_transaction()` / `with_read_transaction()`  
**Action**: Rewrite to use `with_transaction<FlexZoneT, DataBlockT>()`

**Effort**: High (complete rewrite)  
**Impact**: Critical (tests wrong API)

#### 1.2 Update Schema Validation Tests
**Files**: `test_schema_validation.cpp`, `workers/schema_validation_workers.cpp`  
**Issue**: Only validates DataBlock schema, not FlexZone  
**Action**: Add dual-schema test cases, verify both hashes

**Effort**: Medium (add new test cases)  
**Impact**: Critical (missing core validation)

---

### Phase 2: Important (Correctness) - MEDIUM PRIORITY

#### 2.1 Add Dual-Schema Tests to BLDS
**Files**: `test_schema_blds.cpp`, `workers/schema_blds_workers.cpp`  
**Issue**: Only tests single-schema generation  
**Action**: Add tests for dual-schema generation and validation

**Test Cases to Add**:
- `GenerateDualSchemaInfo_StoresBothHashes`
- `ValidateDualSchemaMatch_BothMatch`
- `ValidateDualSchemaMatch_FlexZoneMismatch`
- `ValidateDualSchemaMatch_DataBlockMismatch`

**Effort**: Low (add 4 test cases)  
**Impact**: High (verify schema system works)

#### 2.2 Update MessageHub Tests
**Files**: `test_message_hub.cpp`, `workers/messagehub_workers.cpp`  
**Issue**: Broker registration uses single `schema_hash`  
**Action**: Document single-schema limitation or add dual support

**Effort**: Low (documentation or minor update)  
**Impact**: Medium (clarify broker behavior)

#### 2.3 Fix Minor Issues
**File**: `test_phase_a_protocol.cpp`  
**Issue**: Line 121 comment mentions deprecated `flexible_zone_configs`  
**Action**: Fix comment to reference `flex_zone_size`

**Effort**: Trivial  
**Impact**: Low (documentation only)

---

### Phase 3: Enhancement (Coverage) - LOW PRIORITY

#### 3.1 Add RAII Layer Tests
**Files**: New file `test_raii_layer_full.cpp`  
**Purpose**: Comprehensive RAII transaction API testing  
**Coverage**:
- Normal usage (with/without FlexZone)
- Corner cases (timeout, empty FlexZone)
- Error paths (schema mismatch, size mismatch)
- Racing (concurrent transactions)

**Effort**: High (new test file)  
**Impact**: Medium (improve coverage)

#### 3.2 Add Dual-Schema Stress Tests
**Files**: New file `test_dual_schema_stress.cpp`  
**Purpose**: Multiprocess stress testing with dual-schema validation  
**Coverage**:
- Multiple producers with different FlexZone schemas (should fail)
- Multiple consumers validating both schemas
- Schema mismatch under load
- Racing conditions with schema validation

**Effort**: High (new test file + workers)  
**Impact**: Low (stress testing)

#### 3.3 Add Header Layout Verification
**Files**: `test_schema_validation.cpp` (add test)  
**Purpose**: Verify dual-schema header layout  
**Test Cases**:
- `HeaderStoresFlexZoneSchemaHashCorrectly`
- `HeaderStoresDataBlockSchemaHashCorrectly`
- `HeaderSize_Remains4KB_WithDualSchema`

**Effort**: Low (add 3 test cases)  
**Impact**: Medium (verify ABI correctness)

---

## Test Coverage Matrix

### Current Coverage (Before Modernization)

| Category | Normal Usage | Corner Cases | Error Paths | Racing | Documentation |
|----------|-------------|--------------|-------------|--------|---------------|
| C-API Level | ✅ Good | ✅ Good | ✅ Good | ✅ Good | ✅ Good |
| C++ Primitive | ✅ Good | ✅ Good | ✅ Good | ✅ Good | ✅ Good |
| C++ Schema-Aware | ⚠️ Partial | ⚠️ Partial | ⚠️ Partial | ❌ Missing | ⚠️ Partial |
| RAII Layer | 🔴 Wrong API | ❌ Missing | ❌ Missing | ❌ Missing | ⚠️ Partial |
| Dual-Schema | ❌ Missing | ❌ Missing | ❌ Missing | ❌ Missing | ❌ Missing |

### Target Coverage (After Modernization)

| Category | Normal Usage | Corner Cases | Error Paths | Racing | Documentation |
|----------|-------------|--------------|-------------|--------|---------------|
| C-API Level | ✅ Good | ✅ Good | ✅ Good | ✅ Good | ✅ Good |
| C++ Primitive | ✅ Good | ✅ Good | ✅ Good | ✅ Good | ✅ Good |
| C++ Schema-Aware | ✅ Good | ✅ Good | ✅ Good | ✅ Good | ✅ Good |
| RAII Layer | ✅ Good | ✅ Good | ✅ Good | ✅ Good | ✅ Good |
| Dual-Schema | ✅ Good | ✅ Good | ✅ Good | ✅ Good | ✅ Good |

---

## Implementation Strategy

### Strategy 1: Incremental Update (Recommended)

**Approach**: Update tests one phase at a time, keeping build green

**Advantages**:
- Tests remain passing throughout
- Easy to review and validate
- Low risk

**Timeline**:
- Phase 1: 2-3 days
- Phase 2: 1-2 days
- Phase 3: 3-5 days
- **Total**: ~1 week

### Strategy 2: Big Bang Rewrite

**Approach**: Rewrite all tests at once

**Advantages**:
- Cleaner end result
- No deprecated code

**Disadvantages**:
- High risk
- Tests broken during transition
- Harder to review

**Timeline**: 3-5 days (all at once)

**Recommendation**: Use Strategy 1 (Incremental)

---

## Best Practices for Test Modernization

### 1. Test Documentation Standards

Every test must include:

```cpp
/**
 * @test TestName
 * @brief One-line description
 * 
 * @details
 * Detailed explanation of what this test verifies.
 * 
 * @test_strategy
 * For racing/multiprocess tests, describe:
 * - Process/thread roles (producer, consumer, observer)
 * - Sequence of operations (who does what when)
 * - Expected race conditions
 * - How result is verified
 * 
 * @test_level [C-API | C++ Primitive | C++ Schema-Aware | RAII]
 * @coverage [Normal | Corner | Error | Racing]
 * @stress_level [None | Low | Medium | High | Tunable]
 */
```

### 2. Multiprocess Test Documentation Template

```cpp
/**
 * @test_strategy Multiprocess Writer-Reader Race
 * 
 * Process A (Writer):
 *   1. Create producer with schema A
 *   2. Write 100 slots rapidly
 *   3. Verify all writes succeed
 * 
 * Process B (Reader - Compatible):
 *   1. Attach consumer with schema A (match)
 *   2. Read all slots
 *   3. Verify all reads succeed, schemas match
 * 
 * Process C (Reader - Incompatible):
 *   1. Attempt attach with schema B (mismatch)
 *   2. Should fail with SchemaValidationException
 *   3. Verify error message mentions which schema mismatched
 * 
 * Race Condition:
 *   - Process B may attach before Process A writes all slots
 *   - Process C may attempt attach concurrently with Process B
 * 
 * Expected Result:
 *   - Process A: 100 writes succeed
 *   - Process B: 100 reads succeed, all checksums valid
 *   - Process C: Attach fails immediately with clear error
 * 
 * Verification:
 *   - Exit codes: A=0, B=0, C=1 (expected failure)
 *   - Stderr of C contains "FlexZone schema hash mismatch" or "DataBlock schema hash mismatch"
 * 
 * Stress Level: Tunable via STRESS_LEVEL env var (1-10)
 *   - Level 1: 100 slots, 2 readers
 *   - Level 10: 10,000 slots, 20 readers
 */
```

### 3. Error Path Testing Standards

Every error path test must:
- Trigger the error condition explicitly
- Verify exception type (if C++)
- Verify error code (if C-API)
- Verify error message is clear and actionable
- Clean up resources even on error

```cpp
TEST_CASE("SchemaValidation_FlexZoneMismatch_ThrowsSchemaValidationException")
{
    // Setup: Create producer with FlexZoneA
    auto producer = create_datablock_producer<FlexZoneA, Message>(...);
    
    // Error condition: Consumer expects FlexZoneB
    REQUIRE_THROWS_AS(
        find_datablock_consumer<FlexZoneB, Message>(...),
        SchemaValidationException
    );
    
    // Verify error message clarity
    try {
        find_datablock_consumer<FlexZoneB, Message>(...);
        FAIL("Should have thrown");
    } catch (const SchemaValidationException &e) {
        std::string msg = e.what();
        REQUIRE(msg.find("FlexZone schema hash mismatch") != std::string::npos);
        REQUIRE(msg.find("expected") != std::string::npos);
        REQUIRE(msg.find("actual") != std::string::npos);
    }
}
```

### 4. Corner Case Testing Standards

Test boundaries explicitly:
- Zero (empty FlexZone, zero slots)
- One (single slot, single consumer)
- Max (max capacity, max consumers)
- Alignment edges (4K-1, 4K, 4K+1)
- Wraparound (ring buffer full cycle)

```cpp
TEST_CASE("FlexZone_ZeroSize_IsValid")
{
    DataBlockConfig config;
    config.flex_zone_size = 0; // No FlexZone
    
    // Should succeed with void FlexZone type
    auto producer = create_datablock_producer<void, Message>(hub, name, policy, config);
    
    // FlexZone span should be empty
    REQUIRE(producer->flexible_zone_span().empty());
}

TEST_CASE("RingBuffer_WrapAround_At4KBoundary")
{
    config.ring_buffer_capacity = 10;
    config.logical_unit_size = 4096; // Exactly 4K
    
    // Write 20 slots (2x capacity, forces wraparound)
    for (int i = 0; i < 20; ++i) {
        // ...verify wraparound works correctly
    }
}
```

### 5. Racing Condition Testing Standards

Use synchronization primitives:
- Barriers for coordinated start
- Semaphores for sequencing
- Atomic flags for status
- Timeouts for deadlock prevention

```cpp
// Use shared memory for cross-process coordination
struct RaceCoordination {
    std::atomic<int> ready_count{0};
    std::atomic<int> start_flag{0};
    std::atomic<int> error_count{0};
};

void producer_process() {
    // Wait for all processes ready
    coord->ready_count.fetch_add(1);
    while (coord->start_flag.load() == 0) { /* spin */ }
    
    // Now race!
    for (int i = 0; i < 1000; ++i) {
        // ...write slot
    }
}
```

### 6. Stress Level Tuning

Make stress tests configurable:

```cpp
int get_stress_level() {
    const char *env = std::getenv("STRESS_LEVEL");
    return env ? std::atoi(env) : 1; // Default: level 1
}

TEST_CASE("DualSchema_StressTest_MultipleConsumers")
{
    int level = get_stress_level();
    int num_slots = 100 * level;
    int num_consumers = 2 * level;
    int timeout_ms = 1000 + (level * 100);
    
    INFO("Running stress level " << level 
         << ": " << num_slots << " slots, " 
         << num_consumers << " consumers");
    
    // ... test with tunable parameters
}
```

---

## File-by-File Action Plan

### Core DataBlock Tests

#### File: `test_transaction_api.cpp` (REWRITE)

**Priority**: P0 (Critical)  
**Effort**: 4-6 hours  
**Status**: 🔴 Blocking

**Actions**:
1. Replace `with_write_transaction()` with `producer->with_transaction<FlexZoneT, DataBlockT>()`
2. Replace `with_read_transaction()` with `consumer->with_transaction<FlexZoneT, DataBlockT>()`
3. Update `WriteTransactionContext` to `WriteTransactionContext<FlexZoneT, DataBlockT>`
4. Update `ReadTransactionContext` to `ReadTransactionContext<FlexZoneT, DataBlockT>`
5. Add dual-schema test cases
6. Add documentation headers

**Test Cases to Update**:
- `WriteTransaction_AcquireSlot_Success` (line 47)
- `WriteTransaction_Timeout_ThrowsException` (line 57)
- `ReadTransaction_ConsumeSlot_Success` (line 93)
- `ReadTransaction_Iterator_Works` (line 105)
- All other transaction tests (lines 198, 268, 277, 313)

---

### File: `test_schema_validation.cpp` (UPDATE)

**Priority**: P0 (Critical)  
**Effort**: 2-3 hours  
**Status**: ⚠️ Needs dual-schema

**Actions**:
1. Update factory calls to use dual-schema templates
2. Add FlexZone schema mismatch test
3. Add dual-schema validation test
4. Verify both `flexzone_schema_hash` and `datablock_schema_hash`

**New Test Cases**:
- `ConsumerFailsToConnectWithMismatchedFlexZoneSchema`
- `ConsumerFailsToConnectWithMismatchedDataBlockSchema`
- `ConsumerConnectsWithMatchingDualSchema`
- `HeaderStoresBothSchemaHashesCorrectly`

---

### File: `test_schema_blds.cpp` (UPDATE)

**Priority**: P1 (High)  
**Effort**: 1-2 hours  
**Status**: ⚠️ Missing dual-schema tests

**Actions**:
1. Add dual-schema generation test
2. Add dual-schema validation tests
3. Test both hashes independently

**New Test Cases**:
- `GenerateDualSchemaInfo_StoresBothHashes`
- `ValidateDualSchemaMatch_BothMatch`
- `ValidateDualSchemaMatch_FlexZoneMismatch`
- `ValidateDualSchemaMatch_DataBlockMismatch`

---

### File: `test_message_hub.cpp` (DOCUMENT)

**Priority**: P1 (High)  
**Effort**: 30 min  
**Status**: ⚠️ Needs clarification

**Actions**:
1. Document that broker uses single `schema_hash` (DataBlock only)
2. Add comment explaining why (backward compatibility)
3. Add test verifying `schema_hash` = `datablock_schema_hash`

---

### File: `test_phase_a_protocol.cpp` (FIX COMMENT)

**Priority**: P2 (Low)  
**Effort**: 5 min  
**Status**: ⚠️ Outdated comment

**Actions**:
1. Line 121: Change "flexible_zone_configs" to "flex_zone_size"

---

### Files: OK (No Changes Needed)

- `test_slot_protocol.cpp` ✅
- `test_recovery_api.cpp` ✅
- `test_error_handling.cpp` ✅
- `test_datablock_mutex.cpp` ✅

---

## New Test Files to Create

### 1. `test_dual_schema_validation.cpp` (NEW)

**Purpose**: Comprehensive dual-schema validation testing  
**Priority**: P1  
**Effort**: 3-4 hours

**Test Cases**:
- Dual schema storage verification
- Dual schema validation (both must match)
- FlexZone mismatch error path
- DataBlock mismatch error path
- Mixed scenarios (one matches, one doesn't)

### 2. `test_raii_layer_full.cpp` (NEW)

**Purpose**: Complete RAII transaction API testing  
**Priority**: P2  
**Effort**: 4-6 hours

**Test Cases**:
- Normal usage (with/without FlexZone)
- Exception safety
- Resource cleanup on error
- Timeout handling
- Iterator behavior
- Concurrent transactions

---

## Validation Checklist

After modernization, verify:

- [ ] All tests compile without warnings
- [ ] All tests pass
- [ ] No deprecated API usage
- [ ] All dual-schema paths tested
- [ ] All racing tests documented
- [ ] Error paths have clear assertions
- [ ] Corner cases explicitly tested
- [ ] Stress levels are tunable
- [ ] Documentation headers present
- [ ] CI/CD pipeline passes

---

## Timeline Estimate

**Phase 1 (Critical)**: 2-3 days
- Rewrite transaction API tests: 6 hours
- Update schema validation tests: 3 hours
- Testing and validation: 3 hours

**Phase 2 (Important)**: 1-2 days
- Update BLDS tests: 2 hours
- Update MessageHub tests: 1 hour
- Fix minor issues: 30 min
- Testing and validation: 2 hours

**Phase 3 (Enhancement)**: 3-5 days
- Create new RAII layer tests: 6 hours
- Create dual-schema stress tests: 8 hours
- Add header verification tests: 2 hours
- Testing and validation: 4 hours

**Total**: 6-10 days (depends on team size and priority)

---

**Document Control**  
Created: 2026-02-15  
Last Modified: 2026-02-15  
Maintained by: QA Team  
Review Cycle: After each phase completion
