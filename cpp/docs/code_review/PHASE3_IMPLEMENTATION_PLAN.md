# Phase 3: C++ RAII Layer Implementation Plan

**Date:** 2026-02-15  
**Status:** Planning → Ready to Begin  
**Based On:** `DATAHUB_CPP_RAII_LAYER_DESIGN_DRAFT.md`

---

## Overview

Phase 3 implements a unified, type-safe C++ RAII layer with:
- Context-centric transaction API (`with_transaction<FlexZoneT, DataBlockT>`)
- Non-terminating iterators for slot acquisition
- Compile-time + runtime schema validation
- Raw memory access as an opt-in capability
- Hybrid automatic/explicit heartbeat

---

## Implementation Strategy

### Approach: Bottom-Up, Incremental
1. **Build foundational types first** (Result, SlotRef, ZoneRef)
2. **Add context layer** (TransactionContext with validation)
3. **Implement iterator semantics** (non-terminating, Result-based)
4. **Integrate with existing Producer/Consumer**
5. **Add schema validation hooks**
6. **Create examples and tests**

### Key Design Note
Per the design document (§1.2):
- **Layer 1.75 (SlotRWAccess)** → **REMOVED** (redundant with C API)
- **Layer 2 (Transaction API)** → **REPLACED** by `with_transaction` member
- **Guards** → **KEPT** but `commit()` → `mark_success()`

---

## Phase 3 Task Breakdown

### **Phase 3.1: Implement Result<T, E> Type** 
**Priority:** Critical (Foundation)  
**Estimated Complexity:** Low  
**Files:**
- Create: `src/include/utils/result.hpp`

**Requirements:**
```cpp
enum class SlotAcquireError {
    Timeout,  // Timed out waiting for slot
    NoSlot,   // No slot available (non-blocking)
    Error     // Fatal error (unrecoverable)
};

template <typename T, typename E>
class Result {
public:
    static Result ok(T value);
    static Result error(E err, int code = 0);
    
    [[nodiscard]] bool is_ok() const noexcept;
    [[nodiscard]] bool is_error() const noexcept;
    
    T& value();              // throws if error
    const T& value() const;  // throws if error
    E error() const;         // returns error enum
    int error_code() const;  // returns detailed code
    
private:
    std::variant<T, std::pair<E, int>> m_data;
};

// Convenience alias
template <typename SlotRefT>
using SlotAcquireResult = Result<SlotRefT, SlotAcquireError>;
```

**Tests:**
- Result construction (ok/error)
- Value access (success/failure)
- Error code retrieval
- Move semantics

---

### **Phase 3.2: Implement SlotRef**
**Priority:** Critical (Foundation)  
**Estimated Complexity:** Medium  
**Files:**
- Create: `src/include/utils/slot_ref.hpp`
- Modify: `src/include/utils/data_block.hpp` (forward declarations)

**Requirements:**
```cpp
template <typename DataBlockT, bool IsMutable>
class SlotRef {
public:
    // Typed access
    using value_type = std::conditional_t<IsMutable, DataBlockT, const DataBlockT>;
    [[nodiscard]] value_type& get();
    
    // Raw access (opt-in)
    [[nodiscard]] std::span<std::byte> raw_access() requires IsMutable;
    [[nodiscard]] std::span<const std::byte> raw_access() const;
    
    // Slot metadata
    [[nodiscard]] uint64_t slot_id() const noexcept;
    [[nodiscard]] size_t slot_index() const noexcept;
    
private:
    // Wraps existing SlotWriteHandle or SlotConsumeHandle
    std::variant<SlotWriteHandle*, SlotConsumeHandle*> m_handle;
    
    static_assert(std::is_trivially_copyable_v<DataBlockT>,
                  "DataBlockT must be trivially copyable for shared memory");
};

// Type aliases
template <typename T> using WriteSlotRef = SlotRef<T, true>;
template <typename T> using ReadSlotRef = SlotRef<T, false>;
```

**Design Notes:**
- Wraps existing `SlotWriteHandle`/`SlotConsumeHandle`
- `.get()` validates size, returns typed reference
- `.raw_access()` returns full slot span
- Compile-time checks for trivial copyability

**Tests:**
- Typed access (correct type, size validation)
- Raw access (correct span)
- Const correctness
- Trivial copyability enforcement

---

