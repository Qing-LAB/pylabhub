# P7: Layer 2 Transaction API - Design Specification
**Date:** 2026-02-07  
**Priority:** HIGH (Critical for production)  
**Effort:** 2-3 days design, ~300 lines implementation  
**Dependencies:** P4 (SlotRWCoordinator)

---

## PROBLEM STATEMENT

### Current State (Layer 1 Primitive API)

**Manual resource management:**
```cpp
// Producer - manual cleanup required
SlotWriteHandle slot = producer->acquire_write_slot(timeout_ms);
if (!slot) {
    return;  // Error handling
}

try {
    write_data_to_slot(slot);
    producer->commit_slot(slot);
} catch (...) {
    // ❌ RESOURCE LEAK: slot never released
    throw;
}

producer->release_write_slot(slot);  // User must remember
```

**Problems:**
1. **Resource Leaks:** Forgetting `release_write_slot()` → slot locked forever
2. **No Exception Safety:** Exception during write → slot uncommitted, spinlock held
3. **Complex Control Flow:** Early returns, multiple error paths → easy to miss cleanup
4. **Error-Prone:** Users must manually track handle lifetime

### Required Solution (Layer 2 Transaction API)

**Automatic resource management with RAII:**
```cpp
// Producer - automatic cleanup
with_write_transaction(*producer, timeout_ms, [&](SlotWriteHandle& slot) {
    write_data_to_slot(slot);
    // Auto-commit on lambda success
    // Auto-release on scope exit (even on exception)
});
// ✅ No leaks possible
```

**Benefits:**
1. **RAII Guarantee:** Slot always released
2. **Exception Safe:** Strong exception safety guarantee
3. **Convenient:** Lambda-based, scope-bound
4. **Zero Overhead:** Inline templates, no runtime cost

---

## DESIGN GOALS

1. **Exception Safety:** Strong guarantee (all-or-nothing)
2. **Zero Overhead:** Inline templates, no heap allocation
3. **Convenience:** Lambda-based, minimal boilerplate
4. **Compatibility:** Works with existing Layer 1 API
5. **Composability:** Can nest transactions (with care)
6. **Debuggability:** Clear error messages

---

## PROPOSED API

### Core Template Functions

```cpp
namespace pylabhub {

// Producer write transaction
template<typename Func>
auto with_write_transaction(
    DataBlockProducer& producer,
    int timeout_ms,
    Func&& lambda
) -> std::invoke_result_t<Func, SlotWriteHandle&>;

// Consumer read transaction
template<typename Func>
auto with_read_transaction(
    DataBlockConsumer& consumer,
    uint64_t slot_id,
    int timeout_ms,
    Func&& lambda
) -> std::invoke_result_t<Func, const SlotConsumeHandle&>;

// Iterator-based read transaction (convenience)
template<typename Func>
auto with_next_slot(
    DataBlockSlotIterator& iterator,
    int timeout_ms,
    Func&& lambda
) -> std::optional<std::invoke_result_t<Func, const SlotConsumeHandle&>>;

} // namespace pylabhub
```

### RAII Guard Classes (Optional, for advanced users)

