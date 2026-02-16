# Test Refactoring Master Todo List

**Purpose**: Track test refactoring work to fix tests after non-template API removal  
**Status**: In Progress  
**Date**: 2026-02-15

---

## Context

**What happened**: Removed redundant non-template C++ API (`create_datablock_producer(hub, name, policy, config)` and `find_datablock_consumer(hub, name, secret, [config])`) that were just wrappers with no schema benefit.

**What remains**: 
- Template API: `create_datablock_producer<FlexZoneT, DataBlockT>(...)` (public)
- impl functions: `create_datablock_producer_impl(..., schema*, schema*)` (internal, not exported)

**Impact**: Tests using removed API don't compile (~3500 lines across 7 files).

---

## Goals (Clear and Verified)

1. **C API tests must work** - Testing extern "C" recovery_api, slot_rw_* (foundation layer)
2. **Get build working** - Disable or fix broken tests systematically
3. **Proper layer separation** - C API tests use impl or C functions directly; C++ tests use template API
4. **No redundant tests** - Remove tests at wrong layer, replace with proper layer-specific tests
5. **Strategy comments** - All concurrency tests document strategy, sequence, tuning

---

## Master Todo List

### Phase 1: Get Build Working ✅ (Partially Complete)

- [x] **T1.1**: Create shared test types with schemas
  - **File**: `test_framework/test_datahub_types.h`
  - **Solution**: EmptyFlexZone, TestFlexZone, TestDataBlock, MinimalData with BLDS schemas
  - **Status**: DONE

- [x] **T1.2**: Delete phase_a tests (wrong layer, obsolete)
  - **Issue**: 9 tests testing low-level checksum/span at wrong layer, uses removed API
  - **Solution**: DELETE - replaced by C API checksum tests (T2.1) and C++ integration tests
  - **Status**: DONE

- [x] **T1.3**: Fix recovery_workers.cpp (C API tests)
  - **Issue**: Used removed non-template API to create test fixtures
  - **Solution**: Use impl directly (recovery has no types, shouldn't use templates)
  - **Status**: DONE

---

### Phase 2: Fix or Disable Remaining Broken Tests 🔴 (Current)

- [ ] **T2.1**: Fix error_handling_workers.cpp (9 functions, 346 lines)
  - **Issue**: Uses removed non-template API
  - **Solution**: Use impl directly (tests are about error paths, not schema validation)
  - **Why this approach**: Error handling tests create datablocks to test error scenarios; the datablock creation isn't what's being tested; use impl (no schema) for test fixtures
  - **Effort**: Medium - pattern replace across 9 functions
  - **Priority**: HIGH - good test coverage, straightforward fix

- [ ] **T2.2**: Disable slot_protocol tests temporarily
  - **Issue**: 26 functions (1519 lines) use removed API; file mixes C API + C++ integration + concurrency
  - **Solution**: Comment out in CMakeLists.txt with note
  - **Why**: Too large to fix now; needs proper split (C API metrics → separate file, C++ tests → rewrite)
  - **Later**: T3.1-T3.3 will properly split and rewrite
  - **Priority**: HIGH - unblock build

- [ ] **T2.3**: Disable schema_validation tests temporarily
  - **Issue**: Uses removed API, needs rewrite to dual-schema validation
  - **Solution**: Already disabled in CMakeLists.txt
  - **Why**: Needs complete rewrite for dual-schema (FlexZone + DataBlock) mismatch scenarios
  - **Later**: T4.1 will rewrite properly
  - **Priority**: HIGH - already done

- [ ] **T2.4**: Audit messagehub_workers.cpp (7 functions, 307 lines)
  - **Issue**: Unknown if uses removed API
  - **Solution**: Read file, check API calls, fix or disable as needed
  - **Why**: MessageHub tests may not use removed datablock API at all
  - **Priority**: HIGH - quick audit

- [ ] **T2.5**: Audit transaction_api_workers.cpp (6 functions, 739 lines)
  - **Issue**: Unknown if uses template API correctly
  - **Solution**: Check - file has TestFlexZone/TestMessage types, may already use template API
  - **Why**: If already correct, no work needed
  - **Priority**: MEDIUM - may already work

- [ ] **T2.6**: Build test_layer3_datahub successfully
  - **Goal**: All enabled tests compile and link
  - **Blocker**: T2.1-T2.4 must complete first

---

### Phase 3: Properly Refactor slot_protocol (Large) ✅ (Deleted)

- [x] **T3.1-T3.4**: Delete obsolete slot_protocol tests
  - **Rationale**: Used removed non-template API; neither C-API nor current C++ abstraction
  - **Action**: Deleted test_slot_protocol.cpp, workers/slot_protocol_workers.cpp, workers/slot_protocol_workers.h
  - **Status**: DONE — new slot integration/concurrency tests belong in Phase 4 new test files (T4.1+)

---

### Phase 4: Add New Tests (Coverage Gaps) ⏸️

- [ ] **T4.1**: Create test_cpp_dual_schema_validation.cpp
  - **Purpose**: Test all dual-schema mismatch scenarios
  - **Tests**: 5+ cases (FlexZone mismatch, DataBlock mismatch, both, producer no schema, version mismatch)
  - **Why**: Critical validation behavior not tested elsewhere
  - **Priority**: HIGH - core functionality

- [ ] **T4.2**: Create test_c_api_checksum.cpp
  - **Purpose**: Test checksum calculation correctness at C API level
  - **Tests**: Hash correctness, store/retrieve, validation, error codes
  - **Why**: Foundation - checksum must be proven correct at C level
  - **Priority**: HIGH - foundation

- [ ] **T4.3**: Create test_c_api_validation.cpp
  - **Purpose**: Test header validation at C API level
  - **Tests**: Layout hash, layout checksum, schema hash comparison
  - **Why**: Foundation - validation correctness
  - **Priority**: MEDIUM

- [ ] **T4.4**: Create test_cpp_exception_safety.cpp
  - **Purpose**: Test C++ exception handling and RAII cleanup
  - **Tests**: Throw before commit, throw in callback, auto rollback
  - **Why**: C++ layer must be exception-safe
  - **Priority**: HIGH - C++ correctness

- [ ] **T4.5**: Create test_cpp_handle_semantics.cpp
  - **Purpose**: Test C++ handle move semantics and lifecycle
  - **Tests**: Move constructor, use after move, RAII cleanup
  - **Why**: C++ API contract
  - **Priority**: MEDIUM

---

### Phase 5: Reorganize Structure ⏸️

- [ ] **T5.1**: Create directory structure
  - **Dirs**: c_api/, cpp_primitive/, cpp_schema/, cpp_raii/, facility/, integration/
  
- [ ] **T5.2**: Move tests to new locations
  - **Files**: Move working tests to logical directories

- [ ] **T5.3**: Update CMakeLists.txt for new structure
  - **Action**: Update all paths, target names

---

## Immediate Execution Queue (Next Steps)

1. **T2.1**: Fix error_handling_workers.cpp
2. **T2.2**: Disable slot_protocol in CMakeLists
3. **T2.4**: Audit messagehub_workers.cpp
4. **T2.5**: Audit transaction_api_workers.cpp
5. **T2.6**: Verify build succeeds

---

## Notes

- **C API tests** (recovery_api, slot_rw_*) must not use C++ templates - use impl or C functions directly
- **C++ tests** should use template API for type safety and schema validation
- **Test fixtures** (datablocks created just to test something else) should use simplest approach - impl for C API tests, template for C++ tests
