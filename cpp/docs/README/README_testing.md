# C++ Test Suite Architecture

This document outlines the architecture of the pyLabHub C++ test suite. Its goal is to ensure that tests are organized, scalable, and easy for developers to write and run.

## Table of Contents
1. [High-Level Philosophy](#high-level-philosophy)
2. [Test Suite Structure](#test-suite-structure)
3. [Quick Start: Running Tests](#quick-start-running-tests)
4. [How to Add a New Test](#how-to-add-a-new-test)
5. [Multi-Process Testing Deep Dive](#a-deep-dive-how-multi-process-testing-works)
6. [Test Staging and Dependencies](#test-staging-and-dependencies)
7. [Platform-Specific Behavior](#platform-specific-behavior-and-gotchas)

## Related Documents

- [README_Versioning.md](README_Versioning.md) — Version scheme; version API tests live in `test_platform.cpp`
- **`docs/DATAHUB_TODO.md`** — Execution order and priorities for DataHub; test checklist and Phase C/D items live there.
- **`docs/IMPLEMENTATION_GUIDANCE.md`** — Testing strategy, test patterns, and **MessageHub code review** (DataHub integration).

---

## 1. High-Level Philosophy

Our test suite is built on three core principles:

1.  **Clarity**: Test code should be as readable and well-organized as the production code it validates.
2.  **Dependency Isolation**: Tests for base utilities (Layer 1) should not depend on full `pylabhub-utils` when testing foundational types (e.g. spin state/SpinGuard, `recursion_guard`, `scope_guard`).
3.  **Speed**: A fast "inner loop" is critical. Developers must be able to run only the tests relevant to their changes without waiting for a full suite build.

To achieve this, we use a **multiple-executable model**, where different test categories have their own dedicated test executables.

## 2. Test Suite Structure

The test suite is composed of several distinct **CMake targets** located in the `tests/` directory:

| Target | Type | Contents | Purpose |
|--------|------|----------|---------|
| `pylabhub-test-framework` | Static Library | Shared test infrastructure (`test_entrypoint.cpp`, `test_process_utils.cpp`, `shared_test_helpers.cpp`) | Common functionality for all test executables |
| **Layer 0** `test_layer0_platform` | Executable | Platform: `get_pid`, `monotonic_time_ns`, `is_process_alive`, `shm_*`, debug | Platform and version API |
| **Layer 1** `test_layer1_*` | Executables | `test_spinlock`, `test_recursion_guard`, `test_scope_guard`, `test_formattable` (spin state, SpinGuard, RecursionGuard, ScopeGuard, format tools) | Base utilities; depend only on staged utils |
| **Layer 2** `test_layer2_*` | Executables | Lifecycle, FileLock, Logger, JsonConfig, SharedSpinLock, CryptoUtils, Backoff, etc. (each test links only the workers it uses) | Service-layer tests; multi-process workers for FileLock, Logger, etc. |
| **Layer 3** `test_layer3_datahub` | Executable | DataHub: schema BLDS/validation, recovery API, slot protocol, phase A, error handling, MessageHub, DataBlockMutex (single executable, multiple worker files) | DataBlock/MessageHub tests; acts as worker process for multi-process tests |

This structure lets you build and run only the layer you need (e.g. `test_layer1_spinlock` or `test_layer3_datahub`) for a faster development cycle.

## 3. Quick Start: Running Tests

### Build and Stage Tests

```bash
# From project root
cd cpp/build

# Build and stage all tests (requires staged dependencies)
cmake --build . --target stage_tests

# Or if PYLABHUB_STAGE_ON_BUILD=ON (default), just build:
cmake --build .
```

Tests are staged to `build/stage-debug/tests/` along with all required DLLs and shared libraries.

### Run All Tests

```bash
# From build directory
ctest

# With output
ctest --output-on-failure

# Verbose
ctest -V
```

### Run Specific Test Suites

```bash
# Run all tests in InProcessSpinStateTest suite (spin state + SpinGuard)
ctest -R "^InProcessSpinStateTest"

# Run a single test case
ctest -R "^InProcessSpinStateTest.BasicAcquireRelease$"

# Run all Lifecycle tests
ctest -R "^LifecycleTest"

# Run all DataHub tests (Layer 3)
ctest -R "test_layer3_datahub"

# Run all multi-process tests
ctest -R "MultiProcess"
```

### Run Tests from Staged Directory

```bash
# Navigate to staged test directory
cd build/stage-debug/tests

# Run Layer 0 (platform)
./test_layer0_platform

# Run Layer 2 service tests with filter
./test_layer2_filelock --gtest_filter=FileLockTest.*

# Run Layer 3 DataHub tests
./test_layer3_datahub --gtest_filter=SlotProtocolTest.*

# List available tests
./test_layer3_datahub --gtest_list_tests

# Run with repeat for stress testing
./test_layer1_spinlock --gtest_repeat=100 --gtest_filter=InProcessSpinStateTest.*
```

### Practical Testing Workflows

**Workflow 1: Test-Driven Development**
```bash
# Edit code in src/utils/file_lock.cpp
# Edit test in tests/test_pylabhub_utils/test_filelock.cpp

# Quick rebuild and test
cmake --build build --target test_pylabhub_utils
cd build/stage-debug/tests
./test_pylabhub_utils --gtest_filter=FileLockTest.BasicLocking
```

**Workflow 2: Debugging Test Failures**
```bash
# Run single test with debugger
cd build/stage-debug/tests
gdb ./test_pylabhub_utils
(gdb) run --gtest_filter=FileLockTest.FailingTest
```

**Workflow 3: Continuous Integration**
```bash
# Build everything
cmake --build build

# Run all tests with XML output
cd build
ctest --output-junit results.xml --output-on-failure
```

## 9. A Deep Dive: How Multi-Process Testing Works

The multi-process testing logic is used for components like `pylabhub::utils::FileLock`, `JsonConfig`, and DataBlock. A test executable (e.g. `test_layer2_filelock` or `test_layer3_datahub`) can act as both a **"Parent"** (the test runner) and a **"Worker"** (a child process spawned to perform a specific task).

**FileLock tests and `.lock` files:** Tests use `clear_lock_file()` to remove lock files *before* each test run, ensuring isolation. In production, `.lock` files are harmless if left on disk; the library does not remove them on shutdown. If cleanup is desired (e.g., after a crash), use an external script when nothing is running.

This is managed by three key components in the `pylabhub-test-framework`:
1.  **`test_entrypoint.cpp`**: Provides the `main()` function for test executables. It checks command-line arguments. If a "worker mode" argument is present, it calls a registered dispatcher. Otherwise, it runs GoogleTest (`RUN_ALL_TESTS()`).
2.  **`test_process_utils.h`**: Provides the `WorkerProcess` RAII class, which is the primary tool for spawning and managing child processes. It handles argument passing, redirects the worker's stdout/stderr to files for inspection, and ensures the worker is terminated.
3.  **Worker dispatcher** (e.g. in each layer’s `workers/` and the test executable’s `main()`): Maps worker mode strings (e.g. `"filelock.nonblocking_acquire"`) to C++ worker functions so the child process runs the right code.

### Step-by-Step Execution Flow

This sequence diagram illustrates the flow for a multi-process `FileLock` test.

```mermaid
sequenceDiagram
    participant CTest
    participant Parent as test_layer2_filelock (Parent)
    participant Child as test_layer2_filelock (Worker)

    Note over CTest, Parent: Step 1: CTest runs the test executable.
    CTest->>Parent: ./test_layer2_filelock

    Note over Parent: Step 2: Parent process starts.
    Parent->>Parent: main() in 'test_entrypoint.cpp' is called.<br/>No worker args found, so it calls RUN_ALL_TESTS().

    Note over Parent: Step 3: GoogleTest runs a specific test case.
    Parent->>Parent: Executes TEST_F(FileLockTest, MultiProcessNonBlocking).

    Note over Parent, Child: Step 4: The Parent spawns a worker process (same executable).
    Parent->>Child: Creates WorkerProcess("filelock.nonblocking_acquire").<br/>fork/CreateProcess with worker argument.

    Note over Child: Step 5: Worker process starts.
    Child->>Child: main() in 'test_entrypoint.cpp' is called.<br/>Sees worker arg "filelock.nonblocking_acquire".

    Note over Child: Step 6: Worker dispatches to the correct function.
    Child->>Child: main() sees worker arg; dispatcher calls worker::filelock::nonblocking_acquire().

    Note over Child: Step 7: Worker executes test logic & exits.
    Child->>Child: The worker function runs its assertions and returns an exit code.

    Note over Parent: Step 8: Parent waits and verifies results.
    Parent->>Parent: proc.wait_for_exit() is called.<br/>Parent checks worker's exit code, stdout, and stderr.
```

## 4. How to Add a New Test

### Example 1: Simple Unit Test

Let's add tests for a new `StringUtils` class in `pylabhub-utils`.

1.  **Create the test file:** `tests/test_pylabhub_utils/test_stringutils.cpp`
    ```cpp
    #include <gtest/gtest.h>
    #include "utils/StringUtils.hpp"  // Your new header
    
    using namespace pylabhub::utils;
    
    // Test suite name should match the class/component being tested
    TEST(StringUtilsTest, TrimWhitespace) {
        EXPECT_EQ(StringUtils::trim("  hello  "), "hello");
        EXPECT_EQ(StringUtils::trim(""), "");
        EXPECT_EQ(StringUtils::trim("   "), "");
    }
    
    TEST(StringUtilsTest, SplitString) {
        auto result = StringUtils::split("a,b,c", ',');
        ASSERT_EQ(result.size(), 3);
        EXPECT_EQ(result[0], "a");
        EXPECT_EQ(result[1], "b");
        EXPECT_EQ(result[2], "c");
    }
    
    TEST(StringUtilsTest, EdgeCases) {
        EXPECT_THROW(StringUtils::parse_int("not-a-number"), std::invalid_argument);
        EXPECT_NO_THROW(StringUtils::parse_int("42"));
    }
    ```

2.  **Update `tests/test_pylabhub_utils/CMakeLists.txt`:**
    ```cmake
    add_executable(test_pylabhub_utils
      # ... existing test files
      test_stringutils.cpp  # <-- Add your new test file here
    )
    ```

3.  **Build and run:**
    ```bash
    cmake --build build --target test_pylabhub_utils
    ctest -R "^StringUtilsTest"
    ```

### Example 2: Test with Fixtures (Setup/Teardown)

For tests that need common setup:

```cpp
// tests/test_pylabhub_utils/test_database.cpp
#include <gtest/gtest.h>
#include "utils/Database.hpp"

using namespace pylabhub::utils;

// Fixture provides setup/teardown for each test
class DatabaseTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Runs before each TEST_F
        temp_db_path = "/tmp/test_db.sqlite";
        db = std::make_unique<Database>(temp_db_path);
        db->init();
    }
    
    void TearDown() override {
        // Runs after each TEST_F
        db.reset();  // Close database
        std::remove(temp_db_path.c_str());  // Clean up
    }
    
    std::string temp_db_path;
    std::unique_ptr<Database> db;
};

// Use TEST_F (not TEST) to access fixture members
TEST_F(DatabaseTest, InitialState) {
    EXPECT_TRUE(db->is_open());
    EXPECT_EQ(db->count_rows("users"), 0);
}

TEST_F(DatabaseTest, InsertAndQuery) {
    db->insert_user("Alice", 30);
    EXPECT_EQ(db->count_rows("users"), 1);
    
    auto user = db->get_user("Alice");
    EXPECT_EQ(user.age, 30);
}
```

### Example 3: Multi-Process Test

For testing components that require multiple processes (like file locks):

```cpp
// tests/test_pylabhub_utils/test_mylock.cpp
#include <gtest/gtest.h>
#include "test_framework/test_process_utils.h"
#include "utils/MyLock.hpp"

using namespace pylabhub::utils;
using namespace pylabhub::test;

TEST(MyLockTest, MultiProcessExclusion) {
    const std::string lock_path = "/tmp/test.lock";
    
    // Parent acquires lock
    MyLock parent_lock(lock_path);
    ASSERT_TRUE(parent_lock.try_acquire());
    
    // Spawn worker process that tries to acquire same lock
    WorkerProcess proc("mylock.try_acquire", {lock_path});
    proc.spawn();
    
    // Wait for worker to finish
    int exit_code = proc.wait_for_exit(std::chrono::seconds(5));
    
    // Worker should fail to acquire (returns exit code 1)
    EXPECT_EQ(exit_code, 1);
    
    // Release parent lock
    parent_lock.release();
}
```

Then create the worker function in `tests/test_pylabhub_utils/mylock_workers.cpp`:
```cpp
#include "mylock_workers.h"
#include "utils/MyLock.hpp"

namespace pylabhub::test::worker::mylock {

int try_acquire(const std::vector<std::string>& args) {
    if (args.empty()) return 2;  // Error: no lock path
    
    MyLock lock(args[0]);
    if (lock.try_acquire()) {
        lock.release();
        return 0;  // Success
    }
    return 1;  // Failed to acquire
}

}  // namespace
```

And register it in `worker_dispatcher.cpp`:
```cpp
#include "mylock_workers.h"

// In dispatch_worker function:
if (worker_name == "mylock.try_acquire") {
    return worker::mylock::try_acquire(args);
}
```

### Choosing a test pattern

Use one of three patterns so tests are isolated and repeatable:

| Pattern | When to use | Process model |
|--------|----------------|----------------|
| **PureApiTest** | Pure functions, no lifecycle/I/O | Single process, no shared state |
| **LifecycleManagedTest** | Logger, FileLock, JsonConfig, in-process concurrency | Single process, shared lifecycle |
| **WorkerProcess** | Multi-process (IPC, file locks between processes, or tests that finalize lifecycle) | Parent spawns worker; worker runs in separate process |

For **FileLock** and **DataBlock** multi-process tests, use **WorkerProcess** and register workers in the test executable’s dispatcher. **CTest** runs each test in a separate process; running the test binary directly runs all tests in one process—prefer CTest or WorkerProcess when tests change global state. See **`docs/IMPLEMENTATION_GUIDANCE.md`** § Testing for CTest vs direct execution.

### Key Testing Patterns

1. **Use descriptive test names:** `TEST(ComponentTest, WhatIsBeingTested)`
2. **One assertion per test (when practical):** Makes failures clear
3. **Use ASSERT_* for prerequisites:** Test stops if assertion fails
4. **Use EXPECT_* for checks:** Test continues even if expectation fails
5. **Test edge cases:** Empty inputs, null pointers, boundary values
6. **Clean up resources:** Use fixtures for proper setup/teardown

## 5. Test Staging and Dependencies

Understanding how test executables are staged and how they find their dependencies is crucial for debugging build issues.

### The Staging Process

When you build tests, the CMake system performs these steps:

1. **Build third-party dependencies** (`stage_third_party_deps`)
   - Builds `libsodium`, `luajit`, `fmt`, `libzmq`, etc.
   - Stages libraries to `build/stage-debug/lib/`
   - Stages headers to `build/stage-debug/include/`

2. **Build internal libraries** (`stage_core_artifacts`)
   - Builds `pylabhub-utils` shared library
   - Stages to `build/stage-debug/lib/` (POSIX) or `build/stage-debug/bin/` (Windows)

3. **Build test framework** (`pylabhub-test-framework`)
   - Static library linked by all test executables

4. **Build test executables** (`stage_tests`)
   - Executables output directly to `build/stage-debug/tests/`
   - On Windows: DLLs are automatically copied to `tests/` directory

### Dependency Resolution

**On Linux/macOS:**
- Test executables use `RPATH` set to `$ORIGIN/../lib`
- When `./test_pylabhub_utils` runs, it looks for `.so` files in `../lib/`
- Example: `tests/test_pylabhub_utils` finds `lib/libpylabhub-utils.so`

**On Windows:**
- Test executables look for `.dll` files in the same directory
- The `stage_tests` target ensures DLLs are copied to `tests/`
- Example: `tests/test_pylabhub_utils.exe` finds `tests/pylabhub-utils.dll`

### Viewing Dependencies

**Linux:**
```bash
cd build/stage-debug/tests
ldd ./test_pylabhub_utils
# Shows:
#   libpylabhub-utils.so => ../lib/libpylabhub-utils.so
#   libfmt.so.10 => ../lib/libfmt.so.10
```

**macOS:**
```bash
cd build/stage-debug/tests
otool -L ./test_pylabhub_utils
# Shows:
#   @rpath/libpylabhub-utils.dylib
#   @rpath/libfmt.10.dylib
```

**Windows:**
```powershell
cd build\stage-debug\tests
dumpbin /DEPENDENTS test_pylabhub_utils.exe
# Shows:
#   pylabhub-utils.dll
#   fmt.dll
```

### Troubleshooting Staging Issues

**Issue: Test fails with "shared library not found"**

Check staging:
```bash
# Are libraries staged?
ls -la build/stage-debug/lib/*.so
ls -la build/stage-debug/lib/*.dylib
ls -la build/stage-debug/tests/*.dll  # Windows

# Does test have correct RPATH? (Linux)
readelf -d build/stage-debug/tests/test_pylabhub_utils | grep RPATH

# Check what libraries test is looking for
ldd build/stage-debug/tests/test_pylabhub_utils  # Linux
otool -L build/stage-debug/tests/test_pylabhub_utils  # macOS
```

**Issue: Test executable not staged**

Check registration:
```bash
# Did CMake find the test?
cd build
cmake .. | grep "test_pylabhub_utils"

# Is it in the staging target dependencies?
cmake --build . --target help | grep stage
```

## 7. Advanced CTest Usage

All tests are managed by CTest. The `gtest_discover_tests` function registers each test with CTest using the format `TestSuiteName.TestName`.

### Useful CTest Commands

```bash
# List all discovered tests (doesn't run them)
ctest -N

# List tests matching pattern
ctest -N -R "^LifecycleTest"

# Run tests with verbose output
ctest -V

# Run tests showing only failures
ctest --output-on-failure

# Run tests in parallel (4 jobs)
ctest -j4

# Run specific test by exact name
ctest -R "^LifecycleTest.InitializeAndFinalize$"

# Exclude tests by pattern
ctest -E "MultiProcess"

# Run tests and generate XML report
ctest --output-junit results.xml

# Run tests with timeout
ctest --timeout 60

# Rerun only failed tests
ctest --rerun-failed

# Run tests in random order (good for finding order dependencies)
ctest --schedule-random
```

### GoogleTest Direct Execution

For more control, run test executables directly:

```bash
cd build/stage-debug/tests

# Run all tests in executable
./test_pylabhub_utils

# Filter by test suite
./test_pylabhub_utils --gtest_filter=FileLockTest.*

# Filter by specific test
./test_pylabhub_utils --gtest_filter=FileLockTest.BasicLocking

# Exclude tests
./test_pylabhub_utils --gtest_filter=-*MultiProcess*

# List available tests
./test_pylabhub_utils --gtest_list_tests

# Run with repeat (stress testing)
./test_pylabhub_utils --gtest_repeat=100

# Break on first failure
./test_pylabhub_utils --gtest_break_on_failure

# Shuffle test order
./test_pylabhub_utils --gtest_shuffle

# Run with specific random seed (for reproducibility)
./test_pylabhub_utils --gtest_shuffle --gtest_random_seed=12345
```

### Integration with IDEs

**Visual Studio Code:**
Configure in `.vscode/settings.json`:
```json
{
    "cmake.ctestArgs": [
        "--output-on-failure"
    ],
    "testMate.cpp.test.executables": "build/stage-*/tests/*{test,Test,TEST}*"
}
```

**CLion:**
1. Go to Run → Edit Configurations
2. Add "Google Test" configuration
3. Set Target: `test_pylabhub_utils`
4. Set Test filter: `FileLockTest.*`

## 8. Platform-Specific Behavior and Gotchas

### Deadlock on Windows When Capturing `stderr`

When writing tests, a common pattern is to capture standard output or standard error to verify what a function writes to the console. The `StringCapture` helper in `pylabhub-test-framework` is designed for this purpose.

However, there is a significant gotcha on **Windows** when testing functions that use the `DbgHelp` library, such as `pylabhub::debug::print_stack_trace`.

**The Problem:**

- The `StringCapture` helper works by redirecting `stderr` to a **pipe** (a fixed-size in-memory buffer).
- The `print_stack_trace` function, on its first use, initializes the `DbgHelp` library (`DbgHelp.dll`).
- This initialization process (`SymInitialize`) is complex and may write its own status messages or errors to `stderr`.
- If the `DbgHelp` output is large enough to fill the pipe's buffer, the call to `print_stack_trace` will **block** (freeze), waiting for the pipe to be read.
- However, the test framework is also blocked, waiting for `print_stack_trace` to finish before it can read the pipe. This creates a **deadlock**.

**Solution:**

For any test that validates the output of `pylabhub::debug::print_stack_trace`, do **not** use `StringCapture`. Instead, redirect `stderr` to a temporary file for the duration of the test. This avoids the blocking behavior of pipes and makes the test robust.

See `PlatformTest.PrintStackTrace` in `tests/test_pylabhub_corelib/test_platform.cpp` for a canonical example of this file-based redirection.

---

## DataHub and MessageHub test plan (Phase A–D)

**Purpose:** (1) Plan the tests required for DataBlock/DataHub implementation and protocol. (2) MessageHub code review for C++20, abstraction, and DataHub integration lives in **`docs/IMPLEMENTATION_GUIDANCE.md`** § MessageHub code review.

**Execution order and priorities** are in **`docs/DATAHUB_TODO.md`**. This section provides test rationale and Phase A–D detail; do not use it as a competing roadmap.

**Cross-platform:** All tests must be runnable on every supported platform (Windows, Linux, macOS, FreeBSD). Avoid “skip on platform X” unless justified and documented.

### Part 0: Foundational APIs used by DataBlock

DataBlock depends on these; their correctness must be covered before relying on DataBlock behavior.

| Foundation | Used by DataBlock for | Coverage |
|------------|------------------------|----------|
| Platform (get_pid, monotonic_time_ns, is_process_alive) | Writer/reader ownership, heartbeat, zombie reclaim | test_platform_core, test_platform_shm |
| shm_create / shm_attach / shm_close / shm_unlink | Segment create/attach | test_platform_shm |
| Backoff, Crypto (BLAKE2b), SharedSpinLock, Lifecycle, Schema BLDS | Spin loops, checksums, zone locking, header hash | test_backoff_strategy, test_crypto_utils, test_shared_memory_spinlock, test_schema_blds |

### Part 1: DataBlock/DataHub test plan

**Scope:** DataBlock core (layout, header, slot coordination, writer/reader acquire/commit/release, flexible zone); Producer/Consumer API (create/find, flexible_zone_span, checksum, acquire/release); protocol and agreement (config, expected_config, schema); integrity and diagnostics; MessageHub (connect, register_producer, discover_producer).

**Phase A – Protocol/API correctness (no broker):** Flexible zone empty/non-empty (producer, consumer, slot handles); checksum false when no zones / true when valid; consumer with/without expected_config; schema store/validate and mismatch fails.

**Phase B – Slot protocol in one process:** Create producer and consumer (same process); acquire write slot, write+commit; acquire consume slot, read; checksum update/verify; optional diagnostic handle.

**Phase C – MessageHub and broker:** No-broker behavior (connect/disconnect, send/receive when disconnected, parse errors); with-broker: register_producer, discover_producer, one write/read; broker error/timeout paths; schema metadata contract.

**Phase D – Concurrency and multi-process:** Writer acquisition timeout and eventual success; reader TOCTTOU and wrap-around; concurrent readers; zombie writer reclaim; DataBlockMutex; cross-process basic exchange and high load.

### Phase D checklist (path to completion)

| # | Item | Priority | Status |
|---|------|----------|--------|
| D1 | Writer acquisition – timeout when readers hold; success when readers drain | P1 | ✅ Done (in-process) |
| D2 | Zombie writer reclaim | P2 | ✅ Done |
| D3 | DataBlockMutex cross-process | P2 | ✅ Done |
| D4 | Cross-process basic exchange (one write, one read) | P1 | ✅ Done |
| D5 | Reader TOCTTOU retry path | P1 | ✅ Done |
| D6 | Reader wrap-around (generation mismatch) | P1 | ⚠️ Partial |
| D7 | Concurrent readers same slot | P1 | ✅ Done |
| D8 | Cross-process high load (many write/read rounds) | P1 | ❌ Not done |
| D9 | Cross-process writer blocks on reader | P1 | ❌ Not done |
| D10 | Cross-process multiple rounds | P2 | ❌ Not done |

**Remaining:** D6 (generation mismatch assertion), D8–D10 (cross-process). See **`docs/DATAHUB_TODO.md`** for current priorities.

### Test infrastructure needs

- **DataBlockTestFixture (or equivalent):** Create producer/consumer with given config (and optional schema); cleanup shm/handles.
- **Broker for tests:** In-process mock or small broker for REG_REQ/DISC_REQ with shm_name, schema_hash, schema_version.
- **Schema tests:** validate_header_layout_hash and consumer schema mismatch (test_schema_validation enabled in CMake).

### Current test coverage (Layer 3 DataHub)

| Plan area | Covered | Notes |
|-----------|---------|--------|
| Part 0 (foundations) | Yes | Platform shm, SharedSpinLock, Backoff, Crypto, Lifecycle, Schema BLDS |
| Phase A – Protocol/API | Yes | test_phase_a_protocol, phase_a_workers, test_schema_validation |
| Phase B – Slot protocol | Yes | test_slot_protocol, slot_protocol_workers |
| Error handling | Yes | test_error_handling, error_handling_workers |
| Recovery/diagnostics | Smoke | test_recovery_api, recovery_workers |
| Phase C – MessageHub + broker | No | To be implemented |
| Phase D – Concurrency / multi-process | Partial | D1–D5, D7 done; D6 partial; D8–D10 not done |
| Recovery scenario | Deferred | Policy to be defined first |

### Scenario coverage matrix (summary)

Coverage exists for: **ConsumerSyncPolicy** (Latest_only, Single_reader, Sync_reader); **DataBlockPolicy** (Single, DoubleBuffer, RingBuffer); ring capacity 1–4; **Physical page size** (Size4K, Size4M); logical unit size 0 and custom; **Flexible zone** (no zones, single zone, zone with spinlock); **ChecksumPolicy** (None, Manual, Enforced). Optional gaps: ring capacity >4, Size16M.

### Summary

Prioritize: (1) flexible zone and checksum semantics (no access when undefined), (2) producer/consumer agreement and schema validation, (3) slot write/commit and read with checksums, (4) MessageHub with or without broker, (5) concurrency and multi-process. Implement in phases: API correctness → single-process slot protocol → MessageHub/broker → concurrency/multi-process. For MessageHub code review (header/impl alignment, C++20, abstraction, broker contract), see **`docs/IMPLEMENTATION_GUIDANCE.md`** § MessageHub code review.

---

## Quick Reference

### Common Commands

| Task | Command |
|------|---------|
| Build tests | `cmake --build build --target stage_tests` |
| Run all tests | `ctest` (from `build/` directory) |
| Run tests with output | `ctest --output-on-failure` |
| Run specific suite | `ctest -R "^MyTestSuite"` |
| Run single test | `ctest -R "^MyTestSuite.SpecificTest$"` |
| Run test executable directly | `./build/stage-debug/tests/test_layer3_datahub` (or `test_layer2_*`, etc.) |
| Filter tests in executable | `./test_layer3_datahub --gtest_filter=MyTest.*` |
| List available tests | `./test_layer3_datahub --gtest_list_tests` |
| Debug a test | `gdb ./test_layer3_datahub` |

### Test File Locations

| Component | Test File Location | Test Executable(s) |
|-----------|-------------------|---------------------|
| Platform (version, shm, is_process_alive, debug) | `tests/test_layer0_platform/` | `test_layer0_platform` |
| SpinGuard, InProcessSpinState, RecursionGuard, ScopeGuard, format tools | `tests/test_layer1_base/` | `test_layer1_spinlock`, `test_layer1_recursion_guard`, `test_layer1_scope_guard`, `test_layer1_format_tools` |
| Lifecycle, FileLock, Logger, JsonConfig, SharedSpinLock, CryptoUtils | `tests/test_layer2_service/` | `test_layer2_lifecycle`, `test_layer2_filelock`, `test_layer2_logger`, `test_layer2_shared_memory_spinlock`, etc. |
| DataBlock, MessageHub, schema, recovery, slot protocol, error handling | `tests/test_layer3_datahub/` | `test_layer3_datahub` (single executable; use `--gtest_filter=*` to select suites) |

### GoogleTest Assertions

| Assertion | Behavior | Use When |
|-----------|----------|----------|
| `ASSERT_*` | Stops test on failure | Checking prerequisites |
| `EXPECT_*` | Continues test on failure | Testing outcomes |
| `ASSERT_TRUE(cond)` | Fatal if false | Must be true to continue |
| `EXPECT_EQ(a, b)` | Non-fatal equality | Comparing values |
| `EXPECT_THROW(stmt, exception)` | Expects exception | Testing error handling |
| `EXPECT_NO_THROW(stmt)` | Expects no exception | Testing success path |

### Multi-Process Testing Pattern

```cpp
// Spawn worker process
WorkerProcess proc("worker.function_name", {"arg1", "arg2"});
proc.spawn();

// Wait and check result
int exit_code = proc.wait_for_exit(std::chrono::seconds(5));
EXPECT_EQ(exit_code, 0);

// Check worker output
std::string stdout_content = proc.read_stdout();
EXPECT_THAT(stdout_content, ::testing::HasSubstr("expected output"));
```

### Staging Directory Structure

```
build/stage-debug/
├── bin/                    # Executables and DLLs (Windows)
├── lib/                    # Shared libraries (POSIX)
├── include/                # Headers
├── tests/                  # Test executables + DLLs (Windows)
│   ├── test_pylabhub_corelib
│   ├── test_pylabhub_utils
│   └── *.dll (Windows only)
└── .stage_complete         # Marker file
```

**Executable names:** Use `test_layer0_platform`, `test_layer1_spinlock` (and other layer1 targets), `test_layer2_*` (e.g. `test_layer2_filelock`, `test_layer2_logger`), and `test_layer3_datahub` for DataHub tests. Replace any legacy `test_pylabhub_utils` references in docs with the appropriate layer executable.
