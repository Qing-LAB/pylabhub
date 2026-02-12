# Data Exchange Hub - Implementation Setup Summary

**Date:** 2026-02-10
**Status:** In progress (Phase 0 partial, recovery/diagnostics building)

---

## What Was Created

I've analyzed your codebase structure and created three comprehensive documents to guide the Data Exchange Hub implementation:

### 1. **IMPLEMENTATION_GUIDANCE.md** (7,800 lines)

**Purpose**: Permanent reference guide for implementation patterns and best practices.

**Key Sections**:
- Architecture principles (Dual-Chain, Two-Tier Synchronization)
- Codebase structure (layered headers, source organization)
- **Integration with existing services** (Lifecycle, Logger, Platform, Debug)
- ABI stability guidelines (pImpl idiom)
- Memory management patterns (RAII, memory ordering)
- Error handling strategy
- Testing strategy (multi-process, ThreadSanitizer)
- Common pitfalls and solutions
- Code review checklist

**Use When**: Writing new code, reviewing PRs, onboarding new developers

### 2. **DATAHUB_TODO.md** (10,500 lines)

**Purpose**: Living task list updated as implementation progresses.

**Organized by Phase**:
- **Phase 0**: Code Refactoring and Service Integration (🔴 CRITICAL - see below)
- **Phase 1**: P9 Schema Validation (Week 1)
- **Phase 2**: Core SlotRWCoordinator (Week 2)
- **Phase 3**: DataBlock Factory (Week 2)
- **Phase 4**: MessageHub and Broker (Week 2-3)
- **Phase 5**: P8 Error Recovery (Week 3)
- **Phase 6**: Testing (Week 4)
- **Phase 7**: Deployment (Week 5)

**Use When**: Planning work, tracking progress, updating status

### 3. **DATAHUB_IMPLEMENTATION_SUMMARY.md** (This Document)

**Purpose**: Quick overview and getting started guide.

---

## Key Insight: Code Refactoring (Phase 0)

### Your Feedback

> "Some of the helper functions for the data hub module could be integrated/moved to other service modules in this project such that the logic and design of the whole project would be improved and separately tested."

**You are absolutely correct!** I've identified this as **Phase 0 (Priority 0.1)** in the TODO list.

### Why This Is Critical

The current DataHub implementation has several helper functions that should be **generalized and moved to appropriate service modules**:

#### 1. **Platform Service Enhancements** (`src/utils/platform.cpp`)

**Move from DataHub → Platform**:

```cpp
// BEFORE (in shared_memory_spinlock.cpp, data_block.cpp)
static bool is_process_alive(uint64_t pid) { /* ... */ }
static uint64_t now_ns() { /* ... */ }
static void* shm_open_wrapper(...) { /* ... */ }

// AFTER (in platform.cpp, reusable everywhere)
namespace pylabhub::platform {
    PYLABHUB_UTILS_EXPORT bool is_process_alive(uint64_t pid);
    PYLABHUB_UTILS_EXPORT uint64_t monotonic_time_ns();
    PYLABHUB_UTILS_EXPORT void* shm_create(const char* name, size_t size);
    PYLABHUB_UTILS_EXPORT void* shm_attach(const char* name, size_t size);
}
```

**Benefits**:
- FileLock can use `is_process_alive()` for PID-based locking
- Logger can use `monotonic_time_ns()` for timestamps
- Other IPC modules can use `shm_*` functions

#### 2. **Crypto Utilities Module** (NEW: `src/utils/crypto_utils.cpp`)

**Move from DataHub → Crypto Utils**:

```cpp
// BEFORE (in anonymous namespace in data_block.cpp)
static bool compute_blake2b(uint8_t* out, const void* data, size_t len) { /* ... */ }
static bool verify_blake2b(const uint8_t* stored, const void* data, size_t len) { /* ... */ }

// AFTER (in crypto_utils.cpp, reusable everywhere)
namespace pylabhub::crypto {
    PYLABHUB_UTILS_EXPORT bool compute_blake2b(uint8_t* out, const void* data, size_t len);
    PYLABHUB_UTILS_EXPORT bool verify_blake2b(const uint8_t* stored, const void* data, size_t len);
    PYLABHUB_UTILS_EXPORT void generate_random_bytes(uint8_t* out, size_t len);
}
```

