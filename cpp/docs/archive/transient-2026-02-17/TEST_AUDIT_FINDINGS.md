# Test Audit: Detailed Findings

**Date**: 2026-02-15

---

## CRITICAL: Tests Using Removed Non-Template API

These tests call the removed non-template wrappers and **will not compile**:

### phase_a_workers.cpp
**Status:** 🔴 **BROKEN** - Uses removed API  
**Lines:** Multiple calls to non-template `create_datablock_producer(hub, channel, policy, config)`  
**Action:** **REWRITE** - Switch to template API with schema types

**Example current code:**
```cpp
auto producer = create_datablock_producer(hub_ref, channel,
                                          DataBlockPolicy::RingBuffer, config);
auto consumer = find_datablock_consumer(hub_ref, channel, config.shared_secret, config);
```

**Should be:**
```cpp
// Define types and schemas
struct EmptyFlexZone {};  // or std::monostate for no flex zone
struct TestData { uint64_t value; };

PYLABHUB_SCHEMA_BEGIN(TestData)
    PYLABHUB_SCHEMA_MEMBER(value)
PYLABHUB_SCHEMA_END(TestData)

auto producer = create_datablock_producer<std::monostate, TestData>(
    hub_ref, channel, DataBlockPolicy::RingBuffer, config);
auto consumer = find_datablock_consumer<std::monostate, TestData>(
    hub_ref, channel, config.shared_secret, config);
```

### error_handling_workers.cpp
**Status:** 🔴 **BROKEN** - Uses removed API  
**Action:** **REWRITE** - Switch to template API

### slot_protocol_workers.cpp  
**Status:** ⚠️ **MIXED** - Some use removed C++ API, some use C API  
**C API usage:** Lines using `slot_rw_*` functions - **PROTECTED, DO NOT CHANGE**  
**C++ API usage:** Lines using non-template create/find - **REWRITE**  
**Action:** Separate C API tests (keep) from C++ tests (rewrite)

### messagehub_workers.cpp
**Status:** ⚠️ **CHECK** - May use removed API  
**Action:** Audit and update if needed

---

## Test Organization Issues

### Generic/Unclear Names
- **test_datablock_mutex.cpp** - What mutex? What aspect of datablocks?
- **test_slot_protocol.cpp** - Too generic, mixes C and C++ API?

### Missing Strategy Comments
Most concurrency/multiprocess tests lack:
- Test strategy (what we're trying to prove)
- Expected sequence/interleaving
- Stress parameters (how to tune)

---

## Next Steps (Immediate)

1. **Audit phase_a_workers.cpp** - Count how many tests, assess rewrite effort
2. **Create template test helper** - Shared test types and schema definitions
3. **Start rewriting** - phase_a tests to template API
4. **Separate C API tests** - Extract from slot_protocol_workers.cpp

Creating detailed per-file audit next...
