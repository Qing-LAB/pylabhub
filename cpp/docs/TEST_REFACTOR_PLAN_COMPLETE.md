# Complete Test Refactoring Plan

**Document ID**: TEST-REFACTOR-001  
**Status**: Ready for Execution  
**Date**: 2026-02-15

---

## File-by-File Analysis and Action

### 🔴 DELETE (Wrong Layer / Obsolete)

#### 1. test_phase_a_protocol.cpp + workers/phase_a_workers.cpp
**Tests:** 9 functions - flex zone spans, checksum methods, config agreement  
**Problem:** Testing C++ API behavior at wrong layer, uses removed non-template API  
**Why delete:**
- Checksum correctness → belongs in C API tests
- Span accessors → trivial, covered by integration
- Config agreement → better tested at C++ contract level (new tests)

**Action:** **DELETE** both files  
**Replacement:** See Phase 1-3 below (C API checksum tests, C++ contract tests)

---

### 🟢 KEEP & ENHANCE (C API - Protected)

#### 2. test_recovery_api.cpp + workers/recovery_workers.cpp
**API Level:** C API (extern "C" recovery_api)  
**Status:** 🟢 PROTECTED  
**Tests:** 7 functions - process alive, integrity validation, slot diagnostics, heartbeat  
**Current:** Basic coverage  
**Action:** **KEEP + ENHANCE**
- [ ] Add all error codes
- [ ] Add strategy comments
- [ ] Add stress scenarios (tunable iterations)
- [ ] Verify correctness against current recovery_api.hpp

---

### ⚠️ SPLIT (Mixed C API + Broken C++)

#### 3. test_slot_protocol.cpp + workers/slot_protocol_workers.cpp
**API Level:** MIXED (C API metrics + C++ integration)  
**Tests:** 26 functions - huge file, mixed purposes  
**Current issues:**
- Uses removed non-template API (lines using create_datablock_producer)
- Mixes C API testing (metrics) with C++ integration
- Generic name, unclear organization

**Action:** **SPLIT into 3 files**

##### 3a. Extract → test_c_api_slot_metrics.cpp (NEW, C API, PROTECTED)
**Move these:**
- `writer_timeout_metrics_split()` - Lines 1066+ (uses slot_rw_get_metrics, slot_rw_reset_metrics)
- Any other direct C API slot_rw_* usage

**Focus:** C API correctness
- [ ] All SlotAcquireResult codes
- [ ] Metrics under load
- [ ] Add strategy comments

##### 3b. Transform → test_cpp_slot_integration.cpp (NEW)
**Rewrite these to template API:**
- `write_read_succeeds_in_process()`
- `structured_slot_data_passes()`
- `ring_buffer_iteration_content_verified()`
- `layout_with_checksum_and_flexible_zone_succeeds()`
- `physical_logical_unit_size_used_and_tested()`
- `cross_process_writer/reader` (if still relevant - check if RAII tests cover)

**Focus:** C++ integration - normal usage, data flows correctly

##### 3c. Transform → test_cpp_concurrency.cpp (NEW)
**Rewrite these to template API + add stress:**
- `writer_blocks_on_reader_then_unblocks()` - Contention test
- `high_contention_wrap_around()` - Ring buffer full
- `high_load_single_reader()` - Throughput test
- `policy_latest_only/single_reader/sync_reader()` - Policy behavior under load

**Focus:** C++ concurrency, high contention, tunable stress

**Action:** **SPLIT + REWRITE**

---

### ⚠️ AUDIT & LIKELY REWRITE

#### 4. test_schema_validation.cpp + workers/schema_validation_workers.cpp
**API Level:** C++ Schema  
**Tests:** 2 functions - matching schema succeeds, mismatch fails  
**Current:** Uses old single-schema API?  
**Action:** **AUDIT + REWRITE**
- [ ] Check if uses deprecated single-schema template
- [ ] Rewrite to dual-schema template API
- [ ] Add all mismatch scenarios:
  - FlexZone mismatch, DataBlock match
  - FlexZone match, DataBlock mismatch
  - Both mismatch
  - Producer no schema, consumer expects
  - Major version mismatch
- [ ] Rename to test_cpp_dual_schema_validation.cpp

---

