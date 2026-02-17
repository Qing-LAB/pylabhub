# Additional DataHub-Related Tests Outside test_layer3_datahub

**Analysis Date**: 2026-02-15

---

## Summary

Found **5 additional test files** with DataHub/MessageHub/slot references outside test_layer3_datahub:

| File | Layer | Type | Relevance | Action |
|------|-------|------|-----------|--------|
| test_layer2_service/**test_slot_rw_coordinator.cpp** | Layer 2 | **C API** | **HIGH** - Pure C slot API | ✅ **PROTECTED** (already in plan) |
| test_layer2_service/**test_shared_memory_spinlock.cpp** | Layer 2 | Spinlock | **MEDIUM** - Used by DataBlock | Audit (facility test) |
| test_layer2_service/**test_backoff_strategy.cpp** | Layer 2 | Backoff | **LOW** - Utility, may be used in retries | Keep as-is (utils test) |
| test_raii_layer/**test_result.cpp** | RAII | Result<T,E> | **MEDIUM** - Used by RAII layer | Keep (RAII facility) |
| test_framework/**shared_test_helpers.cpp** | Framework | Test utils | **LOW** - Helper functions | Keep (infrastructure) |
| test_layer0_platform/**test_platform_shm.cpp** | Layer 0 | SHM primitives | **LOW** - Platform abstraction | Keep (platform test) |

---

## Detailed Analysis

### 1. test_layer2_service/test_slot_rw_coordinator.cpp
**Status:** ✅ **ALREADY IN REFACTORING PLAN**  
**API Level:** C API (slot_rw_*)  
**Classification:** PROTECTED - Core C API test  
**Action:** Enhance with missing coverage (already documented in plan)

---

### 2. test_layer2_service/test_shared_memory_spinlock.cpp
**API Level:** SharedSpinLock facility  
**Relevance:** DataBlock uses SharedSpinLock for flex zone locking  
**Current Tests:**
- Basic lock/unlock
- Trylock
- Cross-process locking
- Timeout behavior

**Analysis:**
- Tests the **facility** (SharedSpinLock), not DataBlock usage
- Correct placement (layer 2 - service/facility)
- DataBlock's usage of spinlocks is tested via flex zone tests

**Action:** **KEEP AS-IS**
- Good facility test (separate from DataBlock)
- If we reorganize facility tests, could move to facility/ but not urgent
- Already has multiprocess tests

---

### 3. test_layer2_service/test_backoff_strategy.cpp
**API Level:** BackoffStrategy utility  
**Relevance:** May be used in retry logic (MessageHub, recovery?)  
**Current Tests:**
- Backoff timing calculation
- Exponential backoff
- Max attempts

**Analysis:**
- Tests a **utility class**, not directly DataBlock/MessageHub
- Correct layer (service utility)
- May be used by DataBlock timeout/retry logic, but that's tested separately

**Action:** **KEEP AS-IS**
- Utility test, correct layer
- Not part of DataBlock/MessageHub refactoring

---

### 4. test_raii_layer/test_result.cpp
**API Level:** Result<T, E> type (RAII facility)  
**Relevance:** Used by RAII transaction layer return types  
**Current Tests:**
- Result construction
- Ok/Err paths
- Error propagation
- Move semantics

**Analysis:**
- Tests the **Result<T,E> facility** used by RAII layer
- Separate test directory (test_raii_layer/) - makes sense
- RAII transaction tests use Result, this tests the Result type itself

**Action:** **KEEP AS-IS**
- Correct separation: test facility vs test usage
- test_raii_layer/ is appropriate location
- When we create cpp_raii/ for transaction tests, this stays separate (tests the Result type, not transactions)

---

### 5. test_framework/shared_test_helpers.cpp
**API Level:** Test framework infrastructure  
**Relevance:** Helper functions for DataHub tests (make_test_channel_name, cleanup, etc.)  
**Current:**
- Test channel naming
- Cleanup helpers
- Process spawn utilities

**Analysis:**
- **Infrastructure**, not tests
- Used by all DataHub tests
- Correct location (test_framework/)

**Action:** **KEEP AS-IS**
- Infrastructure, not a test
- May need to add helper for shared test types (TestFlexZone, TestDataBlock schemas)
- Add test_datahub_types.h here (new file in plan)

---

### 6. test_layer0_platform/test_platform_shm.cpp
**API Level:** Platform layer (shm_open, shm_unlink, etc.)  
**Relevance:** Low-level primitives used by DataBlock  
**Current Tests:**
- SHM creation/destruction
- Cross-process sharing
- Error handling

**Analysis:**
- Tests **platform abstraction**, not DataBlock
- Correct layer (layer 0 - platform)
- DataBlock builds on these primitives

**Action:** **KEEP AS-IS**
- Platform test, correct layer
- Not part of DataHub refactoring

---

## Updated Test Organization

### DataHub & Related Tests (Complete Picture)

```
tests/
├── test_layer0_platform/
│   └── test_platform_shm.cpp              ✅ Keep - platform primitives
│
├── test_layer2_service/
│   ├── test_slot_rw_coordinator.cpp        ✅ PROTECTED - C API (in refactor plan)
│   ├── test_shared_memory_spinlock.cpp     ✅ Keep - facility test
│   └── test_backoff_strategy.cpp           ✅ Keep - utility test
│
├── test_layer3_datahub/                    🔄 REFACTOR (main focus)
│   ├── c_api/                              (new structure - see main plan)
│   ├── cpp_primitive/
│   ├── cpp_schema/
│   ├── cpp_raii/
│   ├── facility/
│   └── integration/
│
├── test_raii_layer/
│   └── test_result.cpp                     ✅ Keep - Result<T,E> facility
│
└── test_framework/
    ├── shared_test_helpers.cpp             ✅ Keep - infrastructure
    └── test_datahub_types.h                ➕ NEW - shared test types
```

---

## Conclusion

**No additional DataHub/MessageHub test files need refactoring beyond test_layer3_datahub.**

The 5 additional files found are:
- **1 protected C API test** (slot_rw_coordinator) - already in refactoring plan
- **4 support/facility tests** - correctly placed, keep as-is

**All DataHub/MessageHub integration and API tests are in test_layer3_datahub/** - this is the correct organization.

The refactoring plan focuses on the right directory (test_layer3_datahub/), and the related tests in other layers (platform SHM, spinlock, backoff, Result type) are appropriately separated by layer.

---

## No Changes Needed

**Files outside test_layer3_datahub:**
- ✅ test_slot_rw_coordinator.cpp - Already in refactor plan (C API - protected)
- ✅ test_shared_memory_spinlock.cpp - Facility test, correct layer
- ✅ test_backoff_strategy.cpp - Utility test, correct layer  
- ✅ test_result.cpp - RAII facility, correct layer
- ✅ shared_test_helpers.cpp - Infrastructure, correct layer
- ✅ test_platform_shm.cpp - Platform test, correct layer

**All other DataHub tests are in test_layer3_datahub/** - addressed by main refactoring plan.