### **Phase 3.3: Implement ZoneRef**
**Priority:** Critical (Foundation)  
**Estimated Complexity:** Medium  
**Files:**
- Create: `src/include/utils/zone_ref.hpp`

**Requirements:**
```cpp
template <typename FlexZoneT, bool IsMutable>
class ZoneRef {
public:
    // Typed access
    using value_type = std::conditional_t<IsMutable, FlexZoneT, const FlexZoneT>;
    [[nodiscard]] value_type& get();
    
    // Raw access (opt-in)
    [[nodiscard]] std::span<std::byte> raw_access() requires IsMutable;
    [[nodiscard]] std::span<const std::byte> raw_access() const;
    
    // Zone index
    [[nodiscard]] size_t index() const noexcept;
    
private:
    DataBlockProducer* m_producer = nullptr;  // or Consumer*
    size_t m_index = 0;
    
    static_assert(std::is_trivially_copyable_v<FlexZoneT>,
                  "FlexZoneT must be trivially copyable for shared memory");
};

// Type aliases
template <typename T> using WriteZoneRef = ZoneRef<T, true>;
template <typename T> using ReadZoneRef = ZoneRef<T, false>;
```

**Design Notes:**
- Phase 2 already simplified to single flex zone (index always 0)
- Design allows future multi-zone support via index parameter
- `.get()` validates size, returns typed reference
- `.raw_access()` returns full zone span

**Special Case:**
- `ZoneRef<void, ...>` for no-flexzone mode (returns empty span)

**Tests:**
- Typed access with size validation
- Raw access
- Void specialization (no flexzone)
- Bounds checking (when index != 0)

---

### **Phase 3.4: Implement TransactionContext**
**Priority:** Critical (Core API)  
**Estimated Complexity:** High  
**Files:**
- Create: `src/include/utils/transaction_context.hpp`
- Create: `src/utils/transaction_context.cpp`

**Requirements:**
```cpp
template <typename FlexZoneT, typename DataBlockT, bool IsWrite>
class TransactionContext {
public:
    // Flexible zone access
    auto flexzone() -> std::conditional_t<IsWrite,
                                          WriteZoneRef<FlexZoneT>,
                                          ReadZoneRef<FlexZoneT>>;
    
    // Slot iterator (non-terminating)
    auto slots(std::chrono::milliseconds timeout) -> SlotIterator<DataBlockT, IsWrite>;
    
    // Commit (producer only)
    void commit() requires IsWrite;
    
    // Validation (consumer only)
    [[nodiscard]] bool validate_read() const requires (!IsWrite);
    
    // Optional heartbeat convenience
    void update_heartbeat();
    
    // Context metadata
    [[nodiscard]] const DataBlockConfig& config() const noexcept;
    [[nodiscard]] const DataBlockLayout& layout() const noexcept;
    
private:
    // Entry validation (constructor)
    void validate_schema();
    void validate_layout();
    void validate_checksums();
    
    // Holds Producer* or Consumer*
    std::variant<DataBlockProducer*, DataBlockConsumer*> m_handle;
    
    // Current slot state
    std::optional<std::variant<SlotWriteHandle, SlotConsumeHandle>> m_current_slot;
};

// Type aliases
template <typename F, typename D>
using WriteTransactionContext = TransactionContext<F, D, true>;

template <typename F, typename D>
using ReadTransactionContext = TransactionContext<F, D, false>;
```

**Entry Validation (Constructor):**
1. Schema validation (if schema registered)
2. Layout validation (sizeof checks)
3. Checksum policy enforcement
4. Flex zone size validation

**Tests:**
- Context creation with valid schema
- Schema mismatch detection
- Size mismatch detection
- Checksum policy enforcement
- Flexzone access
- Commit semantics

---

### **Phase 3.5: Implement ctx.slots() Non-Terminating Iterator**
**Priority:** Critical (Core API)  
**Estimated Complexity:** High  
**Files:**
- Create: `src/include/utils/slot_iterator.hpp`
- Create: `src/utils/slot_iterator.cpp`

**Requirements:**
```cpp
template <typename DataBlockT, bool IsWrite>
class SlotIterator {
public:
    using value_type = SlotAcquireResult<SlotRef<DataBlockT, IsWrite>>;
    
    // Iterator interface (C++20 ranges)
    SlotIterator& operator++();              // Acquire next slot
    value_type operator*() const;            // Current result
    [[nodiscard]] bool operator==(std::default_sentinel_t) const;
    
    // Range interface
    SlotIterator begin();
    std::default_sentinel_t end() const;
    
private:
    TransactionContext<?, DataBlockT, IsWrite>* m_ctx;
    std::chrono::milliseconds m_timeout;
    value_type m_current_result;
    bool m_done = false;  // Only true on fatal error
};
```

