# Test Implementation Roadmap

**Goal:** Proper test coverage at each API layer, focusing on what matters at that layer.

---

## Phase 1: C API Foundation (Ensure Complete Coverage)

### 1.1 Audit Existing C API Tests
- [ ] **test_slot_rw_coordinator.cpp** - Add missing coverage:
  - [ ] All SlotAcquireResult error codes
  - [ ] Stress test with env var `SLOT_RW_STRESS_ITERATIONS` (default 1000, high 100000)
  - [ ] Add strategy comments to concurrent test
  - [ ] Test all edge cases (slot full, timeout, etc.)

- [ ] **test_recovery_api.cpp** - Add missing coverage:
  - [ ] All recovery error codes
  - [ ] Stress scenarios (many consumers, zombie cleanup)
  - [ ] Add strategy comments

### 1.2 Add Missing C API Tests

#### New: test_c_api_checksum.cpp
```cpp
// Pure C API checksum tests - no C++ handles
TEST(CApi_Checksum, Blake2b_ComputeHash_CorrectOutput)
TEST(CApi_Checksum, StoreInHeader_RetrieveMatch)
TEST(CApi_Checksum, ValidationFails_OnCorruption)
TEST(CApi_Checksum, NullPointer_ReturnsError)
```

#### New: test_c_api_validation.cpp
```cpp
// Header validation, layout checks - C API level
TEST(CApi_Validation, HeaderLayoutHash_MatchesCompiled)
TEST(CApi_Validation, LayoutChecksum_DetectsMismatch)
TEST(CApi_Validation, SchemaHash_CompareCorrectly)
```

---

## Phase 2: Remove Obsolete Tests

### 2.1 Delete phase_a_workers.cpp
**Reason:** Testing wrong layer, using removed API, not aligned with new architecture  
**Replacement:** C API tests (phase 1) + new C++ tests (phase 3)

### 2.2 Audit and Clean Other Workers
- [ ] **error_handling_workers.cpp** - Check if testing C API errors (keep) or C++ (rewrite)
- [ ] **slot_protocol_workers.cpp** - Split:
  - Extract C API metric tests → keep with test_slot_rw_coordinator.cpp
  - Remove/rewrite C++ portions

---

## Phase 3: Add C++ Abstraction Tests

### 3.1 Exception Safety (High Priority)

#### File: test_cpp_exception_safety.cpp
```cpp
/**
 * TEST STRATEGY: Verify C++ handles and RAII layer properly clean up on exceptions
 * 
 * FOCUS:
 * - Transaction rollback on exception before commit
 * - No dangling resources after exception
 * - Exception propagation preserves error info
 */

TEST(CppPrimitive_Exception, WriteHandle_ThrowBeforeCommit_NoDataVisible)
TEST(CppPrimitive_Exception, WriteHandle_ThrowAfterCommit_DataStillVisible)
TEST(CppPrimitive_Exception, ConsumerDestroy_DuringRead_SafeCleanup)

TEST(CppRaii_Exception, WithTransaction_CallbackThrows_AutoRollback)
TEST(CppRaii_Exception, WithTransaction_ExceptionPropagates_WithContext)
```

### 3.2 Concurrency and Contention (High Priority)

#### File: test_cpp_concurrency.cpp
```cpp
/**
 * TEST STRATEGY: Verify correctness under high contention
 * 
 * TUNING: 
 * - Env var CONCURRENCY_THREAD_COUNT (default 4, high 16)
 * - Env var CONCURRENCY_ITERATIONS (default 1000, high 100000)
 * 
 * SEQUENCE (MultipleWriters test):
 * 1. Spawn N writer threads
 * 2. Each writes M messages with thread ID
 * 3. Reader verifies: all N*M messages received, correct ordering per thread
 * 
 * EXPECTED: No lost messages, no corruption, proper serialization
 */

TEST(CppPrimitive_Concurrency, MultipleWriters_SingleSlot_Serialized)
TEST(CppPrimitive_Concurrency, ManyReaders_OneFastWriter_AllReceive)
TEST(CppPrimitive_Concurrency, ProducerConsumer_HighThroughput_NoLoss)
```

### 3.3 Handle Semantics and Lifecycle

#### File: test_cpp_handle_semantics.cpp
```cpp
TEST(CppPrimitive_Handle, MoveConstructor_TransfersOwnership)
TEST(CppPrimitive_Handle, UseAfterMove_IsInvalid)
TEST(CppPrimitive_Handle, Destructor_AutoRelease)
TEST(CppPrimitive_Handle, AcquireAfterProducerDestroy_Fails)
```

### 3.4 Error Handling and Propagation

