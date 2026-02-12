# PyLabHub Layered Test Architecture

**Date:** 2026-02-09
**Status:** Design Document
**Purpose:** Comprehensive test refactoring aligned with layered module architecture

---

## Executive Summary

This document proposes a complete restructuring of the PyLabHub test suite to mirror the layered umbrella header architecture (plh_platform → plh_base → plh_service → plh_datahub). The goal is to create a maintainable, scalable test ecosystem that ensures each layer is thoroughly validated before higher layers depend on it.

### Key Principles

1. **Layer Isolation**: Test each layer independently before testing dependencies
2. **Bottom-Up Validation**: Platform → Base → Service → DataHub
3. **Comprehensive Coverage**: Test all public APIs and critical internal functions
4. **Independent Processes**: Use gtest with worker processes for realistic IPC scenarios
5. **Clear Organization**: Test file structure mirrors source module structure
6. **Future-Proof**: Easy to add new tests as modules evolve

---

## Current Module Architecture

### Layer 0: plh_platform.hpp (Platform Abstraction)
**Library**: `pylabhub-basic` (static)
**Dependencies**: None
**Modules**:
- Platform detection macros (PYLABHUB_PLATFORM_*, PYLABHUB_IS_*)
- `pylabhub::platform`:
  - `get_native_thread_id()`
  - `get_pid()`
  - `get_executable_name()`
  - `get_version_*()` (major, minor, rolling, string)
  - `is_process_alive(uint64_t pid)`
  - `monotonic_time_ns()`
  - `elapsed_time_ns(uint64_t start_ns)`

### Layer 1: plh_base.hpp (Foundational Utilities)
**Library**: `pylabhub-basic` (static)
**Dependencies**: plh_platform.hpp
**Modules**:
- `utils/format_tools.hpp`: Formatting utilities (formattable concepts, format helpers)
- `utils/debug_info.hpp`: Stack traces, debug symbols
- `utils/atomic_guard.hpp`: RAII atomic boolean guard
- `utils/recursion_guard.hpp`: Reentrant call detection
- `utils/scope_guard.hpp`: RAII cleanup actions
- `utils/module_def.hpp`: Lifecycle module registration interface

### Layer 2: plh_service.hpp (Core Services)
**Library**: `pylabhub-utils` (shared)
**Dependencies**: plh_base.hpp
**Modules**:
- `utils/backoff_strategy.hpp`: Spin loop backoff strategies (header-only templates)
- `utils/lifecycle.hpp`: Application lifecycle management (LifecycleGuard, dependency resolution)
- `utils/crypto_utils.hpp`: Cryptographic primitives (BLAKE2b, random generation, libsodium)
- `utils/file_lock.hpp`: Cross-platform file locking (exclusive/shared)
- `utils/logger.hpp`: Async logging system (Logger, sinks, command queue)

### Layer 3: plh_datahub.hpp (Data Exchange)
**Library**: `pylabhub-utils` (shared)
**Dependencies**: plh_service.hpp
**Modules**:
- `utils/schema_blds.hpp`: Schema validation (BLDS, SchemaInfo, BLAKE2b hashing)
- `utils/json_config.hpp`: JSON configuration loader (nlohmann::json wrapper)
- `utils/message_hub.hpp`: Service discovery broker (producer/consumer registration)
- `utils/data_block.hpp`: Shared memory IPC (DataBlock, transactions, ring buffers)
  - Supporting headers:
    - `utils/data_block_mutex.hpp`: OS-backed control zone locks
    - `utils/shared_memory_spinlock.hpp`: PID-based user-space spin locks
    - `utils/slot_rw_coordinator.h`: C interface for slot coordination
    - `utils/slot_rw_access.hpp`: C++ template wrappers for slot access

### Unclassified Headers (P10+ Forward-Looking)
**Status**: API defined, implementation incomplete
**Should be**: Organized into a future `plh_recovery.hpp` umbrella (Layer 3.5)
**Modules**:
- `utils/recovery_api.hpp`: C-style recovery API (diagnostics, slot reset, zombie cleanup)
- `utils/slot_diagnostics.hpp`: C++ wrapper for slot diagnostics
- `utils/slot_recovery.hpp`: C++ wrapper for slot recovery
- `utils/heartbeat_manager.hpp`: C++ RAII heartbeat registration
- `utils/integrity_validator.hpp`: C++ integrity validation wrapper

**Recommendation**: Create `plh_recovery.hpp` umbrella header for P10+ implementation:
```cpp
#pragma once
#include "plh_datahub.hpp"
#include "utils/recovery_api.hpp"        // C interface
#include "utils/slot_diagnostics.hpp"    // C++ diagnostics
#include "utils/slot_recovery.hpp"       // C++ recovery
#include "utils/heartbeat_manager.hpp"   // RAII heartbeat
#include "utils/integrity_validator.hpp" // Integrity checks
```

---

## Current Test Structure (Before Refactoring)

### test_framework/ ✅ (Keep)
**Purpose**: Shared test infrastructure
**Contents**:
- `shared_test_helpers.h/.cpp`: StringCapture, file I/O, scaling, worker helpers
- `main.cpp`: gtest main with worker dispatcher integration