**Benefits**:
- Single libsodium initialization point (via Lifecycle)
- JsonConfig can use checksums for config file integrity
- Logger can use for encrypted log files
- MessageHub can use for ZeroMQ keypair generation

#### 3. **Backoff Strategy Module** (NEW: `src/include/utils/backoff_strategy.hpp`)

**Move from DataHub → Backoff Strategy**:

```cpp
// BEFORE (in anonymous namespace in data_block.cpp)
static void backoff(int iteration) {
    if (iteration < 4) std::this_thread::yield();
    else if (iteration < 10) std::this_thread::sleep_for(std::chrono::microseconds(1));
    else std::this_thread::sleep_for(std::chrono::microseconds(iteration * 10));
}

// AFTER (in backoff_strategy.hpp, header-only, reusable everywhere)
namespace pylabhub::utils {
    class ExponentialBackoff {
        void operator()(int iteration) { /* ... */ }
    };
}
```

**Benefits**:
- FileLock can use for lock acquisition retries
- SharedSpinLock can use (already using ad-hoc version)
- MessageHub can use for ZeroMQ reconnect backoff
- Testable in isolation

#### 4. **Debug Info Enhancements** (`src/utils/debug_info.cpp`)

**Move from DataHub → Debug Info**:

Timeout reporting should live in the module where the timeout occurs (e.g. data_block, file_lock, message_hub), using the Logger module (e.g. LOGGER_WARN) so reporting is persistent and not compiled out. Do not add a generic timeout reporter in debug_info; debug_info is low-level and has no Logger dependency.

### Implementation Order

**Do Phase 0 FIRST** (before other phases):

1. **Day 1-2**: Refactor platform utilities
   - Move `is_process_alive()`, `monotonic_time_ns()`, `shm_*` functions
   - Update all existing usages (DataBlock, FileLock)

2. **Day 2**: Create crypto_utils module
   - Move BLAKE2b functions
   - Integrate with Lifecycle (libsodium init)

3. **Day 3**: Create backoff_strategy module
   - Templatized, header-only
   - Update SharedSpinLock, SlotRWState to use it

**Why First?**
- Reduces coupling in DataHub code
- Makes DataHub implementation cleaner
- Allows parallel development (one person on DataHub, another on utilities)
- Improves testability (test utilities independently)

---

## Getting Started

### Step 1: Review Documents

1. Read **IMPLEMENTATION_GUIDANCE.md** (focus on Sections 3-4: Structure and Integration)
2. Read **DATAHUB_TODO.md** Phase 0 (Code Refactoring section)
3. Review existing code:
   - `src/include/utils/data_block.hpp` (API surface)
   - `src/utils/data_block.cpp` (partial implementation)
   - `src/utils/shared_memory_spinlock.cpp` (SharedSpinLock)

### Step 2: Start with Phase 0 (Refactoring)

**First PR: Platform Utilities**
- Move `is_process_alive()` to `platform.cpp`
- Move `monotonic_time_ns()` to `platform.cpp`
- Add unit tests for these functions
- Update DataHub code to use them

**Second PR: Crypto Utilities**
- Create `src/utils/crypto_utils.cpp` and header
- Move BLAKE2b functions
- Register with Lifecycle
- Add unit tests

**Third PR: Backoff Strategy**
- Create `src/include/utils/backoff_strategy.hpp`
- Update SharedSpinLock to use it
- Add unit tests

**Estimated Time**: 2-3 days total

### Step 3: Continue with Phase 1 (P9 Schema Validation)

See **DATAHUB_TODO.md** Section "Phase 1: P9 Schema Validation"

---

## Current Codebase Status

### ✅ Already Implemented

