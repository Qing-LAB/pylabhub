# Data Exchange Hub - Implementation Guidance

**Document Version:** 1.0
**Last Updated:** 2026-02-09
**Status:** Active Development Guide

**Doc policy:** This is the **unified implementation guidance** for DataHub and related modules. Refer to this document during design and implementation (like a single "GEMINI.md"). **Execution order and checklist** live in **`docs/DATAHUB_TODO.md`**; do not duplicate priorities or roadmap here. See `docs/DOC_STRUCTURE.md` for the full documentation layout.

---

## Table of Contents

1. [Overview](#overview)
2. [Architecture Principles](#architecture-principles)
3. [Codebase Structure](#codebase-structure)
4. [Integration with Existing Services](#integration-with-existing-services)
5. [ABI Stability Guidelines](#abi-stability-guidelines)
6. [Memory Management Patterns](#memory-management-patterns)
7. [Error Handling Strategy](#error-handling-strategy)
8. [Testing Strategy](#testing-strategy)
9. [Common Pitfalls and Solutions](#common-pitfalls-and-solutions)
10. [Code Review Checklist](#code-review-checklist)

---

## Overview

This document provides implementation guidance for the **Data Exchange Hub** (DataBlock) module within the `pylabhub::utils` library. The design specification is in `docs/hep/HEP-CORE-0002-DataHub-FINAL.md`.

### Design Philosophy

1. **Zero-copy IPC**: Shared memory for high-performance data transfer
2. **ABI Stability**: All public classes use pImpl idiom for shared library compatibility
3. **Layered API**: Three layers (C API, C++ wrappers, Transaction API) for different use cases
4. **Service Integration**: Leverage existing lifecycle, logger, and platform services
5. **Memory Safety**: Atomic operations with correct memory ordering for ARM/x86

---

## Architecture Principles

### Dual-Chain Architecture

The Data Exchange Hub uses a **dual-chain** memory layout:

- **TABLE 1 (Flexible Zones)**: User-defined atomics with SharedSpinLock coordination
- **TABLE 2 (Fixed Buffers)**: Ring buffer slots with SlotRWState coordination

```
┌────────────────────────────────────────────────────────────┐
│ SharedMemoryHeader (4KB)                                   │
│  - Magic, version, security                                │
│  - Ring buffer config and state                            │
│  - Metrics (256 bytes)                                     │
│  - Consumer heartbeats (512 bytes)                         │
│  - SharedSpinLock states (256 bytes)                       │
└────────────────────────────────────────────────────────────┘
┌────────────────────────────────────────────────────────────┐
│ TABLE 1: Flexible Zones (user-defined size)                │
│  - Application-specific data structures                    │
│  - Protected by SharedSpinLock                             │
└────────────────────────────────────────────────────────────┘
┌────────────────────────────────────────────────────────────┐
│ SlotRWState Array (48 bytes × N slots)                     │
│  - Cache-aligned coordination metadata                     │
└────────────────────────────────────────────────────────────┘
┌────────────────────────────────────────────────────────────┐
│ TABLE 2: Data Slots (unit_block_size × N slots)            │
│  - Fixed-size ring buffer slots                            │
│  - Protected by SlotRWState                                │
└────────────────────────────────────────────────────────────┘
```

### Two-Tier Synchronization

1. **OS Mutex (DataBlockMutex)**: For control zone (creation, header modifications)
2. **Atomic Coordination**: For hot path (slot access, flexible zone access)
   - SharedSpinLock: PID-based spinlock for flexible zones
   - SlotRWState: Writer-reader coordination for data slots

---

## Codebase Structure

### Layered Headers (Umbrella Pattern)

**DO NOT** include individual headers directly. Use layered umbrellas:

```cpp
// Layer 0: Platform detection
#include "plh_platform.hpp"

// Layer 1: Basic utilities (format, debug, guards)
#include "plh_base.hpp"

// Layer 2: Services (lifecycle, filelock, logger)
#include "plh_service.hpp"

// Layer 3: Data hub (jsonconfig, messagehub, datablock)
#include "plh_datahub.hpp"
```

### Source File Organization

```
src/
├── include/
│   ├── plh_platform.hpp           # Layer 0 umbrella
│   ├── plh_base.hpp               # Layer 1 umbrella
│   ├── plh_service.hpp            # Layer 2 umbrella
│   ├── plh_datahub.hpp            # Layer 3 umbrella
│   ├── utils/recovery_api.hpp       # P8 Recovery C API
│   ├── utils/slot_diagnostics.hpp   # P8 Diagnostics API
│   └── utils/
│       ├── data_block.hpp         # Main DataBlock API
│       ├── message_hub.hpp        # Broker communication
│       ├── shared_memory_spinlock.hpp # SharedSpinLock
│       ├── slot_rw_coordinator.h  # C API for SlotRWState
│       └── slot_rw_access.hpp     # C++ template wrappers
└── utils/
    ├── data_block.cpp             # DataBlock implementation
    ├── message_hub.cpp            # MessageHub implementation
    ├── shared_memory_spinlock.cpp     # SharedSpinLock implementation
    ├── datablock_recovery.cpp     # P8 Recovery implementation
    ├── slot_diagnostics.cpp       # P8 Diagnostics implementation
    ├── slot_recovery.cpp          # P8 Recovery operations
    ├── heartbeat_manager.cpp      # Consumer heartbeat tracking
    └── integrity_validator.cpp    # P8 Integrity validation
```

### Build System Integration

**CMakeLists.txt** (`src/utils/CMakeLists.txt`):

```cmake
set(UTILS_SOURCES
  # ... existing sources ...
  data_block.cpp
  shared_memory_spinlock.cpp
  datablock_recovery.cpp
  slot_diagnostics.cpp
  slot_recovery.cpp
  heartbeat_manager.cpp
  integrity_validator.cpp
)

# Generated export header
generate_export_header(pylabhub-utils
  BASE_NAME         "pylabhub_utils"
  EXPORT_MACRO_NAME "PYLABHUB_UTILS_EXPORT"
  EXPORT_FILE_NAME  "${CMAKE_CURRENT_BINARY_DIR}/pylabhub_utils_export.h"
)
```

---

## Integration with Existing Services

### 1. Lifecycle Management

**Pattern**: Register DataHub module for automatic initialization/shutdown.

```cpp
// In message_hub.cpp
namespace pylabhub::hub
{

// Module initialization
static void datahub_startup() {
    // Initialize ZeroMQ context, broker connection, etc.
    LOGGER_INFO("[DataHub] Module initialized");
}

static void datahub_shutdown() {
    // Cleanup ZeroMQ resources
    LOGGER_INFO("[DataHub] Module shutdown");
}

// Factory for lifecycle module
pylabhub::utils::ModuleDef GetLifecycleModule() {
    return pylabhub::utils::ModuleDef("DataHub")
        .with_startup(datahub_startup)
        .with_shutdown(datahub_shutdown)
        .with_dependencies({"Logger"}); // Depend on logger
}

} // namespace pylabhub::hub
```

**Usage in application**:

```cpp
int main() {
    pylabhub::utils::LifecycleGuard lifecycle(
        pylabhub::utils::MakeModDefList(
            pylabhub::utils::Logger::GetLifecycleModule(),
            pylabhub::hub::GetLifecycleModule()
        )
    );
    // DataHub is now initialized and ready to use
}
```

### 2. Logger Integration

**DO**: Use logger for all non-critical diagnostics and debugging.

```cpp
// Good: Use existing logger
LOGGER_INFO("[DataBlock:{}] Producer registered", name);
LOGGER_DEBUG("[DataBlock:{}] Slot {} acquired by PID {}", name, slot_idx, pid);
LOGGER_ERROR("[DataBlock:{}] Failed to acquire slot: timeout", name);
```

**DON'T**: Use `std::cout`, `printf`, or custom logging.

### 3. Platform Utilities

**DO**: Use `pylabhub::platform` functions for portability.

```cpp
// Good: Use platform utilities
uint64_t pid = pylabhub::platform::get_pid();
uint64_t tid = pylabhub::platform::get_native_thread_id();
std::string exe_name = pylabhub::platform::get_executable_name();
```

**DON'T**: Use platform-specific APIs directly in cross-platform code.

### 4. Format Tools

**DO**: Use `format_tools` for consistent formatting.

```cpp
#include "plh_base.hpp" // Includes format_tools

// Good: Use format_tools
std::string formatted = pylabhub::format_tools::format_bytes(total_bytes);
std::string timestamp = pylabhub::format_tools::format_timestamp(now_ns);
```

### 5. Debug Info

**DO**: Use `debug::print_stack_trace()` for crash diagnostics.

```cpp
#include "plh_base.hpp" // Includes debug_info

// In error paths
catch (const std::exception& ex) {
    LOGGER_ERROR("Critical error: {}", ex.what());
    pylabhub::debug::print_stack_trace();
    std::abort();
}
```

---

## ABI Stability Guidelines

### pImpl Idiom (MANDATORY for Public Classes)

All public classes in `data_block.hpp` MUST use pImpl:

```cpp
// data_block.hpp
class PYLABHUB_UTILS_EXPORT DataBlockProducer {
public:
    DataBlockProducer();
    ~DataBlockProducer(); // MUST be defined in .cpp

    // Public API...

private:
    std::unique_ptr<DataBlockProducerImpl> pImpl; // Opaque pointer
};

// data_block.cpp
struct DataBlockProducerImpl {
    // All STL containers, member variables here
    std::vector<FlexibleZoneInfo> flexible_zones;
    std::unique_ptr<DataBlockMutex> control_mutex;
    std::shared_ptr<void> shm_ptr;
    // ...
};

DataBlockProducer::DataBlockProducer()
    : pImpl(std::make_unique<DataBlockProducerImpl>()) {}

DataBlockProducer::~DataBlockProducer() = default; // Must be in .cpp
```

### Export Macros

Use `PYLABHUB_UTILS_EXPORT` for all public symbols:

```cpp
// Public class
class PYLABHUB_UTILS_EXPORT DataBlockProducer { /* ... */ };

// Public function
PYLABHUB_UTILS_EXPORT std::unique_ptr<DataBlockProducer>
create_datablock_producer(/* ... */);

// Public struct (if needed in API)
struct PYLABHUB_UTILS_EXPORT DataBlockConfig { /* ... */ };
```

### What NOT to Expose

**DO NOT** put these in public headers:

- STL containers (`std::vector`, `std::map`, `std::string` members)
- Third-party types (`zmq::socket_t`, `nlohmann::json` members)
- Platform-specific types (`HANDLE`, `pthread_mutex_t`)
- Template implementations (keep in `.hpp`, but not in public API)

---

## Memory Management Patterns

### Shared Memory Lifecycle

```cpp
// 1. Producer creates shared memory
auto producer = create_datablock_producer(hub, "sensor_data", config);

// 2. Consumer attaches to existing shared memory
auto consumer = find_datablock_consumer(hub, "sensor_data", secret);

// 3. Automatic cleanup on destruction
// Producer destructor: unmap, close fd, unlink shm_name (if owner)
// Consumer destructor: unmap, close fd, decrement consumer count
```

### RAII for Slot Access

**Lifetime contract:** Slot handles hold pointers into shared memory. Release or destroy all
SlotWriteHandle and SlotConsumeHandle instances *before* destroying the DataBlockProducer or
DataBlockConsumer. Otherwise the handle destructor accesses freed memory (use-after-free).

```cpp
// Primitive API (manual)
auto slot = producer->acquire_write_slot(timeout_ms);
if (slot) {
    slot->write(data, size);
    slot->commit(size);
    producer->release_write_slot(*slot); // Manual release
}

// Transaction API (RAII, recommended)
with_write_transaction(*producer, timeout_ms, [&](SlotWriteHandle& slot) {
    slot.write(data, size);
    slot.commit(size);
}); // Automatic release
```

### Memory Ordering (CRITICAL)

**ARM/RISC-V platforms require explicit memory barriers!**

```cpp
// CORRECT: Use acquire/release semantics
uint64_t write_idx = header->write_index.load(std::memory_order_acquire);
header->commit_index.store(write_idx, std::memory_order_release);

// WRONG: Will break on ARM
uint64_t write_idx = header->write_index.load(); // defaults to seq_cst (too strong)
header->commit_index = write_idx; // no memory barrier (too weak)
```

**Reference**: Section 4.3 of HEP-CORE-0002-DataHub-FINAL.md

---

## Error Handling Strategy

### Exception Policy

1. **Public API**: Use exceptions for programming errors (invalid arguments, precondition violations)
2. **Hot Path**: Return error codes (avoid exceptions in slot acquisition loops)
3. **Recovery API**: Return `RecoveryResult` enum

```cpp
// Public API: Throw on invalid config
std::unique_ptr<DataBlockProducer> create_datablock_producer(const DataBlockConfig& config) {
    if (config.ring_buffer_capacity == 0) {
        throw std::invalid_argument("ring_buffer_capacity must be > 0");
    }
    // ...
}

// Hot path: Return nullptr on timeout
std::unique_ptr<SlotWriteHandle> DataBlockProducer::acquire_write_slot(int timeout_ms) {
    // No exceptions, return nullptr on timeout
    if (/* timeout */) {
        return nullptr;
    }
    return std::make_unique<SlotWriteHandle>(/* ... */);
}

// Recovery API: Return enum
RecoveryResult datablock_force_reset_slot(const char* shm_name, uint32_t slot_index, bool force) {
    if (/* invalid slot */) {
        return RECOVERY_INVALID_SLOT;
    }
    if (/* unsafe */) {
        return RECOVERY_UNSAFE;
    }
    // ...
    return RECOVERY_SUCCESS;
}
```

### Logging Errors

**Pattern**: Log at error site, propagate to caller.

```cpp
// Good: Log and return error code
if (!SharedSpinLock::try_lock_for(timeout_ms)) {
    LOGGER_ERROR("[DataBlock:{}] Failed to acquire spinlock {} after {}ms",
                 name, index, timeout_ms);
    return nullptr; // Propagate to caller
}

// Bad: Silent failure
if (!lock.try_lock()) {
    return nullptr; // Caller has no idea what went wrong
}
```

---

## Testing Strategy

**Cross-references:** Test plan and Phase A–D rationale: **`docs/testing/DATAHUB_AND_MESSAGEHUB_TEST_PLAN_AND_REVIEW.md`**. Topic summary: **`docs/README/README_testing.md`**. Execution order and priorities: **`docs/DATAHUB_TODO.md`**.

### Test Organization

```
tests/test_pylabhub_utils/
├── test_datablock.cpp           # Unit tests for DataBlock
├── test_transaction_api.cpp     # P7 Transaction API tests
├── test_recovery_api.cpp        # P8 Recovery API tests
├── test_schema_validation.cpp   # P9 Schema validation tests
├── test_benchmarks.cpp          # Performance benchmarks
└── CMakeLists.txt
```

### Multi-Process Tests

Use worker processes for IPC tests:

```cpp
// test_datablock.cpp
TEST(DataBlockTest, ProducerConsumerBasic) {
    // Parent process: producer
    auto producer = create_datablock_producer(hub, "test_channel", config);

    // Spawn child process: consumer
    pid_t child_pid = fork();
    if (child_pid == 0) {
        // Child: consumer
        auto consumer = find_datablock_consumer(hub, "test_channel", secret);
        // Read data, verify...
        exit(0);
    }

    // Parent: write data
    with_write_transaction(*producer, 1000, [&](SlotWriteHandle& slot) {
        slot.write(test_data, sizeof(test_data));
        slot.commit(sizeof(test_data));
    });

    // Wait for child
    int status;
    waitpid(child_pid, &status, 0);
    ASSERT_EQ(WEXITSTATUS(status), 0);
}
```

### ThreadSanitizer (ARM MANDATORY)

```bash
# Build with TSan
cmake -S . -B build -DPYLABHUB_USE_SANITIZER=Thread

# Run tests
ctest --test-dir build --output-on-failure
```

### Test pattern choice and CTest vs direct execution

Use one of three patterns: **(1) PureApiTest** — pure functions, no lifecycle; **(2) LifecycleManagedTest** — needs Logger/FileLock/etc. in one process, shared lifecycle; **(3) WorkerProcess** — multi-process (IPC, file locks across processes, or when a test finalizes lifecycle so later tests would break). When tests modify global/singleton state in a non-reversible way (e.g. finalize lifecycle), run them via **WorkerProcess** so each run is in a separate process. **CTest** runs each test in a separate process; **direct execution** of the test binary runs all tests in one process. If you rely on process isolation (e.g. lifecycle re-init), use CTest or design the test to use WorkerProcess. See **`docs/README/README_testing.md`** for multi-process flow and staging.

---

## Common Pitfalls and Solutions

### Pitfall 1: Memory Ordering on ARM

**Problem**: Code works on x86 but fails on ARM due to weak memory model.

**Solution**: Always use `memory_order_acquire` for loads, `memory_order_release` for stores.

```cpp
// WRONG: Will break on ARM
auto state = slot->slot_state.load(); // defaults to seq_cst

// CORRECT: Explicit acquire
auto state = slot->slot_state.load(std::memory_order_acquire);
```

### Pitfall 2: TOCTTOU Races in Reader Acquisition

**Problem**: Reader checks state, but writer changes it before reader acquires.

**Solution**: Use double-check pattern + generation counter (Section 4.2.3).

```cpp
// CORRECT: Double-check with generation
uint64_t gen_before = rw->write_generation.load(std::memory_order_acquire);
auto state = rw->slot_state.load(std::memory_order_acquire);
if (state != SlotState::COMMITTED) {
    return SLOT_NOT_READY;
}
rw->reader_count.fetch_add(1, std::memory_order_acq_rel);
std::atomic_thread_fence(std::memory_order_seq_cst); // Fence
uint64_t gen_after = rw->write_generation.load(std::memory_order_acquire);
if (gen_before != gen_after) {
    rw->reader_count.fetch_sub(1, std::memory_order_release);
    return SLOT_RACE_DETECTED;
}
```

### Pitfall 3: PID Reuse

**Problem**: Process crashes, OS reuses PID, new process sees stale lock.

**Solution**: Use generation counter in SharedSpinLock (Section 4.1.2).

```cpp
// SharedSpinLock uses PID + generation
struct SharedSpinLockState {
    std::atomic<uint64_t> owner_pid;
    std::atomic<uint64_t> generation; // Incremented on each acquire
};
```

### Pitfall 4: Forgetting to Update Metrics

**Problem**: Errors occur but metrics not updated, monitoring is blind.

**Solution**: Wrap error paths with metric updates.

```cpp
// Good: Update metrics on error
if (timeout) {
    header->writer_timeout_count.fetch_add(1, std::memory_order_relaxed);
    return nullptr;
}
```

### Pitfall 5: Breaking ABI with STL in Headers

**Problem**: Adding `std::vector<T>` member to public class breaks ABI.

**Solution**: Keep all STL members in pImpl.

```cpp
// BAD: STL member in public class
class DataBlockProducer {
    std::vector<FlexibleZoneInfo> zones; // ABI break!
};

// GOOD: STL member in pImpl
struct DataBlockProducerImpl {
    std::vector<FlexibleZoneInfo> zones; // Hidden from ABI
};
```

---

## Code Review Checklist

### Before Submitting PR

- [ ] All public classes use pImpl idiom
- [ ] All public symbols use `PYLABHUB_UTILS_EXPORT`
- [ ] Memory ordering is correct (acquire/release on ARM)
- [ ] Errors are logged with LOGGER_ERROR/WARN
- [ ] Metrics are updated on error paths
- [ ] Tests cover multi-process scenarios
- [ ] ThreadSanitizer passes on ARM (if available)
- [ ] Documentation updated (HEP document if design changed)
- [ ] No hardcoded paths or magic numbers
- [ ] Code follows CLAUDE.md conventions (Allman braces, 100-char lines, 4-space indent)

### Design Review

- [ ] Does this require lifecycle registration?
- [ ] Is memory ordering correct for ARM?
- [ ] Are there potential TOCTTOU races?
- [ ] Is PID reuse handled correctly?
- [ ] Are checksums validated (if enabled)?
- [ ] Are heartbeats updated (for consumers)?
- [ ] Is schema validation performed (P9)?

### Performance Review

- [ ] Hot path avoids exceptions
- [ ] Atomic operations use relaxed ordering where safe
- [ ] Backoff strategy for spin loops
- [ ] No unnecessary memory barriers

---

## References

- **Design Specification**: `docs/hep/HEP-CORE-0002-DataHub-FINAL.md`
- **Build System**: `CLAUDE.md`
- **Lifecycle Pattern**: `src/include/utils/lifecycle.hpp`
- **Logger Usage**: `src/include/utils/logger.hpp`
- **Platform Utilities**: `src/include/plh_platform.hpp`

---

**Revision History**:
- **v1.0** (2026-02-09): Initial guidance document created for implementation phase