### test_pylabhub_corelib/ ✅ (Mostly Good)
**Purpose**: Tests for `pylabhub-basic` (Layer 0 + Layer 1)
**Current Tests**:
- ✅ `test_platform.cpp` - Platform utilities
- ✅ `test_atomicguard.cpp` - AtomicGuard RAII
- ✅ `test_recursionguard.cpp` - RecursionGuard
- ✅ `test_scopeguard.cpp` - ScopeGuard RAII
- ✅ `test_formattable.cpp` - Format tools

**Issues**: None major, good layer isolation

### test_pylabhub_utils/ ⚠️ (Needs Major Refactoring)
**Purpose**: Tests for `pylabhub-utils` (Layer 2 + Layer 3)
**Current Tests** (mixed layers, poor organization):
- Layer 2 (Service):
  - ✅ `test_lifecycle.cpp` - Lifecycle management
  - ✅ `test_lifecycle_dynamic.cpp` - Dynamic module loading
  - ✅ `test_filelock.cpp` - FileLock functionality
  - ✅ `test_logger.cpp` - Logger sinks and formatting
- Layer 3 (DataHub):
  - ✅ `test_jsonconfig.cpp` - JSON config loading
  - ✅ `test_messagehub.cpp` - MessageHub registration
  - ⚠️ `test_datablock.cpp` - DataBlock (mixed concerns, API issues)
  - ⚠️ `test_datablock_management_mutex.cpp` - DataBlockMutex
  - ⚠️ `test_transaction_api.cpp` - Transaction API (API issues)
  - ⚠️ `test_schema_validation.cpp` - Schema validation (incomplete)
  - ❌ `test_recovery_api.cpp` - Recovery (unimplemented, forward-looking)
- Other:
  - `test_benchmarks.cpp` - Performance benchmarks

**Issues**:
1. Mixed layer concerns (Service and DataHub tests in same directory)
2. No tests for crypto_utils, backoff_strategy
3. DataBlock tests use obsolete APIs
4. Poor test isolation (no cleanup)
5. Schema validation insufficient
6. Recovery tests reference unimplemented code

### test_misc/ ❓ (Unknown)
**Purpose**: Miscellaneous tests
**Status**: Need to assess contents

---

## Proposed Layered Test Architecture

### Directory Structure

```
tests/
├── CMakeLists.txt                          # Top-level test configuration
├── test_framework/                         # ✅ Shared infrastructure (keep as-is)
│   ├── shared_test_helpers.h/.cpp
│   └── main.cpp
│
├── test_layer0_platform/                   # NEW: Layer 0 tests
│   ├── CMakeLists.txt
│   ├── test_platform_detection.cpp         # Macro tests
│   ├── test_platform_process.cpp           # PID, process liveness
│   ├── test_platform_timing.cpp            # Monotonic time, elapsed time
│   └── test_platform_info.cpp              # Version info, executable name
│
├── test_layer1_base/                       # REFACTOR: Layer 1 tests
│   ├── CMakeLists.txt
│   ├── test_format_tools.cpp               # Formatting utilities
│   ├── test_debug_info.cpp                 # Stack traces, debug symbols
│   ├── test_atomic_guard.cpp               # AtomicGuard RAII
│   ├── test_recursion_guard.cpp            # RecursionGuard
│   ├── test_scope_guard.cpp                # ScopeGuard RAII
│   └── test_module_def.cpp                 # ModuleDef interface
│
├── test_layer2_service/                    # NEW: Layer 2 tests (pure service layer)
│   ├── CMakeLists.txt
│   ├── test_backoff_strategy.cpp           # NEW: Backoff strategies
│   ├── test_lifecycle.cpp                  # Lifecycle management
│   ├── test_lifecycle_dynamic.cpp          # Dynamic module loading
│   ├── test_crypto_utils.cpp               # NEW: BLAKE2b, random generation
│   ├── test_file_lock.cpp                  # FileLock (multi-process)
│   ├── test_logger.cpp                     # Logger core
│   ├── test_logger_sinks.cpp               # Logger sinks
│   ├── test_logger_async.cpp               # Async command queue
│   └── workers/                            # Worker processes for multi-process tests
│       ├── filelock_workers.cpp
│       ├── lifecycle_workers.cpp
│       └── worker_dispatcher.cpp
│
├── test_layer3_datahub/                    # NEW: Layer 3 tests (data exchange)
│   ├── CMakeLists.txt
│   ├── test_schema_blds.cpp                # BLDS schema generation
│   ├── test_schema_validation.cpp          # Schema hash validation
│   ├── test_json_config.cpp                # JSON configuration
│   ├── test_message_hub.cpp                # Service discovery
│   ├── test_datablock_mutex.cpp            # Control zone OS locks
│   ├── test_shared_memory_spinlock.cpp         # User-space PID locks
│   ├── test_datablock_core.cpp             # DataBlock creation, memory layout
│   ├── test_datablock_slot_coordination.cpp # Slot acquire/release, RW locks
│   ├── test_datablock_transactions.cpp     # Transaction API (guards, lambdas)
│   ├── test_datablock_ringbuffer.cpp       # Ring buffer wrap-around, iteration
│   ├── test_datablock_checksum.cpp         # Checksum computation/validation
│   ├── test_datablock_schema.cpp           # Producer/consumer schema validation
│   ├── test_datablock_multiprocess.cpp     # Multi-process IPC scenarios
│   ├── test_datablock_benchmarks.cpp       # Performance benchmarks
│   └── workers/                            # Worker processes for DataBlock tests
│       ├── datablock_workers.cpp
│       └── worker_dispatcher.cpp
│
├── test_layer3_recovery/                   # FUTURE: Layer 3.5 tests (P10+)
│   ├── CMakeLists.txt
│   ├── test_recovery_api.cpp               # C recovery API
│   ├── test_slot_diagnostics.cpp           # Slot diagnostics
│   ├── test_slot_recovery.cpp              # Slot recovery (zombie cleanup)
│   ├── test_heartbeat_manager.cpp          # Heartbeat RAII
│   ├── test_integrity_validator.cpp        # Integrity validation
│   └── workers/
│       └── recovery_workers.cpp            # Crashed process simulations
│
└── test_integration/                       # FUTURE: End-to-end integration tests
    ├── CMakeLists.txt
    ├── test_producer_consumer_flow.cpp     # Full IPC workflow
    ├── test_schema_evolution.cpp           # Schema versioning scenarios
    └── test_real_world_scenarios.cpp       # Representative use cases
```