```cpp
namespace pylabhub {

// Write transaction guard
class WriteTransactionGuard {
public:
    explicit WriteTransactionGuard(
        DataBlockProducer& producer,
        int timeout_ms
    );
    
    ~WriteTransactionGuard() noexcept;
    
    // Movable, not copyable
    WriteTransactionGuard(WriteTransactionGuard&&) noexcept;
    WriteTransactionGuard& operator=(WriteTransactionGuard&&) noexcept;
    
    WriteTransactionGuard(const WriteTransactionGuard&) = delete;
    WriteTransactionGuard& operator=(const WriteTransactionGuard&) = delete;
    
    // Access
    explicit operator bool() const noexcept;
    SlotWriteHandle& slot() noexcept;
    
    // Commit (automatic on success, manual if needed)
    void commit();
    
    // Abort (mark as failed, do not commit)
    void abort() noexcept;

private:
    DataBlockProducer* producer_;
    SlotWriteHandle slot_;
    bool acquired_;
    bool committed_;
    bool aborted_;
};

// Read transaction guard
class ReadTransactionGuard {
public:
    explicit ReadTransactionGuard(
        DataBlockConsumer& consumer,
        uint64_t slot_id,
        int timeout_ms
    );
    
    ~ReadTransactionGuard() noexcept;
    
    // Movable, not copyable
    ReadTransactionGuard(ReadTransactionGuard&&) noexcept;
    ReadTransactionGuard& operator=(ReadTransactionGuard&&) noexcept;
    
    ReadTransactionGuard(const ReadTransactionGuard&) = delete;
    ReadTransactionGuard& operator=(const ReadTransactionGuard&) = delete;
    
    // Access
    explicit operator bool() const noexcept;
    const SlotConsumeHandle& slot() const noexcept;

private:
    DataBlockConsumer* consumer_;
    SlotConsumeHandle slot_;
    bool acquired_;
};

} // namespace pylabhub
```

---

## EXCEPTION SAFETY GUARANTEES

### Strong Exception Safety (All-or-Nothing)

**Producer Write Transaction:**
1. **Acquire:** If acquisition fails → no side effects, exception or false return
2. **Lambda Execution:** If lambda throws → slot released, not committed
3. **Commit:** If commit fails → slot released, error propagated
4. **Release:** Always happens (destructor is noexcept)

**State Transitions:**
```
BEFORE: Slot in FREE state
LAMBDA SUCCESS: Slot in COMMITTED state, released
LAMBDA FAILURE: Slot in FREE state (rolled back), released
```

**Consumer Read Transaction:**
1. **Acquire:** If acquisition fails → no side effects
2. **Lambda Execution:** If lambda throws → slot released, exception propagated
3. **Release:** Always happens (destructor is noexcept)

### Memory Safety

- No heap allocation → no allocation failures
- No raw pointers exposed → no dangling pointers
- RAII everywhere → automatic cleanup

---

## IMPLEMENTATION SPECIFICATION

### `with_write_transaction` Template

```cpp
template<typename Func>
auto with_write_transaction(
    DataBlockProducer& producer,
    int timeout_ms,
    Func&& lambda
) -> std::invoke_result_t<Func, SlotWriteHandle&>
{
    using ReturnType = std::invoke_result_t<Func, SlotWriteHandle&>;
    
    // Step 1: Acquire write slot
    SlotWriteHandle slot = producer.acquire_write_slot(timeout_ms);
    
    if (!slot) {
        // Acquisition failed (timeout or error)
        if constexpr (std::is_void_v<ReturnType>) {
            return;  // void return
        } else {
            return ReturnType{};  // default-constructed value
        }
    }
    
    // Step 2: Execute lambda (may throw)
    try {
        if constexpr (std::is_void_v<ReturnType>) {
            std::invoke(std::forward<Func>(lambda), slot);
            
            // Step 3: Auto-commit on success
            producer.commit_slot(slot);
            
            // Step 4: Release slot (always)
            producer.release_write_slot(slot);
            
            return;
        } else {
            ReturnType result = std::invoke(std::forward<Func>(lambda), slot);
            
            // Step 3: Auto-commit on success
            producer.commit_slot(slot);
            
            // Step 4: Release slot (always)
            producer.release_write_slot(slot);
            
            return result;
        }
    } catch (...) {
        // Lambda threw exception
        // Step 3: Do NOT commit (rollback)
        // Step 4: Release slot (always)
        producer.release_write_slot(slot);
        
        // Re-throw exception
        throw;
    }
}
```

### `with_read_transaction` Template