**Iterator Semantics:**
- **Non-terminating**: Continues indefinitely unless fatal error
- **Yields Result**: Each iteration yields `Result<SlotRef, SlotAcquireError>`
- **Timeout handling**: Returns `Result::error(Timeout)`, does not end iteration
- **NoSlot handling**: Returns `Result::error(NoSlot)`, does not end iteration
- **User breaks**: User checks flexzone flags, events, etc. and breaks explicitly

**Tests:**
- Successful slot acquisition
- Timeout handling (continues iteration)
- NoSlot handling (continues iteration)
- Fatal error (ends iteration)
- User break conditions

---

### **Phase 3.6: Implement with_transaction<FlexZoneT, DataBlockT>()**
**Priority:** Critical (Public API)  
**Estimated Complexity:** Medium  
**Files:**
- Modify: `src/include/utils/data_block.hpp` (add member functions)
- Modify: `src/utils/data_block.cpp` (implement)

**Requirements:**
```cpp
class DataBlockProducer {
public:
    // ... existing API ...
    
    // New: Type-safe transaction API
    template <typename FlexZoneT, typename DataBlockT, typename Func>
    requires std::invocable<Func, WriteTransactionContext<FlexZoneT, DataBlockT>&>
    auto with_transaction(std::chrono::milliseconds timeout, Func&& func)
        -> std::invoke_result_t<Func, WriteTransactionContext<FlexZoneT, DataBlockT>&>;
};

class DataBlockConsumer {
public:
    // ... existing API ...
    
    // New: Type-safe transaction API
    template <typename FlexZoneT, typename DataBlockT, typename Func>
    requires std::invocable<Func, ReadTransactionContext<FlexZoneT, DataBlockT>&>
    auto with_transaction(std::chrono::milliseconds timeout, Func&& func)
        -> std::invoke_result_t<Func, ReadTransactionContext<FlexZoneT, DataBlockT>&>;
};
```

**Implementation Steps:**
1. Create TransactionContext with validation
2. Invoke user lambda with context reference
3. Handle exceptions (ensure cleanup)
4. Return lambda result

**Exception Safety:**
- Context RAII ensures cleanup on throw
- Current slot released automatically
- Heartbeat updated on exit

**Tests:**
- Basic transaction flow
- Exception handling
- Lambda return values
- Type enforcement (compile-time)
- Schema validation (runtime)

---

### **Phase 3.7: Add Runtime Schema Validation Hooks**
**Priority:** High (Type Safety)  
**Estimated Complexity:** Medium  
**Files:**
- Modify: `src/include/utils/data_block.hpp` (add schema storage)
- Modify: `src/utils/data_block.cpp` (validation logic)

**Requirements:**
- Store `schema::SchemaInfo` in Producer/Consumer pImpl
- Add schema parameter to factory functions
- Validate at transaction entry:
  - `sizeof(FlexZoneT)` == registered flex zone schema size
  - `sizeof(DataBlockT)` == registered datablock schema size
- Throw `SchemaMismatchException` on mismatch

**Design Note:**
- Schema validation is **optional** (only when schema registered)
- Size-only validation initially (full BLDS integration later)

**Tests:**
- Schema match (success)
- Schema mismatch (throws)
- No schema registered (success, size checked only)

---

### **Phase 3.8: Implement Hybrid Heartbeat**
**Priority:** Medium (Liveness)  
**Estimated Complexity:** Low  
**Files:**
- Modify: `src/include/utils/data_block.hpp` (add methods)
- Modify: `src/utils/data_block.cpp` (implement)

**Requirements:**
```cpp
class DataBlockProducer {
public:
    // Explicit heartbeat (when idle)
    void update_heartbeat();
};

class DataBlockConsumer {
public:
    // Heartbeat management
    void register_heartbeat();
    void update_heartbeat();
    void unregister_heartbeat();
};
```

**Automatic Heartbeat:**
- Already implemented in existing slot release paths
- No additional work needed

**Explicit APIs:**
- `update_heartbeat()`: Update timestamp in header/heartbeat table
- Consumer: `register`/`unregister` for slot allocation

