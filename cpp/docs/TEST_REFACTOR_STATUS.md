# Test Refactoring: Current Status and Blockers

**Date**: 2026-02-15  
**Status**: BLOCKED - Many files need API updates

---

## What Was Done

### ✅ Completed
1. **Created shared test types** (`test_framework/test_datahub_types.h`)
   - EmptyFlexZone, TestFlexZone, TestDataBlock, MinimalData, LargeTestData, etc.
   - All with BLDS schema definitions

2. **Deleted phase_a tests** (obsolete, wrong layer)
   - test_phase_a_protocol.cpp
   - workers/phase_a_workers.cpp
   - workers/phase_a_workers.h

3. **Updated CMakeLists.txt** - Removed phase_a references

4. **Fixed recovery_workers.cpp** - Uses impl directly (no templates - correct for C API tests)

---

## Current Build Errors

**20+ errors in remaining test files** - all using removed non-template API:

| File | Errors | API Usage | Action Needed |
|------|--------|-----------|---------------|
| **slot_protocol_workers.cpp** | ~15+ | Uses removed API in 26 test functions | **REWRITE or DISABLE** |
| **error_handling_workers.cpp** | ~9 | Uses removed API in 9 test functions | **REWRITE** |
| **messagehub_workers.cpp** | Unknown | Check | **AUDIT** |

---

## Problem: Scope Too Large for Mechanical Fix

**slot_protocol_workers.cpp alone has 1419 lines, 26 test functions.**

Three options:

### Option 1: Disable All Broken Tests, Fix Later
**Pros:** Unblocks build immediately, can fix incrementally  
**Cons:** Loses test coverage during refactoring

```cmake
# CMakeLists.txt - disable broken files
# test_error_handling.cpp  # DISABLED - needs template API rewrite
# workers/error_handling_workers.cpp
# test_slot_protocol.cpp  # DISABLED - needs split + rewrite
# workers/slot_protocol_workers.cpp
```

### Option 2: Fix Critical Files Only
**Pros:** Keeps some coverage, focused effort  
**Cons:** Still significant work

1. Fix error_handling (9 tests) - straightforward, all use same pattern
2. Disable slot_protocol temporarily (too large, needs split)
3. Build succeeds, most tests work

### Option 3: Fix All Now (Large Effort)
**Pros:** Complete, all tests work  
**Cons:** 35+ test function rewrites, high risk of errors

---

## Recommendation: Option 2 (Fix Critical, Disable Large)

### Immediate Actions:
1. **Fix error_handling_workers.cpp** (9 functions, straightforward pattern)
2. **Disable slot_protocol** temporarily (comment out in CMakeLists)
3. **Audit messagehub_workers.cpp** (small file)
4. **Build succeeds**, recovery + error_handling + messagehub tests work

### Later (Separate Task):
- Split slot_protocol into 3 files (c_api metrics, cpp integration, cpp concurrency)
- Rewrite each part properly with strategy comments

---

## Files Still Need Work (After Option 2)

| File | Status | Lines | Effort | Priority |
|------|--------|-------|--------|----------|
| slot_protocol_workers.cpp | DISABLED | 1419 | HIGH | Later - needs split |
| schema_validation_workers.cpp | DISABLED | ~100 | MEDIUM | Later - rewrite dual-schema |
| messagehub_workers.cpp | CHECK | ~250 | LOW | Now - audit |
| transaction_api_workers.cpp | CHECK | ~680 | MEDIUM | Later - audit template usage |
| datablock_management_mutex_workers.cpp | CHECK | ~180 | LOW | Later - evaluate if needed |

---

## Decision Point

**Which option do you want:**
1. **Disable all broken tests** - fastest, fix incrementally
2. **Fix error_handling only** - keeps some coverage, defers large file (recommended)
3. **Fix everything now** - complete but high effort

**My recommendation: Option 2** - Fix error_handling (small, useful), disable slot_protocol (large, needs proper split anyway), get build working, then tackle slot_protocol refactoring as a focused task with proper split and strategy comments.