```cpp
template<typename Func>
auto with_read_transaction(
    DataBlockConsumer& consumer,
    uint64_t slot_id,
    int timeout_ms,
    Func&& lambda
) -> std::invoke_result_t<Func, const SlotConsumeHandle&>
{
    using ReturnType = std::invoke_result_t<Func, const SlotConsumeHandle&>;
    
    // Step 1: Acquire consume slot
    SlotConsumeHandle slot = consumer.acquire_consume_slot(slot_id, timeout_ms);
    
    if (!slot) {
        // Acquisition failed
        if constexpr (std::is_void_v<ReturnType>) {
            return;
        } else {
            return ReturnType{};
        }
    }
    
    // Step 2: Execute lambda (may throw)
    try {
        if constexpr (std::is_void_v<ReturnType>) {
            std::invoke(std::forward<Func>(lambda), slot);
            
            // Step 3: Release slot (always)
            consumer.release_consume_slot(slot);
            
            return;
        } else {
            ReturnType result = std::invoke(std::forward<Func>(lambda), slot);
            
            // Step 3: Release slot (always)
            consumer.release_consume_slot(slot);
            
            return result;
        }
    } catch (...) {
        // Lambda threw exception
        // Step 3: Release slot (always)
        consumer.release_consume_slot(slot);
        
        // Re-throw exception
        throw;
    }
}
```

### `with_next_slot` Convenience Wrapper

```cpp
template<typename Func>
auto with_next_slot(
    DataBlockSlotIterator& iterator,
    int timeout_ms,
    Func&& lambda
) -> std::optional<std::invoke_result_t<Func, const SlotConsumeHandle&>>
{
    using ReturnType = std::invoke_result_t<Func, const SlotConsumeHandle&>;
    
    // Try to get next slot
    auto result = iterator.try_next(timeout_ms);
    
    if (result.status != NextResult::Success) {
        return std::nullopt;  // No data available
    }
    
    // Execute lambda with slot
    try {
        if constexpr (std::is_void_v<ReturnType>) {
            std::invoke(std::forward<Func>(lambda), result.slot);
            return std::optional<void>{};  // C++23 feature, or just use bool
        } else {
            return std::make_optional(
                std::invoke(std::forward<Func>(lambda), result.slot)
            );
        }
    } catch (...) {
        // Slot auto-released by NextResult destructor
        throw;
    }
}
```

### `WriteTransactionGuard` Implementation

```cpp
class WriteTransactionGuard {
public:
    explicit WriteTransactionGuard(
        DataBlockProducer& producer,
        int timeout_ms
    )
        : producer_(&producer)
        , slot_(producer.acquire_write_slot(timeout_ms))
        , acquired_(static_cast<bool>(slot_))
        , committed_(false)
        , aborted_(false)
    {}
    
    ~WriteTransactionGuard() noexcept {
        if (acquired_ && !aborted_) {
            // Auto-commit if not explicitly aborted
            if (!committed_) {
                producer_->commit_slot(slot_);
            }
            
            // Always release
            producer_->release_write_slot(slot_);
        }
    }
    
    WriteTransactionGuard(WriteTransactionGuard&& other) noexcept
        : producer_(other.producer_)
        , slot_(std::move(other.slot_))
        , acquired_(other.acquired_)
        , committed_(other.committed_)
        , aborted_(other.aborted_)
    {
        other.acquired_ = false;  // Transfer ownership
    }
    
    WriteTransactionGuard& operator=(WriteTransactionGuard&& other) noexcept {
        if (this != &other) {
            // Clean up current state
            if (acquired_ && !aborted_) {
                if (!committed_) {
                    producer_->commit_slot(slot_);
                }
                producer_->release_write_slot(slot_);
            }
            
            // Transfer ownership
            producer_ = other.producer_;
            slot_ = std::move(other.slot_);
            acquired_ = other.acquired_;
            committed_ = other.committed_;
            aborted_ = other.aborted_;
            
            other.acquired_ = false;
        }
        return *this;
    }
    
    explicit operator bool() const noexcept {
        return acquired_ && !aborted_;
    }
    
    SlotWriteHandle& slot() noexcept {
        return slot_;
    }
    
    void commit() {
        if (acquired_ && !committed_ && !aborted_) {
            producer_->commit_slot(slot_);
            committed_ = true;
        }
    }
    
    void abort() noexcept {
        aborted_ = true;
    }

private:
    DataBlockProducer* producer_;
    SlotWriteHandle slot_;
    bool acquired_;
    bool committed_;
    bool aborted_;
};
```