#### File: test_cpp_error_handling.cpp
```cpp
TEST(CppPrimitive_Error, LifecycleNotInit_ThrowsException_WithMessage)
TEST(CppPrimitive_Error, SlotAcquireTimeout_ReturnsError_NotThrow)
TEST(CppPrimitive_Error, ChecksumValidationFails_ReturnsNullptr)
TEST(CppPrimitive_Error, InvalidConfig_ThrowsInvalidArgument)

TEST(CppSchema_Error, SchemaMismatch_ReturnsNullptr_NotThrow)
TEST(CppSchema_Error, ProducerNoSchema_ConsumerExpects_ReturnsNullptr)
TEST(CppSchema_Error, MajorVersionMismatch_ReturnsNullptr)
```

### 3.5 Schema Validation (Dual-Schema)

#### File: test_cpp_schema_validation.cpp
```cpp
/**
 * Focus: Template API schema validation behavior
 * NOT testing: Hash calculation correctness (that's C API)
 * Testing: API returns nullptr in all mismatch scenarios
 */

TEST(CppSchema_Validation, BothMatch_Success)
TEST(CppSchema_Validation, FlexZoneMismatch_DataBlockMatch_ReturnsNullptr)
TEST(CppSchema_Validation, FlexZoneMatch_DataBlockMismatch_ReturnsNullptr)
TEST(CppSchema_Validation, BothMismatch_ReturnsNullptr)
TEST(CppSchema_Validation, ProducerNoSchema_ConsumerExpects_ReturnsNullptr)
```

---

## Phase 4: Reorganize Test Structure

### Directory Structure
```
tests/
├── test_layer2_service/
│   ├── test_slot_rw_coordinator.cpp          # C API - PROTECTED
│   └── (existing service tests)
│
├── test_layer3_datahub/
│   ├── c_api/                                 # NEW
│   │   ├── test_c_api_checksum.cpp
│   │   ├── test_c_api_validation.cpp
│   │   └── test_recovery_api.cpp              # MOVE from parent
│   │
│   ├── cpp_primitive/                         # NEW
│   │   ├── test_cpp_exception_safety.cpp
│   │   ├── test_cpp_concurrency.cpp
│   │   ├── test_cpp_handle_semantics.cpp
│   │   ├── test_cpp_error_handling.cpp
│   │   └── workers/
│   │       ├── exception_safety_workers.cpp
│   │       └── concurrency_workers.cpp
│   │
│   ├── cpp_schema/                            # NEW
│   │   ├── test_cpp_schema_validation.cpp
│   │   ├── test_schema_blds.cpp               # MOVE from parent
│   │   └── workers/
│   │       └── schema_validation_workers.cpp
│   │
│   ├── cpp_raii/                              # NEW
│   │   ├── test_raii_transaction.cpp          # RENAME from test_transaction_api.cpp
│   │   └── workers/
│   │       └── raii_workers.cpp               # RENAME from transaction_api_workers.cpp
│   │
│   └── integration/                           # NEW
│       ├── test_message_hub.cpp               # MOVE from parent
│       └── workers/
│           └── messagehub_workers.cpp
```

---

## Phase 5: Add Test Strategy Comments

For every racing/multithread test:
```cpp
/**
 * TEST STRATEGY: <One sentence goal>
 * 
 * SETUP:
 * - <Initial conditions>
 * - <Resource creation>
 * 
 * SEQUENCE:
 * 1. Thread A: <action> → <expected state>
 * 2. Thread B: <action> → <expected state>
 * 3. <Synchronization point>
 * 4. <Validation step>
 * 
 * EXPECTED RESULT:
 * - <What should happen>
 * - <Invariants preserved>
 * 
 * TUNING:
 * - Env var: <NAME> (default <val>, stress <val>)
 * - <What it controls>
 */
```

---

## Implementation Order

1. **Week 1: C API Coverage**
   - Audit test_slot_rw_coordinator.cpp, add missing cases
   - Create test_c_api_checksum.cpp
   - Create test_c_api_validation.cpp

2. **Week 1: Cleanup**
   - Delete phase_a_workers.cpp
   - Delete obsolete tests
   - Document what was removed and why

3. **Week 2: C++ Exception & Error Tests**
   - Create test_cpp_exception_safety.cpp
   - Create test_cpp_error_handling.cpp

4. **Week 2: C++ Concurrency Tests**
   - Create test_cpp_concurrency.cpp
   - Add tunable stress parameters

5. **Week 3: C++ Schema Tests**
   - Create test_cpp_schema_validation.cpp
   - Test all dual-schema mismatch scenarios

6. **Week 3: Reorganization**
   - Create directory structure
   - Move tests to new locations
   - Update CMakeLists.txt

---

## Success Criteria

- [ ] **C API:** 100% function coverage, all error codes tested, stress tests pass
- [ ] **C++ Primitive:** Exception safety verified, concurrency stress passes, handle semantics correct
- [ ] **C++ Schema:** All mismatch scenarios return nullptr, no crashes
- [ ] **C++ RAII:** Transaction safety under exceptions verified
- [ ] **All tests:** Have strategy comments, tunable stress, clear assertions
- [ ] **Organization:** Tests in logical directories, clear naming
- [ ] **No redundancy:** No duplicate test coverage