---

## Layer-by-Layer Test Plan

### Layer 0: Platform Tests (test_layer0_platform/)

**Purpose**: Validate cross-platform abstractions work correctly on target OS

#### test_platform_detection.cpp
**Tests**:
- ✅ `PYLABHUB_PLATFORM_*` macros set correctly for current OS
- ✅ `PYLABHUB_IS_WINDOWS` / `PYLABHUB_IS_POSIX` correct
- ✅ Only one platform macro defined (mutual exclusion)

#### test_platform_process.cpp
**Tests**:
- ✅ `get_pid()` returns current process ID
- ✅ `is_process_alive(get_pid())` returns true (self)
- ✅ `is_process_alive(0)` returns false (invalid PID)
- ✅ `is_process_alive(<dead_pid>)` returns false
  - Multi-process test: spawn child, exit, verify parent detects death
- ✅ `is_process_alive(<nonexistent_pid>)` returns false
- ✅ Thread safety: concurrent calls to `is_process_alive()`

#### test_platform_timing.cpp
**Tests**:
- ✅ `monotonic_time_ns()` returns non-zero
- ✅ `monotonic_time_ns()` is monotonic (never goes backward)
  - Loop 1000x, verify `t2 >= t1`
- ✅ `elapsed_time_ns(start)` returns reasonable values
  - Sleep 100ms, verify elapsed ≈ 100ms ± 20ms
- ✅ `elapsed_time_ns(future_time)` returns 0 (clock skew protection)
- ✅ Precision test: verify nanosecond resolution (platform-dependent)

#### test_platform_info.cpp
**Tests**:
- ✅ `get_version_major/minor/rolling()` return expected values
- ✅ `get_version_string()` format matches `"major.minor.rolling"`
- ✅ `get_executable_name(false)` returns filename only
- ✅ `get_executable_name(true)` returns full path
- ✅ `get_native_thread_id()` returns non-zero
- ✅ `get_native_thread_id()` differs across threads

**Dependencies**: None
**Blocking**: All other layers

---

### Layer 1: Base Tests (test_layer1_base/)

**Purpose**: Validate foundational utilities used by all higher layers

#### test_format_tools.cpp (RENAME from test_formattable.cpp)
**Tests**:
- ✅ `formattable` concept validates correctly
- ✅ Custom format helpers work
- ✅ (Add tests for other format_tools utilities)

#### test_debug_info.cpp (NEW)
**Tests**:
- ✅ `print_stack_trace()` produces output
- ✅ Stack trace includes current function name
- ✅ Debug symbol resolution (if available)

#### test_atomic_guard.cpp (RENAME from test_atomicguard.cpp)
**Tests**:
- ✅ Constructor sets flag to true
- ✅ Destructor resets flag to false
- ✅ `is_set()` returns correct state
- ✅ Non-copyable, non-movable
- ✅ Thread safety: concurrent guards on same atomic

#### test_recursion_guard.cpp (RENAME from test_recursionguard.cpp)
**Tests**:
- ✅ First call succeeds
- ✅ Reentrant call fails (detects recursion)
- ✅ After destructor, next call succeeds
- ✅ Thread-local (different threads independent)

#### test_scope_guard.cpp (RENAME from test_scopeguard.cpp)
**Tests**:
- ✅ Cleanup action executes on normal exit
- ✅ Cleanup action executes on exception
- ✅ `dismiss()` prevents cleanup
- ✅ Move semantics work correctly

#### test_module_def.cpp (NEW)
**Tests**:
- ✅ `ModuleDef` construction with name
- ✅ `set_startup/shutdown` callbacks work
- ✅ `add_dependency/add_dependent` link modules
- ✅ `MakeModDefList` creates module list

**Dependencies**: Layer 0
**Blocking**: Layer 2, Layer 3

---

### Layer 2: Service Tests (test_layer2_service/)

**Purpose**: Validate core services before DataHub depends on them

#### test_backoff_strategy.cpp (NEW - CRITICAL)
**Tests**:
- ✅ `ExponentialBackoff`:
  - First 4 iterations: `yield()` only
  - Iterations 4-10: 1μs sleep
  - Iterations 10+: exponential (10μs, 20μs, 30μs, ...)
- ✅ `ConstantBackoff`: always sleep 10μs
- ✅ `NoBackoff`: no operation (busy spin)
- ✅ `AggressiveBackoff`: `yield()` only
- ✅ Custom backoff strategy (user-defined functor)
- ✅ Backoff doesn't throw exceptions