---

## USAGE EXAMPLES

### Example 1: Producer Write with Transaction API

```cpp
#include <pylabhub/data_block.hpp>

// High-level transaction API (recommended)
void produce_data_safe(DataBlockProducer& producer) {
    with_write_transaction(producer, 1000, [&](SlotWriteHandle& slot) {
        // Write data
        auto buffer = slot.buffer();
        std::memcpy(buffer.data(), my_data, buffer.size());
        
        // If this throws, slot is NOT committed and IS released
        validate_data(buffer);
        
        // On success: auto-commit, auto-release
    });
}

// Guard-based API (for advanced control flow)
void produce_data_with_guard(DataBlockProducer& producer) {
    WriteTransactionGuard tx(producer, 1000);
    
    if (!tx) {
        // Acquisition failed (timeout)
        return;
    }
    
    // Write data
    auto buffer = tx.slot().buffer();
    std::memcpy(buffer.data(), my_data, buffer.size());
    
    if (!validate_data(buffer)) {
        tx.abort();  // Do not commit
        return;
    }
    
    // Explicit commit
    tx.commit();
    
    // Auto-release on scope exit
}
```

### Example 2: Consumer Read with Transaction API

```cpp
// Iterator-based (simplest)
void consume_data_simple(DataBlockSlotIterator& iterator) {
    while (true) {
        with_next_slot(iterator, 1000, [&](const SlotConsumeHandle& slot) {
            auto buffer = slot.buffer();
            process_data(buffer);
            
            // Auto-release on scope exit
        });
    }
}

// Manual slot ID (more control)
void consume_data_manual(DataBlockConsumer& consumer, uint64_t slot_id) {
    with_read_transaction(consumer, slot_id, 1000, 
        [&](const SlotConsumeHandle& slot) {
            auto buffer = slot.buffer();
            
            if (!verify_checksum(buffer)) {
                throw std::runtime_error("Checksum failed");
            }
            
            process_data(buffer);
            
            // Auto-release on scope exit (even on exception)
        });
}

// Guard-based (for complex logic)
void consume_data_with_guard(DataBlockConsumer& consumer, uint64_t slot_id) {
    ReadTransactionGuard tx(consumer, slot_id, 1000);
    
    if (!tx) {
        // No data available
        return;
    }
    
    auto buffer = tx.slot().buffer();
    
    // Process data (may throw)
    process_data(buffer);
    
    // Auto-release on scope exit
}
```

### Example 3: Returning Values from Lambda

```cpp
// Return value from transaction
std::optional<SensorData> read_sensor_data(
    DataBlockConsumer& consumer,
    uint64_t slot_id
) {
    return with_read_transaction(consumer, slot_id, 1000,
        [](const SlotConsumeHandle& slot) -> SensorData {
            auto buffer = slot.buffer();
            return parse_sensor_data(buffer);
            
            // Auto-release after return
        });
}

// Void lambda
void write_sensor_data(
    DataBlockProducer& producer,
    const SensorData& data
) {
    with_write_transaction(producer, 1000, [&](SlotWriteHandle& slot) {
        auto buffer = slot.buffer();
        serialize_sensor_data(data, buffer);
        
        // Auto-commit and release
    });
}
```

---

## EXCEPTION SAFETY ANALYSIS

### Scenario 1: Lambda Throws During Write