#### 5. test_error_handling.cpp + workers/error_handling_workers.cpp
**API Level:** C++ Primitive  
**Tests:** 9 functions - timeouts, wrong secret, invalid handles, bounds checks  
**Current:** Uses removed non-template API (line 36+ in workers)  
**Focus:** C++ error propagation (already correct focus!)  
**Action:** **REWRITE to template API**
- Good test cases, just need API update
- Keep all 9 tests, switch to template API
- Add exception handling tests:
  - [ ] Lifecycle not initialized → throws with message
  - [ ] Schema mismatch → returns nullptr (not throw)
  - [ ] Checksum failure → error code or nullptr

**Rename:** test_cpp_error_handling.cpp (clarify it's C++ layer)

---

### ✅ LIKELY OK (Verify)

#### 6. test_schema_blds.cpp + workers/schema_blds_workers.cpp
**API Level:** Schema generation (BLDS)  
**Tests:** Schema generation, member encoding  
**Action:** **AUDIT**
- [ ] Verify uses current generate_schema_info API
- [ ] Check if testing both FlexZone and DataBlock schema generation
- [ ] Keep if correct, enhance if missing dual-schema test cases

**Move:** cpp_schema/ directory after audit

---

#### 7. test_transaction_api.cpp + workers/transaction_api_workers.cpp
**API Level:** C++ RAII  
**Tests:** 6 functions - with_transaction success/timeout, exception handling, typed access, iterator  
**Action:** **AUDIT**
- [ ] Check if uses template API correctly (likely yes - has TestFlexZone, TestMessage types)
- [ ] Verify exception safety tests are comprehensive
- [ ] Add if missing: exception in callback, rollback behavior

**Rename:** test_cpp_raii_transaction.cpp  
**Move:** cpp_raii/ directory

---

#### 8. test_message_hub.cpp + workers/messagehub_workers.cpp
**API Level:** MessageHub integration  
**Tests:** 7 functions - lifecycle, not-connected errors, broker happy path  
**Action:** **AUDIT**
- [ ] Verify doesn't use removed API
- [ ] Check if integration tests are still relevant (or covered by other tests)
- [ ] Keep if unique coverage

**Move:** integration/ directory

---

### ❓ UNCLEAR PURPOSE

#### 9. test_datablock_mutex.cpp + workers/datablock_management_mutex_workers.cpp
**API Level:** DataBlockMutex (cross-process mutex)  
**Tests:** 4 functions - acquire/release, zombie recovery, attach failure  
**Current:** Tests a facility class (DataBlockMutex)  
**Action:** **EVALUATE**
- [ ] Is DataBlockMutex still used? (Check data_block.cpp)
- [ ] If yes → **KEEP**, move to facility/ directory, rename test_facility_mutex.cpp
- [ ] If no → **DELETE** (obsolete)
- [ ] If implementation detail → **MERGE** mutex tests into relevant integration tests

---

## New Directory Structure (After Refactoring)

```
tests/test_layer3_datahub/
├── c_api/                                      # C API - PROTECTED
│   ├── test_c_api_checksum.cpp                 # NEW - hash calculation correctness
│   ├── test_c_api_validation.cpp               # NEW - header validation
│   ├── test_c_api_slot_metrics.cpp             # NEW - extracted from slot_protocol
│   ├── test_recovery_api.cpp                   # MOVE + ENHANCE
│   └── workers/
│       ├── c_api_checksum_workers.cpp          # NEW
│       ├── c_api_validation_workers.cpp        # NEW
│       └── recovery_workers.cpp                # MOVE
│
├── cpp_primitive/                              # C++ Primitive API
│   ├── test_cpp_slot_integration.cpp           # NEW - from slot_protocol
│   ├── test_cpp_concurrency.cpp                # NEW - from slot_protocol + new
│   ├── test_cpp_exception_safety.cpp           # NEW
│   ├── test_cpp_error_handling.cpp             # REWRITE from error_handling
│   ├── test_cpp_handle_semantics.cpp           # NEW
│   └── workers/
│       ├── slot_integration_workers.cpp        # SPLIT from slot_protocol_workers
│       ├── concurrency_workers.cpp             # SPLIT from slot_protocol_workers
│       ├── exception_safety_workers.cpp        # NEW
│       └── error_handling_workers.cpp          # REWRITE
│
├── cpp_schema/                                 # C++ Schema API
│   ├── test_cpp_dual_schema_validation.cpp     # REWRITE from schema_validation
│   ├── test_schema_blds.cpp                    # MOVE (if audit passes)
│   └── workers/
│       ├── dual_schema_validation_workers.cpp  # REWRITE
│       └── schema_blds_workers.cpp             # MOVE
│
├── cpp_raii/                                   # C++ RAII Transaction Layer
│   ├── test_cpp_raii_transaction.cpp           # RENAME from transaction_api
│   └── workers/
│       └── raii_transaction_workers.cpp        # RENAME
│
├── facility/                                   # Facility Classes
│   ├── test_facility_mutex.cpp                 # MOVE from datablock_mutex (if kept)
│   └── workers/
│       └── mutex_workers.cpp                   # MOVE
│
├── integration/                                # Integration Tests
│   ├── test_message_hub.cpp                    # MOVE (if audit passes)
│   └── workers/
│       └── messagehub_workers.cpp              # MOVE
│
└── test_framework/                             # KEEP - shared infrastructure
    ├── shared_test_helpers.h
    ├── test_datahub_types.h                    # NEW - shared test types
    └── ...
```

---

## Execution Phases

### Phase 1: C API Foundation (Week 1)
**Goal:** Ensure C API is fully tested and correct

1. **Enhance test_recovery_api.cpp**
   - Add error codes, stress scenarios, strategy comments
   - Move to c_api/ directory

2. **Create test_c_api_checksum.cpp** (NEW)
   ```cpp
   // Pure C API - hash calculation correctness
   TEST(CApi_Checksum, Blake2b_KnownInput_MatchesExpectedHash)
   TEST(CApi_Checksum, StoreInHeader_RetrieveMatches)
   TEST(CApi_Checksum, CorruptData_ValidationFails)
   TEST(CApi_Checksum, NullPointer_ReturnsError)
   ```

3. **Create test_c_api_validation.cpp** (NEW)
   ```cpp
   // Header validation at C API level
   TEST(CApi_Validation, HeaderLayoutHash_CompiledVsStored)
   TEST(CApi_Validation, LayoutChecksum_DetectsTamper)
   TEST(CApi_Validation, SchemaHashComparison_MemcmpCorrect)
   ```

4. **Extract test_c_api_slot_metrics.cpp** from slot_protocol
   - Move C API metric tests (slot_rw_get_metrics, slot_rw_reset_metrics)
   - Add missing error codes
   - Add stress with tunable iterations

### Phase 2: Delete Obsolete (Week 1)
**Goal:** Remove wrong-layer tests

1. **DELETE test_phase_a_protocol.cpp + workers/phase_a_workers.cpp**
   - Document what was removed and why (TEST_REFACTOR_CHANGELOG.md)

2. **Audit test_datablock_mutex.cpp**
   - Check if DataBlockMutex still in use
   - DELETE if obsolete, MOVE to facility/ if kept

### Phase 3: C++ Primitive Tests (Week 2)
**Goal:** Test C++ abstraction correctness, robustness, error handling

1. **Split and rewrite test_slot_protocol.cpp**
   - Extract C API → test_c_api_slot_metrics.cpp (Phase 1)
   - Rewrite integration → test_cpp_slot_integration.cpp
   - Rewrite concurrency → test_cpp_concurrency.cpp
   - DELETE original slot_protocol files

2. **Rewrite test_error_handling.cpp** to template API
   - Keep all 9 tests (good error cases)
   - Add exception tests
   - Rename to test_cpp_error_handling.cpp

3. **Create test_cpp_exception_safety.cpp** (NEW)
   ```cpp
   TEST(CppPrimitive_Exception, WriteHandle_ThrowBeforeCommit_NoData)
   TEST(CppPrimitive_Exception, Destructor_AutoCleanup)
   TEST(CppRaii_Exception, WithTransaction_CallbackThrows_Rollback)
   ```

4. **Create test_cpp_handle_semantics.cpp** (NEW)
   ```cpp
   TEST(CppPrimitive_Handle, MoveConstructor_Transfers)
   TEST(CppPrimitive_Handle, UseAfterMove_Invalid)
   TEST(CppPrimitive_Handle, Destructor_AutoRelease)
   ```

### Phase 4: C++ Schema Tests (Week 2-3)
**Goal:** Dual-schema validation coverage

1. **Rewrite test_schema_validation.cpp**
   - Check current implementation
   - Rewrite to dual-schema template API
   - Add all mismatch scenarios (5+ test cases)
   - Rename to test_cpp_dual_schema_validation.cpp

2. **Audit test_schema_blds.cpp**
   - Verify correctness
   - Move to cpp_schema/ if good
   - Enhance if missing dual-schema cases

### Phase 5: C++ RAII & Integration (Week 3)
**Goal:** Transaction safety, MessageHub

1. **Audit test_transaction_api.cpp**
   - Verify uses template API
   - Check exception coverage
   - Rename to test_cpp_raii_transaction.cpp
   - Move to cpp_raii/

2. **Audit test_message_hub.cpp**
   - Check relevance
   - Move to integration/ if kept

### Phase 6: Reorganization (Week 3-4)
**Goal:** Logical structure, clear naming

1. **Create directory structure**
   - c_api/, cpp_primitive/, cpp_schema/, cpp_raii/, facility/, integration/

2. **Move files** to new locations

3. **Update CMakeLists.txt**
   - Update target names
   - Update test discovery
   - Ensure all tests build

4. **Add strategy comments**
   - Every concurrency test gets detailed strategy comment
   - Document tuning parameters (env vars)

---

## Test Coverage Matrix (After Refactoring)

| Layer | Correctness | Error Paths | Concurrency | Stress | Exception Safety |
|-------|-------------|-------------|-------------|--------|------------------|
| **C API** | ✅ Complete | ✅ All codes | ✅ Tunable | ✅ Tunable | N/A |
| **C++ Primitive** | ✅ Integration | ✅ All propagation | ✅ High contention | ✅ Tunable | ✅ Comprehensive |
| **C++ Schema** | ✅ Dual validation | ✅ All mismatches | N/A | N/A | ✅ Null returns |
| **C++ RAII** | ✅ Transactions | ✅ Callbacks | ✅ Multi-thread | N/A | ✅ Auto rollback |

---

## Files Affected Summary

### DELETE (2 files + 2 workers)
- test_phase_a_protocol.cpp
- workers/phase_a_workers.cpp
- workers/phase_a_workers.h
- (possibly test_datablock_mutex.cpp + workers if obsolete)

### SPLIT (1 file + 1 worker → 3 new files)
- test_slot_protocol.cpp → 3 new test files
- workers/slot_protocol_workers.cpp → 3 new worker files

### REWRITE (2 files + 2 workers)
- test_error_handling.cpp → test_cpp_error_handling.cpp
- test_schema_validation.cpp → test_cpp_dual_schema_validation.cpp

### RENAME & MOVE (3 files + 3 workers)
- test_transaction_api.cpp → cpp_raii/test_cpp_raii_transaction.cpp
- test_schema_blds.cpp → cpp_schema/
- test_message_hub.cpp → integration/

### NEW (7 files + 7 workers)
- test_c_api_checksum.cpp
- test_c_api_validation.cpp
- test_c_api_slot_metrics.cpp (extracted)
- test_cpp_concurrency.cpp
- test_cpp_exception_safety.cpp
- test_cpp_handle_semantics.cpp
- test_cpp_slot_integration.cpp

### ENHANCE (1 file + 1 worker)
- test_recovery_api.cpp (add coverage)

---

## Success Criteria

- [ ] Zero tests using removed non-template API
- [ ] All C API functions tested (100% coverage)
- [ ] All error codes tested
- [ ] Concurrency tests have strategy comments and tunable stress
- [ ] Exception safety verified for all C++ layers
- [ ] Dual-schema validation: all 5+ mismatch scenarios tested
- [ ] Tests organized in logical directories
- [ ] Clear naming: test_<layer>_<component>_<category>.cpp
- [ ] No redundant/duplicate tests
- [ ] CMake builds all tests successfully
- [ ] All tests pass

---

## Change Log Document

Creating TEST_REFACTOR_CHANGELOG.md to track:
- What was deleted and why
- What was split and how
- What was rewritten and rationale
- New tests added and purpose
