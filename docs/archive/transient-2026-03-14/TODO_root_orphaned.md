# PyLabHub C++ TODO

Reference this file for outstanding work. Details in docs/ subdirectory.

---

## Current Branch: `feature/data-hub`

---

## IMMEDIATE: Build & Test Verification (BLOCKED - not yet built)

The Layer 2 test infrastructure was refactored this session but **NOT YET BUILT OR TESTED**.

### Must verify these compile and pass:
```bash
cmake -S . -B build
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
# Also run executables directly to validate pattern correctness:
./build/stage-debug/tests/test_layer2_lifecycle
./build/stage-debug/tests/test_layer2_lifecycle_dynamic
./build/stage-debug/tests/test_layer2_filelock_singleprocess
```

### Known issues to watch for:
- `test_filelock_singleprocess.cpp` has a syntax error in `TimedLock` test (closing `});` on wrong line - must fix)
- Layer 2 executables must NOT link `GTest::gtest_main` (test_framework provides main())
- Layer 2 lifecycle/filelock/logger/jsonconfig executables need `layer2_worker_dispatcher.cpp` + worker sources

---

## Layer 2 Test Executables - Status

| Executable | Pattern | Worker Dispatcher | Status |
|---|---|---|---|
| test_layer2_backoff_strategy | 1/2 | no | needs build verify |
| test_layer2_crypto_utils | 1/2 | no | needs build verify |
| test_layer2_lifecycle | 3 | yes (lifecycle_workers.cpp) | needs build verify |
| test_layer2_lifecycle_dynamic | 3 | yes (lifecycle_workers.cpp) | needs build verify |
| test_layer2_filelock | 3 | yes (filelock_workers.cpp) | needs build verify |
| test_layer2_filelock_singleprocess | 2 | no | NEW - needs build verify |
| test_layer2_logger | 3 | yes (logger_workers.cpp) | needs build verify |
| test_layer2_jsonconfig | 3 | yes (jsonconfig_workers.cpp) | needs build verify |

---

## Test Pattern Framework (see docs/TEST_PATTERN_GUIDELINES.md)

Three patterns are defined in `tests/test_framework/test_patterns.h`:

### Pattern 1: PureApiTest
- No lifecycle, no I/O, pure function testing
- Use `class MyTest : public pylabhub::tests::PureApiTest`

### Pattern 2: Lifecycle-Managed (plain `::testing::Test`)
- Lifecycle initialized in `main()` by test_framework (Logger + FileLock + JsonConfig)
- Tests run in SAME process - safe as long as no shutdown/finalize called
- IMPORTANT: CTest hides Pattern issues (each test gets new process). Run executable directly to verify.

### Pattern 3: Multi-Process (WorkerProcess)
Use Pattern 3 when ANY of these apply:
1. Test expects process crash/abort
2. Test checks state BEFORE lifecycle initialization
3. True multi-process IPC (file locks, shared memory BETWEEN processes)
4. Test calls `finalize()`, `shutdown()`, or irreversibly modifies singleton state
5. Test needs completely fresh lifecycle state (dynamic module load/unload)
6. Chaos/robustness testing (e.g., shutdown while threads active)

**CRITICAL:** `test_concurrent_lifecycle_chaos` MUST be Pattern 3 because it calls `LifecycleManager::instance().finalize()` - would corrupt subsequent tests in same process.

---

## Worker Process Architecture

- `test_framework/test_entrypoint.cpp` - provides custom `main()` with worker dispatch
- `test_framework/test_entrypoint.h` - declares `g_self_exe_path`, `register_worker_dispatcher()`
- `test_layer2_service/layer2_worker_dispatcher.cpp` - registers all Layer 2 workers
- Worker sources shared from `test_pylabhub_utils/` (lifecycle/filelock/logger/jsonconfig _workers.cpp)
- Executables using Pattern 3 must NOT link `GTest::gtest_main`

Worker mode invocation: `./executable module.scenario [args...]`

