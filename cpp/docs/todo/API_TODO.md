# API TODO

**Purpose:** Track public API refinements, documentation improvements, and API surface enhancements for DataHub.

**Master TODO:** `docs/TODO_MASTER.md`  
**API Reference:** `cpp/src/include/utils/data_block.hpp`  
**Examples:** `cpp/examples/`

---

## Current Focus

### API Documentation Gaps
**Status**: 🟡 In Progress

- [ ] **Consumer registration to broker** – `MessageHub::register_consumer` is a stub, protocol not yet defined
- [ ] **stuck_duration_ms in diagnostics** – `SlotDiagnostic::stuck_duration_ms` requires timestamp on acquire
- [ ] **DataBlockMutex documentation** – Factory vs direct constructor, exception vs optional/expected
- [ ] **Flexible zone initialization** – Document when flexible_zone_info is populated

### API Consistency
**Status**: 🟢 Ready

- [x] **release_write_slot** – Documented return values and idempotent behavior
- [x] **Slot handle lifetime** – Contract documented in data_block.hpp
- [x] **Recovery error codes** – All codes documented in recovery_api.hpp
- [ ] **Error code consistency** – Review all APIs for consistent error reporting

---

## Public API Surface

### Core DataBlock API
```cpp
// Factory functions
std::unique_ptr<DataBlockProducer> create_datablock_producer(...)
std::unique_ptr<DataBlockConsumer> find_datablock_consumer(...)

// Producer API
class DataBlockProducer {
    std::unique_ptr<SlotWriteHandle> acquire_write_slot(int timeout_ms);
    bool release_write_slot(SlotWriteHandle& handle);
    void update_heartbeat();
    SharedSpinLock get_spinlock(size_t index);  // For flexible zones
    size_t spinlock_count() const noexcept;
    // ... metrics, diagnostics
};

// Consumer API
class DataBlockConsumer {
    std::unique_ptr<SlotConsumeHandle> acquire_consume_slot(int timeout_ms);
    std::unique_ptr<SlotConsumeHandle> acquire_consume_slot(uint64_t slot_id, int timeout_ms);
    bool release_consume_slot(SlotConsumeHandle& handle);
    DataBlockSlotIterator slot_iterator();
    // ... metrics, diagnostics
};

// Handles
class SlotWriteHandle;   // RAII, destroyed before producer
class SlotConsumeHandle; // RAII, destroyed before consumer
```

### Recovery and Diagnostics API
```cpp
// Diagnostics
DataBlockDiagnosticHandle open_datablock_for_diagnostic(const std::string& name);
class SlotDiagnostics;
class IntegrityValidator;

// Recovery
class SlotRecovery;
class HeartbeatManager;
bool datablock_is_process_alive(uint64_t pid);
RecoveryResult datablock_validate_integrity(...);
```

---

## Backlog

### API Enhancements
- [ ] **Config builder pattern** – Fluent API for DataBlockConfig construction
- [ ] **Error callbacks** – Register callbacks for specific error conditions
- [ ] **Flexible zone by name** – Access flexible zones by string name instead of index
- [ ] **Batch operations** – Read/write multiple values efficiently
- [ ] **Async API** – Non-blocking variants with futures/promises (if use case emerges)

### Configuration API
- [ ] **Config validation helpers** – Pre-validate config before creation
- [ ] **Config templates** – Named configs for common patterns (e.g., "single_writer", "high_throughput")
- [ ] **Config explicit-fail test** – Test that creation throws with invalid config

### Diagnostics API
- [ ] **Structured diagnostics** – Return diagnostic info as structured data, not just logs
- [ ] **Health check API** – Single call to check if DataBlock is healthy
- [ ] **Performance metrics** – Expose throughput, latency, contention metrics

### Recovery API Improvements
- [ ] **Integrity repair path** – Low-level repair using only DiagnosticHandle
- [ ] **Graceful degradation** – API for dealing with partially corrupted blocks
- [ ] **Recovery policies** – Configurable recovery behavior (aggressive vs conservative)

---

## API Design Principles

### 1. Error Handling Strategy
- **C API**: Return error codes, no exceptions
- **C++ API**: Throw for contract violations, return nullptr/false for expected failures
- **Recovery API**: Return `RecoveryResult` enum