**Rationale**: DataBlock spin locks use backoff strategies. Must work correctly to avoid deadlocks or performance issues.

#### test_crypto_utils.cpp (NEW - CRITICAL)
**Tests**:
- ✅ `compute_blake2b()` produces 32-byte hash
- ✅ BLAKE2b hash is deterministic (same input → same output)
- ✅ BLAKE2b hash differs for different inputs
- ✅ `compute_blake2b_array()` matches `compute_blake2b()`
- ✅ `verify_blake2b()` returns true for correct hash
- ✅ `verify_blake2b()` returns false for incorrect hash
- ✅ `generate_random_bytes()` produces non-zero output
- ✅ `generate_random_bytes()` differs across calls (randomness)
- ✅ `generate_random_u64()` produces non-zero values
- ✅ `generate_shared_secret()` produces 64-byte secret
- ✅ Lifecycle integration: module initializes libsodium
- ✅ Thread safety: concurrent BLAKE2b calls
- ✅ Performance: BLAKE2b throughput (MB/s)

**Rationale**: Schema validation depends on BLAKE2b. Must be correct and performant.

#### test_lifecycle.cpp (REFACTOR - Already exists)
**Current Tests**: Good coverage
**Actions**:
- ✅ Keep existing tests
- ✅ Fix any API issues
- ✅ Add dependency resolution tests (topological sort)

#### test_lifecycle_dynamic.cpp (REFACTOR - Already exists)
**Current Tests**: Good coverage
**Actions**:
- ✅ Keep existing tests
- ✅ Ensure compatibility with current API

#### test_file_lock.cpp (REFACTOR - Already exists)
**Current Tests**: Multi-process tests
**Actions**:
- ✅ Keep existing multi-process tests
- ✅ Verify worker dispatcher integration
- ✅ Add test for lock timeout behavior

#### test_logger.cpp (REFACTOR - Already exists)
**Current Tests**: Logger and sinks
**Actions**:
- ✅ Keep existing tests
- ⚠️ Split into 3 files for clarity:
  - `test_logger.cpp` - Core logger functionality
  - `test_logger_sinks.cpp` - Sink implementations
  - `test_logger_async.cpp` - Command queue, thread safety

**Dependencies**: Layer 0, Layer 1
**Blocking**: Layer 3 (DataHub uses Logger extensively)

---

### Layer 3: DataHub Tests (test_layer3_datahub/)

**Purpose**: Validate data exchange components with full layer stack

#### test_schema_blds.cpp (NEW - CRITICAL for P9.2)
**Tests**:
- ✅ `BLDSTypeID` maps C++ types correctly:
  - `float` → `"f32"`, `double` → `"f64"`
  - `int8_t` → `"i8"`, ..., `uint64_t` → `"u64"`
  - `bool` → `"b"`, `char` → `"c"`
  - Arrays: `float[4]` → `"f32[4]"`
  - `char[64]` → `"c[64]"` (string special case)
- ✅ `BLDSBuilder` constructs BLDS strings:
  - `add_member("foo", "u64")` → `"foo:u64"`
  - Multiple members: `"foo:u64;bar:f32"`
- ✅ `SchemaVersion::pack()` packs correctly:
  - `{1, 2, 3}.pack()` → verify bit layout
- ✅ `SchemaVersion::unpack()` unpacks correctly:
  - Round-trip: `unpack(pack(v)) == v`
- ✅ `PYLABHUB_SCHEMA_*` macros generate BLDS:
  - Simple struct → correct BLDS string
  - Nested members → correct ordering
- ✅ `generate_schema_info()` computes hash:
  - Same struct → same hash (deterministic)
  - Different struct → different hash
- ✅ `SchemaInfo::matches()` compares hashes correctly

#### test_schema_validation.cpp (REFACTOR - CRITICAL for P9.2)
**Current Tests**: 2 basic tests
**Expanded Tests** (10+ required):
1. ✅ Producer stores schema (hash + version) in SharedMemoryHeader
2. ✅ Consumer validates matching schema succeeds
3. ✅ Consumer rejects mismatched schema (different struct)
4. ✅ Consumer rejects incompatible major version
5. ✅ Consumer accepts compatible minor version (future: P9.5)
6. ✅ Schema producer + schema consumer (matching) → success
7. ✅ Schema producer + non-schema consumer → fails
8. ✅ Non-schema producer + schema consumer → fails
9. ✅ Non-schema producer + non-schema consumer → success
10. ✅ BLDS generation is consistent across compilations
11. ✅ Schema hash is unique (collision test with many structs)
12. ✅ Version packing preserves major/minor/patch correctly
13. ✅ Schema mismatch logs clear error message with details

**Actions**:
- ✅ Fix API usage (MessageHub, shared_secret)
- ✅ Expand test coverage to 10+ tests
- ✅ Add proper cleanup

#### test_json_config.cpp (REFACTOR - Already exists)
**Actions**:
- ✅ Verify current tests work
- ✅ Ensure no dependency on DataBlock (pure config loading)

#### test_message_hub.cpp (REFACTOR - Already exists)
**Actions**:
- ✅ Verify current tests work
- ✅ Add test for schema registry (future P9.4)

#### test_datablock_mutex.cpp (RENAME from test_datablock_management_mutex.cpp)
**Tests**: Control zone OS-backed locks
**Actions**:
- ✅ Verify current tests work
- ✅ Test exclusive lock acquisition
- ✅ Test shared lock acquisition (reader/writer pattern)
- ✅ Test lock timeout behavior
- ✅ Multi-process lock contention

