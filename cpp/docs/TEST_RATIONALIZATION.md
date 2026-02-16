# Test Rationalization: phase_a_workers.cpp

**Analysis:** What are these tests actually testing, and at what layer should they be?

---

## Current phase_a Tests - Layer Analysis

### Tests 1-2: Flexible Zone Span Accessors
- `flexible_zone_span_empty_when_no_zones()`
- `flexible_zone_span_non_empty_when_zones_defined()`

**What they test:** C++ handle accessors returning span<byte>  
**Layer:** C++ primitive API (handle methods)  
**C API coverage:** Not applicable (C API uses pointers/offsets)  
**Decision:** **KEEP at C++ level** - tests that handles return correct spans based on config  
**Focus:** Normal usage, not contention

---

### Tests 3-4: Checksum Update Methods
- `checksum_flexible_zone_fails_when_no_zone()`
- `checksum_flexible_zone_succeeds_when_zone_exists()`

**What they test:** C++ methods `update_checksum_flexible_zone()`  
**Underlying:** C API checksum functions should be tested separately  
**Layer:** This is testing C++ method behavior (bool return, when it succeeds/fails)  
**C API coverage:** Checksum calculation itself should be at C API level  
**Decision:** 
- **C API:** Add comprehensive checksum tests (correct hash, all cases)
- **C++ level:** Keep simplified version - method returns correct bool, error handling

---

### Tests 5-6: Config Agreement (Flexible Zone Access)
- `consumer_without_expected_config_has_empty_flexible_zone_span()`
- `consumer_with_expected_config_has_nonempty_flexible_zone_span()`

**What they test:** Config agreement contract - consumer without config can't access flex zone  
**Layer:** This is API contract/behavior, not low-level functionality  
**C API coverage:** C API doesn't enforce this (raw pointers)  
**Decision:** **KEEP at C++ level** - tests that C++ API enforces "no config = no access" contract

---

### Test 7: Data Round-Trip
- `config_agreement_data_round_trip_ok()`

**What it tests:** Write data to flex zone, read it back  
**Layer:** Integration test through handles  
**Focus:** Should be about normal usage, not the data itself  
**Decision:** **TRANSFORM** - Focus on:
  - Exception safety during write/read
  - Handle lifecycle (RAII, move)
  - Error handling if operations fail

---

### Tests 8-9: Checksum Policy Enforcement
- `checksum_policy_enforced_write_succeeds()`
- `checksum_policy_enforced_consumer_detects_corruption()`

**What they test:** Checksum policy behavior end-to-end  
**C API coverage:** Checksum calculation tested separately  
**Layer:** This is integration - write with checksum, detect corruption  
**Decision:** **TRANSFORM** - Focus on:
  - C++ exception handling when checksum fails
  - Error code propagation
  - Recovery from checksum mismatch

---

## Recommended Test Strategy

### C API Layer (Comprehensive Coverage)
**Goal:** Prove C API functions are correct

#### Checksum Tests (C API)
- [ ] `test_c_api_checksum_calculation` - Verify hash correctness
- [ ] `test_c_api_checksum_store_retrieve` - Write/read from header
- [ ] `test_c_api_checksum_validation` - Compare stored vs computed
- [ ] `test_c_api_checksum_error_cases` - NULL pointers, invalid sizes

#### Slot RW Tests (C API) - **Already covered in test_slot_rw_coordinator.cpp**
- [x] Acquire/commit/release
- [x] Metrics
- [ ] **Add:** All error codes under load
- [ ] **Add:** Stress test with tunable parameters

#### Recovery API Tests (C API) - **Already covered in test_recovery_api.cpp**
- [x] Basic functionality
- [ ] **Add:** Error path coverage
- [ ] **Add:** Stress scenarios

---

### C++ Abstraction Layer (Correctness, Robustness, Error Handling)

#### Handle Lifecycle and Safety
- [ ] `test_cpp_handle_raii` - Handles clean up on exception
- [ ] `test_cpp_handle_move_semantics` - Move-only handles
- [ ] `test_cpp_handle_expired_access` - Use after producer/consumer destroyed