- Layered umbrella headers (plh_platform, plh_base, plh_service, plh_datahub)
- Lifecycle management system
- Logger with multiple sinks
- FileLock (cross-platform)
- Platform utilities: get_pid, get_thread_id, get_version, **is_process_alive**, **monotonic_time_ns** (steady_clock)
- Debug info (stack traces)
- SharedMemoryHeader structure (with P9 fields)
- SlotRWState structure
- SharedSpinLock (basic implementation; uses platform::is_process_alive)
- Transaction guards (API defined, partial implementation)
- **Slot RW C API** (slot_rw_acquire_write/commit/release_write, acquire_read/validate_read/release_read, metrics; optional header)
- **DataBlock diagnostic API** (DataBlockDiagnosticHandle, open_datablock_for_diagnostic)
- P8 Recovery API (defined; recovery/diagnostics modules in build; datablock_validate_integrity, force_reset, release_zombie_* wired)
- MessageHub (connect, send_message, receive_message, get_instance; recv_multipart return checked)

### ⚠️ Partially Implemented

- DataBlock (core exists; factory/consumer paths in use)
- MessageHub (register_consumer stub; broker discovery)
- SlotRWCoordinator (C API in use; C++ wrappers in slot_rw_access.hpp)

### ❌ Not Yet Implemented

- P9 Schema Validation (design complete, implementation pending)
- Broker service
- Complete SlotRWState acquisition/release logic
- DataBlock factory functions
- Heartbeat management
- Integrity validation
- Comprehensive tests

---

## Code Quality Standards

### Must Follow

1. **ABI Stability**: All public classes use pImpl idiom
2. **Memory Ordering**: Explicit `memory_order_acquire`/`release` for atomics
3. **Logging**: Use LOGGER_* macros, not std::cout
4. **Platform Abstraction**: Use pylabhub::platform functions
5. **Lifecycle Integration**: Register modules with GetLifecycleModule()
6. **Code Style**: LLVM-based, Allman braces, 4-space indent, 100-char lines
7. **Testing**: Unit tests for all new functions, multi-process for IPC

### Before Each Commit

- [ ] Run `./tools/format.sh`
- [ ] Run `cmake --build build` (no warnings)
- [ ] Run `ctest --test-dir build --output-on-failure`
- [ ] Update TODO.md with progress
- [ ] Update IMPLEMENTATION_GUIDANCE.md if design changed

---

## Questions to Ask Yourself

Before implementing a new function:

1. **Can this function be moved to a service module?**
   - Is it generally useful beyond DataHub?
   - Does it fit in Platform, Debug, Crypto, or Backoff?

2. **Does this break ABI?**
   - Am I adding STL members to a public class?
   - Am I changing public method signatures?

3. **Is memory ordering correct?**
   - Will this work on ARM?
   - Did I use `memory_order_acquire`/`release`?

4. **Is this tested?**
   - Did I write a unit test?
   - Did I test multi-process scenarios?

5. **Is this logged?**
   - Did I log errors with LOGGER_ERROR?
   - Did I log important events with LOGGER_INFO?

---

## Estimated Timeline (5 Weeks)

| Week | Focus | Deliverables |
|------|-------|--------------|
| **1** | Phase 0 + P9 | Refactored utilities, Schema validation |
| **2** | Core DataBlock | SlotRWState API, Factories |
| **3** | MessageHub + Recovery | Broker, P8 Recovery complete |
| **4** | Testing | All tests passing, TSan clean |
| **5** | Deployment | Python bindings, docs, prod-ready |

---

## Next Steps (Immediate)

1. **Review this summary** and the two companion documents
2. **Start Phase 0, Priority 0.1**: Move `is_process_alive()` to platform.cpp
3. **Update TODO.md** as you complete tasks (mark with ✅)
4. **Ask questions** if any design decision is unclear

---

## Files Created

- `docs/IMPLEMENTATION_GUIDANCE.md` - Permanent reference guide
- `docs/DATAHUB_TODO.md` - Living task list
- `docs/DATAHUB_IMPLEMENTATION_SUMMARY.md` - This file (quick start)

---

## Maintenance

### Update TODO.md After Each Task

```markdown
- [x] **Move PID liveness check** to platform.cpp (✅ 2026-02-10)
```

### Update IMPLEMENTATION_GUIDANCE.md If Design Changes

Example: If you discover a better synchronization pattern, document it in Section 9 (Common Pitfalls).

---

**Ready to begin!** Start with Phase 0 to improve overall code quality, then proceed with P9 and core DataBlock implementation. 🚀