#### test_shared_memory_spinlock.cpp (NEW)
**Tests**: User-space PID-based spin locks
**Tests**:
- ✅ `SharedSpinLock::try_lock()` succeeds when free
- ✅ `SharedSpinLock::try_lock()` fails when held
- ✅ `SharedSpinLock::try_lock_for(timeout)` respects timeout
- ✅ `SharedSpinLock::unlock()` releases lock
- ✅ Zombie lock detection: dead PID → reclaim lock
- ✅ Multi-process lock contention
- ✅ Backoff strategy integration
- ✅ Lock fairness (no starvation)

#### test_datablock_core.cpp (NEW - SPLIT from test_datablock.cpp)
**Tests**: DataBlock creation, memory layout, basic operations
**Tests**:
- ✅ `create_datablock_producer()` succeeds
- ✅ `find_datablock_consumer()` finds producer
- ✅ SharedMemoryHeader initialized correctly:
  - Magic number, version, timestamps
  - Ring buffer capacity, unit block size
  - Schema hash/version (if provided)
- ✅ Memory layout matches specification:
  - Header at offset 0
  - Flexible zones follow header
  - RW states follow flexible zones
  - Ring buffer slots follow RW states
- ✅ DataBlockPolicy::Single creates 1 slot
- ✅ DataBlockPolicy::DoubleBuffer creates 2 slots
- ✅ DataBlockPolicy::RingBuffer creates N slots
- ✅ Shared secret validation works
- ✅ Invalid config rejected (capacity=0, block_size=0, etc.)

#### test_datablock_slot_coordination.cpp (NEW - SPLIT from test_datablock.cpp)
**Tests**: Slot acquire/release, read/write lock coordination
**Tests**:
- ✅ Producer acquires write slot in FREE state
- ✅ Producer cannot acquire slot in WRITING state (self-check)
- ✅ Producer commits slot: FREE → WRITING → COMMITTED
- ✅ Consumer acquires consume slot in COMMITTED state
- ✅ Consumer cannot acquire slot in FREE state
- ✅ Multiple consumers read same slot (reader_count increments)
- ✅ Writer waits for readers to drain (DRAINING state)
- ✅ Writer acquisition timeout works
- ✅ Consumer acquisition timeout works
- ✅ TOCTTOU detection: write_generation increments

#### test_datablock_transactions.cpp (REFACTOR - MERGE test_datablock.cpp + test_transaction_api.cpp)
**Tests**: Transaction API (WriteTransactionGuard, ReadTransactionGuard, lambdas)
**Actions**:
- ✅ Merge tests from test_datablock.cpp and test_transaction_api.cpp
- ✅ Remove pImpl access (use indirect verification)
- ✅ Fix API usage (MessageHub, shared_secret)
- ✅ Add schema-aware transaction tests
- ✅ Test exception safety (abort on exception)
- ✅ Test move semantics
- ✅ Test lambda helpers (with_write_transaction, with_read_transaction, with_next_slot)

#### test_datablock_ringbuffer.cpp (NEW)
**Tests**: Ring buffer wrap-around, iteration, backpressure
**Tests**:
- ✅ Producer wraps around at capacity (slot N → slot 0)
- ✅ Consumer iterator tracks producer writes
- ✅ Consumer iterator skips dropped slots (overwrite)
- ✅ `try_next(timeout)` waits for new data
- ✅ `try_next(0)` returns immediately if no data
- ✅ Backpressure: writer waits if buffer full and readers active
- ✅ Slot ID monotonic (never reuses IDs)

#### test_datablock_checksum.cpp (NEW)
**Tests**: Checksum computation and validation
**Tests**:
- ✅ Checksum policy None: no checksum computed
- ✅ Checksum policy CRC32: checksum stored in SlotRWState
- ✅ Checksum policy BLAKE2b: hash stored in slot metadata
- ✅ Consumer validates checksum on read
- ✅ Corrupted data fails checksum validation
- ✅ Checksum verification performance overhead

#### test_datablock_schema.cpp (EXPAND test_schema_validation.cpp)
**Tests**: Producer/consumer schema validation integrated with DataBlock
**Tests**: (See test_schema_validation.cpp above)

#### test_datablock_multiprocess.cpp (NEW)
**Tests**: Multi-process IPC scenarios
**Tests**:
- ✅ Producer in process A, consumer in process B
- ✅ Multiple consumers in different processes
- ✅ Producer writes, consumer reads correct data
- ✅ Process A writes, process B reads, values match
- ✅ High-frequency writes (1000+ messages)
- ✅ Consumer survives producer restart
- ✅ Producer survives consumer detach

#### test_datablock_benchmarks.cpp (REFACTOR - RENAME from test_benchmarks.cpp)
**Tests**: Performance measurements
**Tests**:
- ✅ Write throughput (messages/sec)
- ✅ Read throughput (messages/sec)
- ✅ Latency (P50, P95, P99, max)
- ✅ Schema validation overhead
- ✅ Checksum validation overhead
- ✅ Multi-consumer scalability

**Dependencies**: Layer 0, Layer 1, Layer 2
**Blocking**: None (top layer)

---

### Layer 3.5: Recovery Tests (test_layer3_recovery/) - FUTURE P10+