#### Exception Handling
- [ ] `test_cpp_exception_during_write` - Transaction rollback on throw
- [ ] `test_cpp_exception_in_callback` - RAII layer handles exceptions
- [ ] `test_cpp_checksum_failure_exception` - How C++ reports checksum error

#### High Contention
- [ ] `test_cpp_concurrent_writers` - Multiple threads, single channel
- [ ] `test_cpp_concurrent_readers` - Multiple readers, behavior under load
- [ ] `test_cpp_producer_consumer_stress` - Throughput under contention (tunable)

#### Error Propagation
- [ ] `test_cpp_nullptr_on_schema_mismatch` - Template API returns nullptr, not throw
- [ ] `test_cpp_error_codes_from_handles` - SlotAcquireResult handling
- [ ] `test_cpp_lifecycle_not_initialized` - Exception with clear message

#### Template API Specific
- [ ] `test_cpp_schema_validation_both` - Dual schema validation (FlexZone + DataBlock)
- [ ] `test_cpp_schema_mismatch_scenarios` - All mismatch cases return nullptr
- [ ] `test_cpp_compile_time_checks` - Static asserts for non-trivially-copyable

---

## Action for phase_a_workers.cpp

### Remove/Replace
1. **Remove:** Low-level checksum correctness tests → Move to C API checksum tests
2. **Remove:** Basic span accessor tests → These are trivial, covered by integration
3. **Keep:** Config agreement behavior (C++ API contract)
4. **Transform:** Round-trip test → Exception safety + handle lifecycle test
5. **Transform:** Checksum policy tests → C++ error handling and exception propagation

### New Tests to Add (C++ Focus)
```cpp
// Exception safety during data write
TEST(CppPrimitive_ExceptionSafety, WriteTransaction_ThrowBeforeCommit_NoDataVisible)
TEST(CppPrimitive_ExceptionSafety, WriteTransaction_ThrowAfterCommit_DataVisible)

// High contention
TEST(CppPrimitive_Contention, MultipleWriters_SingleChannel_ProperSerialization)
TEST(CppPrimitive_Contention, ManyReaders_OneFastWriter_NoMissedReads)

// Error handling
TEST(CppPrimitive_ErrorHandling, SchemaMismatch_ReturnsNullptr_NoThrow)
TEST(CppPrimitive_ErrorHandling, ChecksumFailure_ReturnsError_CanRecover)
TEST(CppPrimitive_ErrorHandling, LifecycleNotInit_ThrowsWithMessage)

// Handle semantics
TEST(CppPrimitive_HandleSemantics, MoveConstructor_TransfersOwnership)
TEST(CppPrimitive_HandleSemantics, UseAfterMove_SafeNoOp)
TEST(CppPrimitive_HandleSemantics, RAIICleanup_AutoReleaseOnDestroy)
```

---

## Summary: New Test Philosophy

### C API (test_layer2_*/test_c_api_*)
**Focus:** Correctness and completeness of low-level operations
- Every function tested
- Every error code tested
- Stress with high iteration counts
- No C++ abstractions

### C++ Primitive API (test_cpp_primitive_*)
**Focus:** Handle behavior, error propagation, normal and contention use
- Exception safety
- RAII/move semantics
- Error code → exception translation
- Concurrency correctness
- NOT testing low-level checksums/validation (that's C API)

### C++ Schema API (test_cpp_schema_*)
**Focus:** Template API, schema validation, type safety
- Dual schema validation
- All mismatch scenarios
- Compile-time checks
- Schema generation correctness

### C++ RAII API (test_cpp_raii_*)
**Focus:** Transaction safety, iterator behavior
- Exception safety in transactions
- Iterator non-terminating behavior
- Callback exception handling

---

## Implementation Priority

1. **Ensure C API coverage is complete** (checksum, validation, recovery)
2. **Remove phase_a_workers.cpp** (obsolete, wrong layer)
3. **Add C++ exception/contention tests** (new focus areas)
4. **Add C++ schema validation tests** (dual-schema scenarios)
5. **Reorganize** per new structure

No mechanical translation of phase_a tests needed - we're replacing them with better-targeted tests at the right layers.