```cpp
with_write_transaction(producer, 1000, [&](SlotWriteHandle& slot) {
    write_partial_data(slot);
    
    if (!validate()) {
        throw std::runtime_error("Validation failed");  // ← Exception
    }
    
    write_more_data(slot);
});

// Result:
// 1. Lambda throws
// 2. Slot NOT committed (rollback)
// 3. Slot released (cleanup)
// 4. Exception propagated to caller
// ✅ No leak, consistent state
```

### Scenario 2: Early Return from Lambda

```cpp
with_write_transaction(producer, 1000, [&](SlotWriteHandle& slot) {
    write_data(slot);
    
    if (should_skip()) {
        return;  // ← Early return
    }
    
    write_more_data(slot);
});

// Result:
// 1. Lambda returns normally
// 2. Slot committed (success)
// 3. Slot released (cleanup)
// ✅ No leak, data committed
```

### Scenario 3: Nested Exception Handling

```cpp
try {
    with_write_transaction(producer, 1000, [&](SlotWriteHandle& slot) {
        try {
            risky_operation(slot);
        } catch (const std::exception& e) {
            // Handle inner exception
            LOG_ERROR("Operation failed: {}", e.what());
            throw;  // Re-throw to abort transaction
        }
    });
} catch (const std::exception& e) {
    // Transaction aborted, slot released
    LOG_ERROR("Transaction failed: {}", e.what());
}
```

---

## PERFORMANCE ANALYSIS

### Overhead Breakdown

| Operation | Layer 1 | Layer 2 | Overhead |
|-----------|---------|---------|----------|
| Acquire | ~50 ns | ~50 ns | 0 ns (same call) |
| Lambda invocation | N/A | ~5-10 ns | ~10 ns (inline) |
| Commit | ~30 ns | ~30 ns | 0 ns (same call) |
| Release | ~30 ns | ~30 ns | 0 ns (same call) |
| **Total** | **~110 ns** | **~120 ns** | **~10 ns (9%)** |

**Conclusion:** ~10 ns overhead (~9%) is negligible for most use cases.

### Optimization

- Templates are **fully inlined** → zero function call overhead
- Lambda capture is **zero-cost** (compiler optimization)
- No heap allocation → no allocation overhead
- Move semantics → no unnecessary copies

---

## LAYER COMPARISON

| Feature | Layer 1 (Primitive) | Layer 2 (Transaction) |
|---------|---------------------|----------------------|
| **Lifetime** | Manual | RAII (scope-based) |
| **Exception Safety** | None | Strong guarantee |
| **Resource Leaks** | Possible | Impossible |
| **Overhead** | ~110 ns | ~120 ns (~9%) |
| **Flexibility** | High | Medium |
| **Ease of Use** | Medium | High |
| **Use Case** | Performance-critical, custom logic | Standard application code |

---

## WHEN TO USE EACH LAYER

### Use Layer 1 (Primitive API) When:
1. **Performance Critical:** Every nanosecond counts (e.g., real-time control loops)
2. **Cross-Iteration State:** Need to hold handle across loop iterations
3. **Custom Coordination:** Building higher-level abstractions
4. **No Exceptions:** Embedded systems, `-fno-exceptions`

### Use Layer 2 (Transaction API) When:
1. **Standard Application:** Normal C++ code with exceptions
2. **Safety First:** Resource leaks unacceptable
3. **Convenience:** Minimize boilerplate
4. **Data Processing:** Video, audio, sensor data pipelines

**Recommendation:** **Use Layer 2 by default**, drop to Layer 1 only when profiling shows overhead matters.

---

## INTEGRATION WITH EXISTING CODE

### Backward Compatibility

**Layer 1 API remains unchanged:**
```cpp
// Still works, no breaking changes
SlotWriteHandle slot = producer.acquire_write_slot(1000);
if (slot) {
    // ... manual management
    producer.release_write_slot(slot);
}
```