**Purpose**: Validate error recovery and diagnostics (deferred to P10+)

#### test_recovery_api.cpp
**Tests**: C-style recovery API
**Status**: ⏸️ GTEST_SKIP() until P10 implemented

#### test_slot_diagnostics.cpp
**Tests**: Slot diagnostics (stuck detection, state inspection)
**Status**: ⏸️ GTEST_SKIP() until P10 implemented

#### test_slot_recovery.cpp
**Tests**: Slot recovery (zombie lock release, force reset)
**Status**: ⏸️ GTEST_SKIP() until P10 implemented

#### test_heartbeat_manager.cpp
**Tests**: Consumer heartbeat RAII registration
**Status**: ⏸️ GTEST_SKIP() until P11 implemented

#### test_integrity_validator.cpp
**Tests**: DataBlock integrity validation and repair
**Status**: ⏸️ GTEST_SKIP() until P11 implemented

**Dependencies**: Layer 3 complete
**Blocking**: None (future work)

---

## Test Infrastructure Improvements

### 1. Shared Test Helpers (test_framework/)

**Existing** (Keep):
- ✅ `StringCapture` - Output capture
- ✅ `read_file_contents()` - File I/O
- ✅ `count_lines()` - Log analysis
- ✅ `wait_for_string_in_file()` - Async monitoring
- ✅ `test_scale()` / `scaled_value()` - CI scaling
- ✅ `run_gtest_worker()` - Worker wrapper

**New Additions**:

```cpp
// shared_test_helpers.h additions:

namespace pylabhub::tests::helper {

/**
 * @brief Generates unique test channel name with timestamp.
 * @param test_name Base name (e.g., "SchemaValidation")
 * @return Unique channel name (e.g., "test_SchemaValidation_1675960234567")
 */
std::string make_test_channel_name(const char* test_name);

/**
 * @brief Cleans up shared memory DataBlock after test.
 * @param channel_name Channel name to clean up
 * @return True if cleanup succeeded
 */
bool cleanup_test_datablock(const std::string& channel_name);

/**
 * @brief RAII guard for test DataBlock cleanup.
 * Usage:
 *   DataBlockTestGuard guard("my_test_channel");
 *   // Test code...
 *   // ~DataBlockTestGuard() cleans up automatically
 */
class DataBlockTestGuard {
public:
    explicit DataBlockTestGuard(const std::string& channel_name);
    ~DataBlockTestGuard();
    const std::string& channel_name() const { return channel_name_; }
private:
    std::string channel_name_;
};

/**
 * @brief Verifies schema stored in DataBlock header.
 * @param consumer Consumer to read header from
 * @param expected Expected SchemaInfo
 * @return True if schema matches
 */
bool verify_schema_stored(
    const pylabhub::hub::DataBlockConsumer& consumer,
    const pylabhub::schema::SchemaInfo& expected
);

/**
 * @brief Extracts schema hash from DataBlock header.
 * @param consumer Consumer to read from
 * @return 32-byte schema hash
 */
std::array<uint8_t, 32> get_stored_schema_hash(
    const pylabhub::hub::DataBlockConsumer& consumer
);

} // namespace pylabhub::tests::helper
```

### 2. Test Naming Conventions

**File Naming**:
- Pattern: `test_<module_name>.cpp`
- Examples:
  - `test_platform_timing.cpp` (not `test_timing.cpp`)
  - `test_datablock_core.cpp` (not `test_datablock.cpp`)

**Test Case Naming**:
- Pattern: `<Category><Test>_<Behavior>`
- Examples:
  - `PlatformTiming_MonotonicTimeNs_NeverGoesBackward`
  - `DataBlockCore_CreateProducer_Succeeds`
  - `SchemaValidation_ConsumerWithMismatch_ReturnsNull`

**Test Fixture Naming**:
- Pattern: `<Module>Test`
- Examples:
  - `class PlatformTimingTest : public ::testing::Test`
  - `class DataBlockCoreTest : public ::testing::Test`

### 3. Test Isolation Best Practices

**Always**:
1. Use unique channel names per test (no hardcoded names)
2. Clean up shared memory in `TearDown()`
3. Use RAII guards for automatic cleanup
4. Reset static state between tests

**Example**:
```cpp
class SchemaValidationTest : public ::testing::Test {
protected:
    void SetUp() override {
        channel_name = helper::make_test_channel_name("SchemaValidation");
        hub = std::make_unique<MessageHub>();
    }

    void TearDown() override {
        producer.reset();  // Release before cleanup
        consumer.reset();
        helper::cleanup_test_datablock(channel_name);
    }

    std::string channel_name;
    std::unique_ptr<MessageHub> hub;
    std::unique_ptr<DataBlockProducer> producer;
    std::unique_ptr<DataBlockConsumer> consumer;
};
```

---

## CMakeLists.txt Organization

### Top-Level tests/CMakeLists.txt
```cmake
if(BUILD_TESTS)
    enable_testing()
    include(GoogleTest)

    # Shared infrastructure
    add_subdirectory(test_framework)

    # Layer 0: Platform
    add_subdirectory(test_layer0_platform)

    # Layer 1: Base
    add_subdirectory(test_layer1_base)

    # Layer 2: Service
    add_subdirectory(test_layer2_service)

    # Layer 3: DataHub
    add_subdirectory(test_layer3_datahub)

    # Layer 3.5: Recovery (P10+, disabled by default)
    option(BUILD_RECOVERY_TESTS "Build recovery tests (requires P10+ implementation)" OFF)
    if(BUILD_RECOVERY_TESTS)
        add_subdirectory(test_layer3_recovery)
    endif()

    # Integration tests (future)
    option(BUILD_INTEGRATION_TESTS "Build integration tests" OFF)
    if(BUILD_INTEGRATION_TESTS)
        add_subdirectory(test_integration)
    endif()
endif()
```