---

## Logger Tests - Pattern Classification

| Test | Pattern | Reason |
|---|---|---|
| BasicLogging | **3** | Logger shutdown in worker would break same-process tests |
| LogLevelFiltering | 2* | Could run in-process with level restore |
| MultithreadStress | 2* | Threads only, no shutdown |
| FlushWaitsForQueue | 2* | No shutdown |
| ShutdownIdempotency | **3** | Calls Logger::shutdown() |
| ReentrantErrorCallback | **3** | Calls shutdown |
| ConcurrentLifecycleChaos | **3** | Calls finalize() |
| InterProcessFlock | **3** | True multi-process |
| RotatingFileSink | 2* | No shutdown |
| QueueFullAndMessageDropping | **3** | Queue exhaustion |

*2* = could be Pattern 2 but must verify no shutdown path

---

## Outstanding Tests (Lower Priority)

### test_pylabhub_utils (legacy suite - keep working, migrate gradually)
- `test_datablock.cpp` - needs review for API compatibility
- `test_transaction_api.cpp` - needs API usage fixes
- `test_schema_validation.cpp` - needs current DataBlock API
- `test_recovery_api.cpp` - stub until P10+ implemented

### Layer 3 DataHub Tests (tests/test_layer3_datahub/)
- `test_schema_blds.cpp` - needs build verify
- `test_schema_validation.cpp` - needs current DataBlock API

---

## Architecture Reference

### Dual Library Structure
- `pylabhub-basic` (static): low-level, NO dependencies. DOES NOT EXIST as CMake target `pylabhub::basic` - headers only, integrated into utils
- `pylabhub-utils` (shared): Logger, FileLock, Lifecycle, JsonConfig, MessageHub, DataBlock

### Key Design Rules
- Pimpl idiom mandatory for all public classes in pylabhub-utils
- All struct size constants must be in `detail::` namespace with version association (NO MAGIC NUMBERS)
- `SharedMemoryHeader` must be exactly 4096 bytes (static assertion in data_block.hpp)
- PLH_DEBUG deliberately has no source_location (variadic template issue) - do NOT add it

### DataBlock Version Constants (data_block.hpp detail:: namespace)
```cpp
detail::HEADER_VERSION_MAJOR = 1
detail::HEADER_VERSION_MINOR = 0
detail::MAX_SHARED_SPINLOCKS = 8
detail::MAX_CONSUMER_HEARTBEATS = 8
detail::MAX_FLEXIBLE_ZONE_CHECKSUMS = 8
```

---

## Files Changed This Session

### New Files
- `tests/test_layer2_service/layer2_worker_dispatcher.cpp` - unified Layer 2 worker dispatcher
- `tests/test_layer2_service/test_filelock_singleprocess.cpp` - Pattern 2 FileLock examples
- `docs/CTEST_VS_DIRECT_EXECUTION.md` - critical doc on CTest vs direct execution behavior
- `docs/TEST_PATTERN_GUIDELINES.md` - pattern decision tree
- `docs/TEST_PATTERN_ANALYSIS.md` - migration plan
- `docs/FILELOCK_TEST_PATTERNS.md` - FileLock-specific pattern guide
- `docs/TEST_FRAMEWORK_IMPROVEMENTS.md` - summary of improvements

### Modified Files
- `tests/test_layer2_service/CMakeLists.txt` - added worker sources, removed gtest_main, added filelock_singleprocess
- `tests/test_framework/test_patterns.h` - clarified LifecycleManagedTest usage (user modified)

---

## Build Commands Reference

```bash
cmake -S . -B build                    # configure
cmake --build build -j$(nproc)         # build all
cmake --build build --target X -j$(nproc)  # build specific target
ctest --test-dir build --output-on-failure  # run all tests
ctest --test-dir build -R "pattern"    # run matching tests
./build/stage-debug/tests/test_X       # run directly (validates pattern correctness)
```
