# Test Modernization - Status Update

**Date**: 2026-02-15  
**Status**: In Progress - Compilation Issues Found

## Current Task

Rewriting `test_transaction_api.cpp` and `workers/transaction_api_workers.cpp` to use v1.0.0 API

## Compilation Errors Encountered

### 1. Schema Registry Namespace Issue
**Error**: `PYLABHUB_SCHEMA_BEGIN` must be used at global namespace or in `pylabhub::schema`

**Fix Required**:
```cpp
// WRONG (causes error):
namespace pylabhub::tests::worker::transaction_api {
    PYLABHUB_SCHEMA_BEGIN(TestFlexZone)  // Error: not in schema namespace
    ...
}

// CORRECT (use at global scope or wrap):
PYLABHUB_SCHEMA_BEGIN(pylabhub::tests::worker::transaction_api::TestFlexZone)
    PYLABHUB_SCHEMA_MEMBER(transaction_count)
PYLABHUB_SCHEMA_END(pylabhub::tests::worker::transaction_api::TestFlexZone)
```

### 2. API Signature Mismatch
**Error**: `find_datablock_consumer(..., &config)` expects reference, not pointer

**Fix Required**:
```cpp
// WRONG:
find_datablock_consumer<FlexZone, Message>(hub, name, secret, &config);

// CORRECT (pass by reference):
find_datablock_consumer<FlexZone, Message>(hub, name, secret, config);

// OR (use no-config overload):
find_datablock_consumer<FlexZone, Message>(hub, name, secret);
```

### 3. Iterator Copy Constructor Deleted
**Error**: `SlotIterator` has deleted copy constructor, range-based for fails

**Root Cause**: `Result<T, E>` has deleted copy constructor  
**Investigation Needed**: Check how `raii_layer_example.cpp` uses `ctx.slots()` successfully

### 4. Missing config() Method
**Error**: `DataBlockProducer` doesn't have `config()` method

**Investigation Needed**: How does `TransactionContext::validate_schema()` get config?

### 5. Test Organization (User Request)
User wants tests logically grouped and separated by:
- C-API level vs C++ abstraction level
- Test category (normal, error, racing, stress)
- Clear naming convention

## Recommended Approach

Given multiple API issues discovered, I recommend:

### Option A: Incremental Fix (Recommended)
1. First fix simple issues (namespace, API signature)
2. Investigate iterator and config issues
3. Test one file at a time
4. Then reorganize test structure

### Option B: Reference Existing Tests
1. Find existing tests that successfully use v1.0.0 API
2. Use as template for test_transaction_api rewrite
3. Ensure consistency with working code

### Option C: Defer Test Rewrite
1. Document that old transaction APIs are removed
2. Mark tests as "needs rewrite for v1.0.0"
3. Focus on tests that don't depend on removed APIs
4. Return to transaction tests after other tests are stable

## Questions for User

1. **Do working v1.0.0 transaction tests exist?**  
   - If yes: Use as template
   - If no: Need to debug API usage pattern

2. **Priority for test modernization?**  
   - Fix compilation issues first?
   - Or move to tests that don't need rewrites (schema validation, config tests)?

3. **Test organization - preferred structure?**  
   - Separate directories for C-API vs C++?
   - Or naming convention like `test_c_api_*.cpp` vs `test_cpp_*.cpp`?

## Current Blocker

Cannot proceed with transaction API tests until:
- Iterator usage pattern is clarified (why does example work but test doesn't?)
- Config access in TransactionContext is resolved

## Suggested Next Steps

**Immediate**:
1. Check if `test_raii_layer/test_result.cpp` has working transaction examples
2. Verify if issue is in my test code or in the RAII implementation

**Alternative Path**:
1. Move to `test_schema_validation.cpp` (simpler, no transaction API dependency)
2. Then `test_schema_blds.cpp` (pure schema tests)
3. Return to transaction tests once patterns are clear

## Files Modified So Far

- ✅ `test_layer3_datahub/workers/transaction_api_workers.cpp` (written, has compile errors)
- ⏸️ Other test files (pending)

**Recommendation**: Pause transaction API tests, move to schema validation tests which are simpler and don't depend on transaction API internals.

---

**Status**: BLOCKED - Needs clarification on transaction API usage patterns  
**Next Action**: Wait for user decision on approach