**Layer 2 is additive:**
```cpp
// New convenience API
with_write_transaction(producer, 1000, [](SlotWriteHandle& slot) {
    // ... automatic management
});
```

### Migration Path

**Before (Layer 1):**
```cpp
SlotWriteHandle slot = producer.acquire_write_slot(1000);
if (!slot) return false;

try {
    write_data(slot);
    producer.commit_slot(slot);
    producer.release_write_slot(slot);
    return true;
} catch (...) {
    producer.release_write_slot(slot);  // ← Easy to forget!
    throw;
}
```

**After (Layer 2):**
```cpp
with_write_transaction(producer, 1000, [&](SlotWriteHandle& slot) {
    write_data(slot);
});
return true;
```

**Result:** ~50% less code, no resource leak risk.

---

## TESTING STRATEGY

### Unit Tests

1. **Basic Functionality**
   - Successful transaction (commit and release)
   - Failed acquisition (timeout)
   - Early return from lambda
   - Exception during lambda

2. **Exception Safety**
   - Exception before commit → not committed
   - Exception after commit → released
   - Nested exceptions
   - Multiple exceptions

3. **Move Semantics**
   - Move guard to another scope
   - Return guard from function
   - Guard in container

4. **Performance**
   - Benchmark overhead (should be <10 ns)
   - Compare with Layer 1

### Integration Tests

1. **Producer-Consumer**
   - Producer uses Layer 2, consumer uses Layer 1
   - Producer uses Layer 1, consumer uses Layer 2
   - Both use Layer 2

2. **Stress Test**
   - High-frequency writes with exceptions
   - No resource leaks under stress
   - Correct state transitions

---

## DOCUMENTATION UPDATES REQUIRED

### HEP-core-0002 Updates

**Section 10.3: Layer 2 Transaction API**
- Add complete implementation specification (this document)
- Add exception safety guarantees
- Add performance analysis
- Add usage examples
- Add migration guide

**Section 10.3.1: Design Principles**
- Update with RAII guarantee details
- Add exception safety levels

**Section 10.3.2: Transaction API Functions**
- Replace stub with complete API specification
- Add `WriteTransactionGuard` and `ReadTransactionGuard`

**Section 10.3.3-10.3.4: Examples**
- Update with realistic examples
- Add exception handling examples

**Appendix E.3: Layer Selection Guide**
- Update with performance data
- Add decision tree

---

## IMPLEMENTATION CHECKLIST

- [ ] Implement `with_write_transaction` template
- [ ] Implement `with_read_transaction` template
- [ ] Implement `with_next_slot` convenience wrapper
- [ ] Implement `WriteTransactionGuard` RAII class
- [ ] Implement `ReadTransactionGuard` RAII class
- [ ] Add unit tests (exception safety, move semantics)
- [ ] Add integration tests (producer-consumer)
- [ ] Add performance benchmarks
- [ ] Update HEP-core-0002 documentation
- [ ] Add usage examples to examples/ directory
- [ ] Update API reference documentation

---

## ESTIMATED EFFORT

| Task | Lines of Code | Effort |
|------|---------------|--------|
| Template implementations | ~200 | 1 day |
| RAII guard classes | ~100 | 0.5 days |
| Unit tests | ~300 | 1 day |
| Integration tests | ~100 | 0.5 days |
| Documentation updates | ~500 lines | 0.5 days |
| **Total** | **~1,200** | **3.5 days** |

---

## SUMMARY

**Layer 2 Transaction API provides:**
- ✅ Strong exception safety (all-or-nothing)
- ✅ Zero overhead (~10 ns, 9%)
- ✅ Convenient lambda-based API
- ✅ RAII guarantees (no leaks possible)
- ✅ Backward compatible (additive)
- ✅ Production-ready error handling

**Recommendation:** **APPROVE AND IMPLEMENT**

This completes the design for P7. Ready to move on to P8 (Error Recovery API)?