### 2. Lifetime and Ownership
- **Factories** return `unique_ptr` (exclusive ownership)
- **Handles** are move-only, destroyed before owner
- **Guards** are move-only, RAII, noexcept destructors

### 3. Thread Safety
- **Producer/Consumer**: Thread-safe (internal mutex)
- **Handles**: Not thread-safe, use from one thread
- **C API**: No locking, caller's responsibility

### 4. Noexcept Marking
- Destructors: Always noexcept
- Simple accessors: noexcept if no throw
- Acquisition: Not noexcept (can fail)
- See IMPLEMENTATION_GUIDANCE.md § Explicit noexcept

---

## Documentation Tasks

### API Reference
- [ ] **Doxygen coverage** – Ensure all public APIs have complete documentation
- [ ] **Parameter descriptions** – Document all parameters, return values, exceptions
- [ ] **Usage examples** – At least one example per major API
- [ ] **Thread safety notes** – Document thread safety for each class

### User Guides
- [ ] **Getting started guide** – Simple producer/consumer example
- [ ] **Configuration guide** – All config options explained
- [ ] **Error handling guide** – How to handle failures at each API level
- [ ] **Migration guide** – From C API to C++, from primitive to transaction API

### Examples
- [ ] **Modernize producer example** – Use latest transaction API
- [ ] **Modernize consumer example** – Use iterator and transaction API
- [ ] **Add recovery example** – Show how to detect and recover from errors
- [ ] **Add flexible zone example** – Show typed flexible zone usage

---

## API Stability

### Breaking Changes (Major Version)
Track breaking changes for future major version bump:
- Removing Layer 1.75 (SlotRWAccess) ✅ Done in v1.0
- Config validation (require explicit parameters) ✅ Done in v1.0
- Structured buffer alignment change (compatibility break) - Planned for v2.0

### Deprecation Candidates
None currently. Maintain stable API for v1.x.

### Experimental APIs
Mark clearly as experimental, subject to change:
- Flexible zone by-name access (when added)
- Async API variants (when added)
- Batch operations (when added)

---

## Related Work

- **RAII Layer** (`docs/todo/RAII_LAYER_TODO.md`) – Transaction API is part of public API
- **Testing** (`docs/todo/TESTING_TODO.md`) – API surface needs comprehensive tests
- **Platform** (`docs/todo/PLATFORM_TODO.md`) – Cross-platform API consistency

---

## Recent Completions

### 2026-02-14
- ✅ Documented all recovery error codes in recovery_api.hpp
- ✅ Unified metrics API with state snapshot fields

### 2026-02-13
- ✅ Documented release_write_slot return values and idempotent behavior
- ✅ Documented slot handle lifetime contract in data_block.hpp
- ✅ Added get_spinlock and spinlock_count to public API

### 2026-02-12
- ✅ Transaction guard API implemented and documented
- ✅ Iterator API refined (try_next, seek_to, seek_latest)

---

## Notes

### API Evolution Strategy

1. **Additive changes only** in minor versions
2. **Deprecation** with at least one minor version notice
3. **Breaking changes** only in major versions
4. **Experimental** APIs clearly marked

### API Review Checklist

Before adding any new public API:
- [ ] Is it necessary? Can existing API cover this?
- [ ] Is the naming consistent with existing APIs?
- [ ] Is the error handling strategy clear?
- [ ] Is thread safety documented?
- [ ] Is lifetime and ownership clear?
- [ ] Are there tests covering the new API?
- [ ] Is it documented with examples?

### Common API Patterns

**Resource acquisition**:
```cpp
// Factory pattern for complex objects
auto obj = create_thing(...);

// Optional for fallible operations
auto opt = try_lock(...);
if (opt.has_value()) { use(*opt); }

// nullptr for expected failures
auto handle = acquire_slot(...);
if (handle) { use(*handle); }
```

**Error reporting**:
```cpp
// Throw for contract violations
if (invalid_config) throw std::invalid_argument("...");

// Return false for expected failures
bool success = operation();

// Return enum for recovery operations
RecoveryResult result = recover();
```