### Per-Layer CMakeLists.txt Pattern
```cmake
# tests/test_layer2_service/CMakeLists.txt
add_executable(test_layer2_service
    test_backoff_strategy.cpp
    test_lifecycle.cpp
    test_crypto_utils.cpp
    test_file_lock.cpp
    test_logger.cpp
    workers/filelock_workers.cpp
    workers/worker_dispatcher.cpp
)

target_link_libraries(test_layer2_service
    PRIVATE
        pylabhub::test_framework
        pylabhub::utils
)

target_compile_definitions(test_layer2_service
    PRIVATE LOGGER_COMPILE_LEVEL=0
)

gtest_discover_tests(test_layer2_service
    WORKING_DIRECTORY "${PYLABHUB_STAGING_DIR}/tests"
)
```

---

## Refactoring Implementation Plan

### Phase 0: Infrastructure Setup (Week 1, Days 1-2)
**Goal**: Prepare test infrastructure and helpers

1. ✅ Create new directory structure:
   - `mkdir test_layer{0,1,2,3}_*/`
2. ✅ Enhance `shared_test_helpers.h`:
   - Add `make_test_channel_name()`
   - Add `cleanup_test_datablock()`
   - Add `DataBlockTestGuard` RAII
   - Add schema verification helpers
3. ✅ Create CMakeLists.txt templates for each layer
4. ✅ Document test naming conventions

**Deliverable**: Test infrastructure ready for migration

### Phase 1: Layer 0 (Platform) Tests (Week 1, Days 2-3)
**Goal**: Create and validate Layer 0 tests (no dependencies)

1. ✅ Create `test_layer0_platform/`:
   - `test_platform_detection.cpp`
   - `test_platform_process.cpp`
   - `test_platform_timing.cpp`
   - `test_platform_info.cpp`
2. ✅ Configure CMakeLists.txt
3. ✅ Run tests, verify 100% pass
4. ✅ Measure coverage (aim for >90%)

**Deliverable**: Layer 0 fully tested, blocking issues resolved

### Phase 2: Layer 1 (Base) Tests (Week 1, Days 3-4)
**Goal**: Migrate and validate Layer 1 tests

1. ✅ Create `test_layer1_base/`
2. ✅ Migrate from `test_pylabhub_corelib/`:
   - Rename files to match conventions
   - Fix any API issues
3. ✅ Add missing tests:
   - `test_debug_info.cpp`
   - `test_module_def.cpp`
4. ✅ Run tests, verify 100% pass

**Deliverable**: Layer 1 fully tested

### Phase 3: Layer 2 (Service) Tests (Week 1, Day 5 - Week 2, Day 2)
**Goal**: Create new critical tests, migrate existing

1. ✅ Create `test_layer2_service/`
2. ✅ **CRITICAL**: Create `test_backoff_strategy.cpp` (NEW)
3. ✅ **CRITICAL**: Create `test_crypto_utils.cpp` (NEW)
4. ✅ Migrate existing tests:
   - `test_lifecycle.cpp`
   - `test_lifecycle_dynamic.cpp`
   - `test_file_lock.cpp`
   - `test_logger.cpp` (consider splitting)
5. ✅ Fix API issues in migrated tests
6. ✅ Run tests, verify 100% pass

**Deliverable**: Layer 2 fully tested, blocking issues resolved

### Phase 4: Layer 3 (DataHub) Tests - Schema (Week 2, Days 2-4) **PRIORITY**
**Goal**: Complete P9.2 schema validation testing

1. ✅ Create `test_layer3_datahub/`
2. ✅ **CRITICAL**: Create `test_schema_blds.cpp` (NEW)
   - 15+ tests for BLDS generation
3. ✅ **CRITICAL**: Refactor `test_schema_validation.cpp`
   - Fix API usage
   - Expand to 10+ comprehensive tests
4. ✅ Run schema tests, verify 100% pass

**Milestone**: P9.2 COMPLETE - schema validation fully tested

### Phase 5: Layer 3 (DataHub) Tests - Core (Week 2, Days 4-5)
**Goal**: Refactor core DataBlock tests

1. ✅ Migrate `test_json_config.cpp`
2. ✅ Migrate `test_message_hub.cpp`
3. ✅ Create `test_datablock_mutex.cpp` (rename + verify)
4. ✅ Create `test_shared_memory_spinlock.cpp` (NEW)
5. ✅ Create `test_datablock_core.cpp` (split from test_datablock.cpp)
6. ✅ Fix API issues in all tests

**Deliverable**: DataBlock core functionality tested

### Phase 6: Layer 3 (DataHub) Tests - Transactions (Week 3, Days 1-2)
**Goal**: Consolidate transaction tests

1. ✅ Create `test_datablock_slot_coordination.cpp` (split from test_datablock.cpp)
2. ✅ Create `test_datablock_transactions.cpp`:
   - Merge test_datablock.cpp + test_transaction_api.cpp
   - Remove pImpl access
   - Fix API issues
   - Add schema-aware tests
