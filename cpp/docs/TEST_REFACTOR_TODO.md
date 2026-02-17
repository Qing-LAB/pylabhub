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

### Phase 5: Reorganize Structure and Rename Tests ⏸️

**Goal:** Make `test_layer3_datahub/` follow the same naming and ordering discipline as
`test_layer0_platform/`, `test_layer1_base/`, and `test_layer2_service/`, where:
- Every file is named `test_{module_name}.cpp` — the module under test is immediately obvious.
- Files are ordered by abstraction level: lower layers first, higher layers later.
- The test binary / target name reflects the layer and module.

**Current problem:** `test_layer3_datahub/` mixes levels (C API, primitive, RAII, integration)
in a flat directory with ambiguous names (`test_error_handling.cpp`, `test_recovery_api.cpp`,
`test_policy_enforcement.cpp`) that don't identify which module or which layer they belong to.
There is also a stray `test_raii_layer/` directory that duplicates the layer-3 concern.

---

- [ ] **T5.1**: Define the target file layout following the architecture hierarchy

  The logical order is: **facility → C API → primitive API → RAII layer → integration/scenario**

  ```
  test_layer3_datahub/
    # ── Facility (individual class/subsystem unit tests) ──────────────────────
    test_datahub_schema_blds.cpp          (rename: test_schema_blds.cpp)
    test_datahub_schema_validation.cpp    (rename: test_schema_validation.cpp)
    test_datahub_config_validation.cpp    (new: T4 config error path tests)
    test_datahub_header_structure.cpp     (new: SharedMemoryHeader dual-schema fields)

    # ── C API layer (slot_rw_coordinator, checksum, recovery) ─────────────────
    test_datahub_c_api_slot_protocol.cpp  (rename: test_c_api_slot_protocol.cpp)
    test_datahub_c_api_checksum.cpp       (rename: test_c_api_checksum.cpp)
    test_datahub_c_api_validation.cpp     (new: T4.3 header/layout validation at C level)
    test_datahub_c_api_recovery.cpp       (rename: test_recovery_api.cpp)

    # ── C++ Primitive API (DataBlockProducer/Consumer, no RAII) ───────────────
    test_datahub_producer_consumer.cpp    (rename: test_error_handling.cpp — name now reflects
                                           what is tested: producer/consumer API error paths)
    test_datahub_mutex.cpp                (rename: test_datablock_mutex.cpp)

    # ── C++ RAII layer (TransactionContext, SlotIterator, policies) ───────────
    test_datahub_transaction_api.cpp      (rename: test_transaction_api.cpp)
    test_datahub_policy_enforcement.cpp   (rename: test_policy_enforcement.cpp)
    test_datahub_exception_safety.cpp     (new: T4.4 RAII exception safety)
    test_datahub_handle_semantics.cpp     (new: T4.5 handle move/lifecycle)

    # ── Integration / Scenario (multi-module, end-to-end) ────────────────────
    test_datahub_messagehub.cpp           (rename: test_message_hub.cpp)

    workers/                              (keep; rename worker files to match above)
  ```

  Additionally, absorb `test_raii_layer/test_result.cpp` — move to `test_layer1_base/`
  (Result<T,E> is a generic utility independent of datahub) and delete the `test_raii_layer/`
  directory.

- [ ] **T5.2**: Rename all files and update worker pairs to match

  Each worker file should follow the same `{test_name}_workers.cpp` convention so the
  pairing is always obvious. Example: `test_datahub_c_api_recovery.cpp` ↔
  `workers/datahub_c_api_recovery_workers.cpp`.

  | Current name | New name |
  |---|---|
  | `test_c_api_slot_protocol.cpp` | `test_datahub_c_api_slot_protocol.cpp` |
  | `test_c_api_checksum.cpp` | `test_datahub_c_api_checksum.cpp` |
  | `test_recovery_api.cpp` | `test_datahub_c_api_recovery.cpp` |
  | `test_error_handling.cpp` | `test_datahub_producer_consumer.cpp` |
  | `test_datablock_mutex.cpp` | `test_datahub_mutex.cpp` |
  | `test_schema_blds.cpp` | `test_datahub_schema_blds.cpp` |
  | `test_schema_validation.cpp` | `test_datahub_schema_validation.cpp` |
  | `test_transaction_api.cpp` | `test_datahub_transaction_api.cpp` |
  | `test_policy_enforcement.cpp` | `test_datahub_policy_enforcement.cpp` |
  | `test_message_hub.cpp` | `test_datahub_messagehub.cpp` |

- [ ] **T5.3**: Update CMakeLists.txt — source paths, add_test names, and labels

  - All test names in `add_test(NAME ...)` should reflect their layer, e.g.
    `DatahubCApiSlotProtocol.*`, `DatahubRaii.*`, `DatahubIntegration.*`.
  - Labels: add a `datahub_c_api`, `datahub_raii`, `datahub_facility` label per group so
    `ctest -L datahub_c_api` runs only the C API tier.
  - The convention already used by layer 0–2 test targets (`test_layer0_platform`, etc.)
    should be preserved: the binary is still `test_layer3_datahub`; the internal test names
    and file names carry the disambiguation.

---

## Immediate Execution Queue (Next Steps)

1. **T4.1**: test_cpp_dual_schema_validation.cpp
2. **T4.3**: test_c_api_validation.cpp
3. **T4.4**: test_cpp_exception_safety.cpp
4. **T4.5**: test_cpp_handle_semantics.cpp
5. **Facility gaps**: test_datahub_config_validation.cpp, test_schema_validation dual-schema update
6. **T5.x**: Rename and reorganize (after all new tests are written, so only one restructure pass)

---

## Notes

- **C API tests** (recovery_api, slot_rw_*) must not use C++ templates - use impl or C functions directly
- **C++ tests** should use template API for type safety and schema validation
- **Test fixtures** (datablocks created just to test something else) should use simplest approach - impl for C API tests, template for C++ tests