**Tests:**
- Automatic heartbeat on slot operations
- Explicit heartbeat updates
- Heartbeat expiration detection

---

### **Phase 3.9: Create Comprehensive Usage Examples**
**Priority:** Medium (Documentation)  
**Estimated Complexity:** Low  
**Files:**
- Create: `cpp/examples/raii_layer_producer_example.cpp`
- Create: `cpp/examples/raii_layer_consumer_example.cpp`
- Create: `cpp/examples/raii_layer_flexzone_only_example.cpp`

**Examples to Cover:**
1. **Basic Producer/Consumer** with typed access
2. **Flexzone coordination** (no datablocks)
3. **Raw memory access** after validation
4. **Timeout handling** in iterator
5. **Schema validation** examples
6. **Heartbeat management** patterns

---

### **Phase 3.10: Build Test Suite for RAII Layer**
**Priority:** High (Verification)  
**Estimated Complexity:** High  
**Files:**
- Create: `cpp/tests/test_raii_layer/test_result_type.cpp`
- Create: `cpp/tests/test_raii_layer/test_slot_ref.cpp`
- Create: `cpp/tests/test_raii_layer/test_zone_ref.cpp`
- Create: `cpp/tests/test_raii_layer/test_transaction_context.cpp`
- Create: `cpp/tests/test_raii_layer/test_slot_iterator.cpp`
- Create: `cpp/tests/test_raii_layer/test_with_transaction.cpp`
- Create: `cpp/tests/test_raii_layer/test_schema_validation.cpp`

**Test Coverage:**
- Unit tests for each component
- Integration tests for full transaction flow
- Error handling (exceptions, Result errors)
- Schema validation
- Timeout behavior
- Raw access

---

## Implementation Order (Recommended)

### **Week 1: Foundation Types**
1. ✅ Result<T, E> type
2. ✅ SlotRef<T, IsMutable>
3. ✅ ZoneRef<T, IsMutable>
4. Unit tests for above

### **Week 2: Context & Iterator**
5. ✅ TransactionContext<F, D, IsWrite>
6. ✅ SlotIterator<D, IsWrite>
7. Integration tests

### **Week 3: Public API**
8. ✅ with_transaction<F, D>() members
9. ✅ Schema validation hooks
10. ✅ Heartbeat APIs
11. Full integration tests

### **Week 4: Examples & Documentation**
12. ✅ Usage examples
13. ✅ Test suite
14. ✅ Documentation updates

---

## Dependencies & Prerequisites

### Phase 2 Completion ✅
- [x] Single flex zone design
- [x] 4K-aligned memory layout
- [x] Checksum architecture
- [x] Placeholder re-mapping APIs

### C++20 Features Required
- Concepts (`std::invocable`, `requires`)
- Ranges (for iterator)
- `std::span`
- `std::variant`
- `std::optional`

### Current Codebase
- Producer/Consumer with internal mutex (thread-safe)
- SlotWriteHandle/SlotConsumeHandle for slot access
- Existing flex zone APIs

---

## Migration Strategy

### Backward Compatibility
- **Keep existing Layer 2 APIs** during transition
- **Mark as deprecated** in documentation
- **Remove Layer 1.75** (SlotRWAccess) - already redundant

### Transition Path
1. **Add new RAII layer** alongside existing APIs
2. **Update examples** to use new API
3. **Deprecate old APIs** after stabilization
4. **Remove old APIs** in next major version

---

## Success Criteria

- [ ] All Phase 3 tasks completed
- [ ] Full test suite passing
- [ ] Examples compile and run
- [ ] Documentation updated
- [ ] No regression in existing functionality
- [ ] Performance within 5% of current implementation

---

## Risk Assessment

### High Risk
- **Iterator semantics**: Non-terminating iterators are non-standard
- **Template complexity**: Heavy template usage may impact compile times

### Medium Risk
- **Schema validation**: Integration with BLDS schema system
- **Exception safety**: Ensuring proper cleanup in all paths

### Low Risk
- **Result type**: Well-understood pattern
- **SlotRef/ZoneRef**: Thin wrappers over existing handles

---

## Next Steps

1. Review this plan with stakeholders
2. Begin Phase 3.1 (Result type implementation)
3. Iterative development with tests
4. Regular integration with main codebase

---

**Status:** Ready to begin Phase 3.1 ✅  
**Estimated Total Duration:** 3-4 weeks for full implementation  
**Current Phase:** Planning Complete → Implementation Pending