3. ✅ Run tests, verify 100% pass

**Deliverable**: Transaction API fully tested

### Phase 7: Layer 3 (DataHub) Tests - Advanced (Week 3, Days 2-4)
**Goal**: Add missing advanced tests

1. ✅ Create `test_datablock_ringbuffer.cpp` (NEW)
2. ✅ Create `test_datablock_checksum.cpp` (NEW)
3. ✅ Create `test_datablock_multiprocess.cpp` (NEW)
4. ✅ Migrate `test_datablock_benchmarks.cpp`

**Deliverable**: DataHub layer fully tested

### Phase 8: Cleanup & Documentation (Week 3, Days 4-5)
**Goal**: Finalize refactoring, document

1. ✅ Delete old test directories:
   - `test_pylabhub_corelib/` (replaced by test_layer{0,1}_*)
   - `test_pylabhub_utils/` (replaced by test_layer{2,3}_*)
2. ✅ Update top-level CMakeLists.txt
3. ✅ Handle `test_layer3_recovery/`:
   - Add GTEST_SKIP() to all tests with P10+ markers
   - Configure as optional (BUILD_RECOVERY_TESTS=OFF by default)
4. ✅ Create documentation:
   - `LAYERED_TEST_GUIDE.md` - How to add tests, conventions
   - `RUNNING_TESTS.md` - How to run specific layers
5. ✅ Run full test suite: `ctest --test-dir build`
6. ✅ Measure coverage: `lcov` or similar
7. ✅ Verify CI integration

**Deliverable**: Refactoring complete, documented, passing

---

## Success Criteria

### Technical
- ✅ All tests pass: `ctest --test-dir build` returns 0
- ✅ No test interdependencies (can run in any order)
- ✅ No shared memory leaks between tests
- ✅ Test coverage ≥ 80% for critical paths
- ✅ CI integration works (GitHub Actions, etc.)

### Organizational
- ✅ Test file structure mirrors source structure
- ✅ Layer dependencies respected (no Layer 3 tests in Layer 2 suite)
- ✅ Naming conventions followed consistently
- ✅ Test isolation enforced (unique names, cleanup)
- ✅ Documentation complete and up-to-date

### P9.2 Specific
- ✅ Schema validation has ≥10 comprehensive tests
- ✅ BLDS generation has ≥15 tests
- ✅ All schema tests use current API
- ✅ Schema tests achieve >90% coverage of schema_blds.hpp

---

## Estimated Effort

| Phase | Effort | Blocking? |
|-------|--------|-----------|
| Phase 0: Infrastructure | 8 hours | YES |
| Phase 1: Layer 0 Tests | 8 hours | YES |
| Phase 2: Layer 1 Tests | 6 hours | YES |
| Phase 3: Layer 2 Tests | 16 hours | YES |
| Phase 4: Schema Tests (P9.2) | 12 hours | **CRITICAL** |
| Phase 5: DataHub Core | 10 hours | NO |
| Phase 6: Transactions | 8 hours | NO |
| Phase 7: Advanced | 12 hours | NO |
| Phase 8: Cleanup & Docs | 6 hours | NO |
| **TOTAL** | **86 hours** | **50 hours blocking** |

**Critical Path for P9.2**: Phases 0-4 (50 hours / ~1.5 weeks)

---

## Long-Term Benefits

### Maintainability
- Clear organization makes finding tests trivial
- Layer isolation prevents cascading test failures
- Naming conventions reduce cognitive load

### Scalability
- Easy to add new tests (clear template per layer)
- New modules naturally fit into layer structure
- Worker process pattern scales to complex scenarios

### Reliability
- Layer dependencies ensure solid foundation
- Test isolation eliminates flakiness
- Comprehensive coverage catches regressions

### Collaboration
- New contributors understand structure immediately
- Code review easier with clear test organization
- Documentation provides onboarding path

---

## Next Actions

**Immediate** (Today):
1. Review and approve this architecture
2. Decide on Phase 0-4 priority (P9.2 blocking)
3. Start Phase 0: Infrastructure setup

**Short-term** (This Week):
4. Complete Phases 0-3 (Platform, Base, Service)
5. Complete Phase 4: Schema validation tests (P9.2)

**Medium-term** (Next 2 Weeks):
6. Complete Phases 5-7 (DataHub full coverage)
7. Complete Phase 8: Cleanup and documentation

---

## Open Questions

1. **Recovery Tests**: Confirm GTEST_SKIP() approach for P10+ tests?
2. **Test Coverage Tool**: Use lcov, gcov, or other?
3. **CI Configuration**: GitHub Actions, Jenkins, other?
4. **Performance Benchmarks**: Include in regular CI or separate?
5. **Umbrella Header for Recovery**: Create `plh_recovery.hpp` now or defer to P10?

---

## Conclusion

This layered test architecture provides a solid, maintainable foundation for PyLabHub testing. By aligning test structure with module architecture and respecting layer dependencies, we ensure that each component is thoroughly validated before higher layers depend on it.

The critical path for P9.2 completion is Phases 0-4 (infrastructure + schema tests), estimated at 50 hours. After that, the remaining DataHub tests can be completed incrementally without blocking further development.

**Recommendation**: Proceed with Phase 0 (Infrastructure Setup) immediately, then execute Phases 1-4 sequentially to complete P9.2 validation.
