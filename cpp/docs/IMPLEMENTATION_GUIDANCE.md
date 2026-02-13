# Data Exchange Hub - Implementation Guidance

**Document Version:** 1.1
**Last Updated:** 2026-02-13
**Status:** Active Development Guide

**Doc policy:** This is the **unified implementation guidance** for DataHub and related modules. Refer to this document during design and implementation (like a single "GEMINI.md"). **Execution order and checklist** live in **`docs/DATAHUB_TODO.md`**; do not duplicate priorities or roadmap here. See `docs/DOC_STRUCTURE.md` for the full documentation layout.

---

## Table of Contents

1. [Overview](#overview)
2. [Architecture Principles](#architecture-principles)
   - [Error reporting: C vs C++](#error-reporting-c-vs-c)
   - [Explicit noexcept where the public API does not throw](#explicit-noexcept-where-the-public-api-does-not-throw)
   - [Config validation and memory block setup](#config-validation-and-memory-block-setup-single-point-of-access)
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
3. **Layered API**: Three layers (C API, C++ wrappers, Transaction API) for different use cases. The **primitive C API** is the stable base; **C++ RAII/abstraction** (guards, with_typed_*) is the default for all higher-level design. Use the C API directly only when performance or flexibility critically require it (e.g. custom bindings, hot paths that cannot use exceptions).
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
│ TABLE 2: Data Slots (slot_stride_bytes × N slots)         │
│  - Fixed-size ring buffer slots                            │
│  - Protected by SlotRWState                                │
└────────────────────────────────────────────────────────────┘
```

### Two-Tier Synchronization

1. **OS Mutex (DataBlockMutex)**: For control zone (creation, header modifications)
2. **Atomic Coordination**: For hot path (slot access, flexible zone access)
   - SharedSpinLock: PID-based spinlock for flexible zones
   - SlotRWState: Writer-reader coordination for data slots

### C++ Abstraction Layers (DataBlock)

The primitive **C API** (Slot RW, Recovery) is the stable base; the **C++ abstraction** is the default for application code. Full design: **`docs/DATAHUB_CPP_ABSTRACTION_DESIGN.md`**.

| Layer   | Use for |
|---------|--------|
| **0 – C API** | C bindings, minimal deps, or when C++ layer cannot be used. |
| **1 – C++ primitive** | `DataBlockProducer`/`Consumer`, `SlotWriteHandle`/`SlotConsumeHandle`, explicit acquire/release. Use when you need explicit lifetime control or non-throwing paths. |
| **1.75 – Typed** | `SlotRWAccess::with_typed_write<T>` / `with_typed_read<T>` on raw `SlotRWState*` + buffer (e.g. from handle internals or diagnostic code). |
| **2 – Transaction API** | **Recommended:** `with_write_transaction`, `with_read_transaction`, `with_next_slot`, `WriteTransactionGuard` / `ReadTransactionGuard`. RAII, exception-safe. |

**Recommended usage:** Prefer Layer 2 (guards and with_*_transaction) for write/read; release or destroy all slot handles before destroying Producer/Consumer. Use C API directly only when performance or flexibility (e.g. custom bindings) require it.

### Error reporting: C vs C++

- **C API:** Report errors via **return codes** (or out-parameters). No exceptions. C has no exceptions; callers from C or other languages expect 0/success or non-zero/error. Example: `slot_rw_acquire_write` returns `SlotAcquireResult`; recovery APIs return `int` (e.g. 0 = success, negative = error).
- **C++ wrapper:** May **throw** where that is the appropriate, idiomatic way to signal failure (e.g. config validation at creation, schema mismatch on attach). Use exceptions for exceptional or contract-violation cases; do not overuse (e.g. hot path can use return/optional). The C++ layer may translate C error codes into exceptions when it improves usability.
- **Summary:** C → error codes; C++ → throw where appropriate. Each layer follows its language conventions.

### Explicit noexcept where the public API does not throw

Mark as **`noexcept`** any public API that is **not supposed to throw** and whose implementation does not throw. This makes the contract explicit and allows optimizations (e.g. move on exception paths). Do **not** mark functions that can throw (e.g. config validation, acquisition failure that throws, or calls into code that may throw).

**Recommendation:**

| Category | Mark `noexcept` | Do not mark |
|----------|-----------------|-------------|
| **Destructors** | All DataBlock-related destructors (~SlotWriteHandle, ~SlotConsumeHandle, ~DataBlockProducer, ~DataBlockConsumer, ~DataBlockSlotIterator, ~DataBlockDiagnosticHandle, transaction guards). Destructors must not throw; explicit `noexcept` enforces that. | — |
| **Simple accessors** | slot_index(), slot_id(), buffer_span(), flexible_zone_span() on handles; last_slot_id(), is_valid() on iterator; spinlock_count() on producer/consumer. They only read state or return empty span. | flexible_zone\<T\>(index) (throws if zone too small). |
| **Bool-returning / result-returning (no throw)** | write(), read(), commit(), update_checksum_*, verify_checksum_*, validate_read() on handles; release_write_slot(), release_consume_slot(); seek_latest(), seek_to(); try_next() (returns NextResult); check_consumer_health(). Implementation returns false/empty/result and does not throw. | — |
| **Acquisition / registration** | acquire_write_slot(), acquire_consume_slot() (return nullptr on failure; implementation does not throw). | acquire_spinlock() (throws out_of_range), next() (throws on timeout), commit() on WriteTransactionGuard (throws on invalid state). |
| **Constructors / factories** | Move constructors and move assignment (already noexcept). | Creator/attach constructors, create_* / find_* (throw or return nullptr). |

If in doubt, do **not** add `noexcept`; adding it incorrectly causes `std::terminate` if the function ever throws.

### Config validation and memory block setup (single point of access)

**Rule:** Config must be checked **before** any creation or operation on memory blocks. All memory-block parameters must be explicitly set from agreed specs. There is a **single point** where config is validated and the memory block is set up, so when things go wrong we know where to look.

**Single point of access:** The **DataBlock creator constructor** `DataBlock(const std::string &name, const DataBlockConfig &config)` in `data_block.cpp` is the only code path that creates a new shared-memory block. It:

1. **Validates config first** (before any allocation): on the C++ path, throws `std::invalid_argument` if any of the following are unset or invalid (a C-level creation API, if added, would return an error code instead):
   - `config.policy` (must not be `DataBlockPolicy::Unset`)
   - `config.consumer_sync_policy` (must not be `ConsumerSyncPolicy::Unset`)
   - `config.physical_page_size` (must not be `DataBlockPageSize::Unset`)
   - `config.ring_buffer_capacity` (must be ≥ 1; 0 means unset and fails)
   - `config.logical_unit_size` (if set: must be ≥ physical and a multiple of physical)
2. **Then** builds `DataBlockLayout::from_config(config)` and computes size.
3. **Then** calls `shm_create` and only after that writes the header and layout checksum.

**Public entry points:** All producer creation goes through `create_datablock_producer` → `create_datablock_producer_impl` → `DataBlock(name, config)`. Consumer attach does not create memory; it opens existing with `DataBlock(name)` and validates layout (and optionally `expected_config`) in `find_datablock_consumer_impl` via `validate_attach_layout_and_config`. So the **single point** for “config checked before any memory creation” is the DataBlock creator constructor.

**Required explicit parameters (no silent defaults):** To avoid memory corruption and sync bugs, the following must be set explicitly on `DataBlockConfig` before creating a producer; otherwise creation fails at the single point above.

| Parameter | Sentinel / invalid | Stored in header |
|-----------|--------------------|------------------|
| `policy` | `DataBlockPolicy::Unset` | 0/1/2 (Single/DoubleBuffer/RingBuffer) |
| `consumer_sync_policy` | `ConsumerSyncPolicy::Unset` | 0/1/2 (Latest_only/Single_reader/Sync_reader) |
| `physical_page_size` | `DataBlockPageSize::Unset` | 4096 / 4M / 16M |
| `ring_buffer_capacity` | `0` (unset) | ≥ 1 |

**Rationale and full parameter table:** See **`docs/DATAHUB_POLICY_AND_SCHEMA_ANALYSIS.md`** (§1 and “Other parameters: fail if not set”).

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

**Usage in application**: Create a `LifecycleGuard` in main() with every module your application uses. The guard initializes them in dependency order and shuts them down in reverse order.

```cpp
#include "plh_datahub.hpp"

int main() {
    pylabhub::utils::LifecycleGuard app_lifecycle(pylabhub::utils::MakeModDefList(
        pylabhub::utils::Logger::GetLifecycleModule(),
        pylabhub::utils::FileLock::GetLifecycleModule(),
        pylabhub::crypto::GetLifecycleModule(),
        pylabhub::hub::GetLifecycleModule(),
        pylabhub::utils::JsonConfig::GetLifecycleModule()
    ));
    // create_datablock_producer / find_datablock_consumer may now be used
}
```

**DataBlock / MessageHub**: `create_datablock_producer()` and `find_datablock_consumer()` throw if the Data Exchange Hub module is not initialized. Include `pylabhub::hub::GetLifecycleModule()` (and typically `CryptoUtils`, `Logger`) in your guard. See **`src/hubshell.cpp`** for a full template.

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
with_write_transaction(*producer, timeout_ms, [&](WriteTransactionContext& ctx) {
    ctx.slot().write(data, size);
    ctx.slot().commit(size);
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

**See also:** [Error reporting: C vs C++](#error-reporting-c-vs-c) and [Explicit noexcept where the public API does not throw](#explicit-noexcept-where-the-public-api-does-not-throw) in Architecture Principles for the C/C++ error convention and when to mark APIs `noexcept`.

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
    with_write_transaction(*producer, 1000, [&](WriteTransactionContext& ctx) {
        ctx.slot().write(test_data, sizeof(test_data));
        ctx.slot().commit(sizeof(test_data));
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

### Testing Async Callbacks

When a component invokes a callback asynchronously (e.g. Logger’s `set_write_error_callback` via `CallbackDispatcher::post()`), do not assume it has run when `flush()` or similar returns. Use a `std::promise` set in the callback and `future.wait_for()` before asserting. See **Pitfall 9** in Common Pitfalls.

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

### Pitfall 6: Non-Deterministic Struct Initialization (Hashing/Checksum)

**Problem**: Structs used in BLAKE2b or layout checksum have uninitialized padding; different runs produce different hashes, causing layout checksum validation to fail randomly.

**Solution**: Value-initialize all structs that feed into hashing or comparison: use `T var{}` or `T var = {}`.

```cpp
// WRONG: Padding may contain garbage
LayoutChecksumInput in;
in.ring_buffer_capacity = header->ring_buffer_capacity;
// ... BLAKE2b(in) - hash includes padding!

// CORRECT: Zero-initialize (including padding)
LayoutChecksumInput in{};
in.ring_buffer_capacity = header->ring_buffer_capacity;
```

Apply to: LayoutChecksumInput, DataBlockLayout, FlexibleZoneInfo, SchemaInfo, any struct passed to crypto or memcmp.

### Pitfall 7: Config parameters that can cause corruption must be set explicitly

**Problem**: Layout- and mode-critical parameters (`policy`, `consumer_sync_policy`, `physical_page_size`, `ring_buffer_capacity`) must not be left unset or mismatched; otherwise producer/consumer can get wrong layout or sync behavior → memory corruption or sync bugs.

**Solution**: These four parameters have **no valid default**; they use sentinels (`Unset` or `0`) and **producer creation fails** (throws `std::invalid_argument`) if any are unset. Always set them explicitly on `DataBlockConfig` before calling `create_datablock_producer`. Consumer attach validates layout (and optional `expected_config`) in one place: `validate_attach_layout_and_config`. See § "Config validation and memory block setup" above and `docs/DATAHUB_POLICY_AND_SCHEMA_ANALYSIS.md`.

### Pitfall 8: Test Atomic Variables Without Proper Ordering

**Problem**: Cross-thread assertion on atomic: worker thread stores, main thread loads; without release/acquire, main may not see the store.

**Solution**: Use `memory_order_release` on the storing thread, `memory_order_acquire` on the loading thread. If using `join()` before the load, synchronization is provided by join; otherwise explicit ordering is required.

```cpp
// Callback (worker thread) stores
callback_count.fetch_add(1, std::memory_order_release);

// Main thread loads (must wait for callback first - see Pitfall 9)
ASSERT_GE(callback_count.load(std::memory_order_acquire), 1);
```

### Pitfall 9: Asserting on Async Callback Before It Runs

**Problem**: Logger error callback is invoked via `callback_dispatcher_.post()` (async). Test asserts callback ran, but `flush()` does not wait for the dispatcher; assertion runs before callback.

**Solution**: Have the callback signal completion (e.g. `promise.set_value()`); test waits on `future.wait_for()` before asserting.

```cpp
std::promise<void> callback_done;
auto fut = callback_done.get_future();
Logger::instance().set_write_error_callback([&](const std::string&) {
    callback_count++;
    callback_done.set_value();  // Signal completion
});
ASSERT_FALSE(Logger::instance().set_logfile("/"));  // Triggers callback
ASSERT_EQ(fut.wait_for(2s), std::future_status::ready);  // Wait for async callback
ASSERT_GE(callback_count.load(std::memory_order_acquire), 1);
```

---

## Code Review Checklist

### Before Submitting PR

- [ ] All public classes use pImpl idiom
- [ ] All public symbols use `PYLABHUB_UTILS_EXPORT`
- [ ] Memory ordering is correct (acquire/release on ARM)
- [ ] Structs used in hashing/checksum are value-initialized (`T var{}`)
- [ ] Test atomics: use release/acquire for cross-thread visibility; use promise to wait for async callbacks before asserting
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
- **v1.2** (2026-02-13): Added § "Config validation and memory block setup (single point of access)": config checked before any memory creation; single point is DataBlock creator constructor; required explicit parameters table. Updated Pitfall 7 to explicit required params (fail if unset). TABLE 2 caption: slot_stride_bytes.
- **v1.1** (2026-02-13): Added Pitfalls 6–9 (deterministic init, config defaults, test atomics, async callbacks); testing async callbacks; checklist updates.
- **v1.0** (2026-02-09): Initial guidance document created for implementation phase
