# CTest vs Direct Test Execution - Critical Differences

## The Hidden Behavior Difference

### CTest Execution (default)
```bash
ctest --test-dir build
```
**Behavior:** Each `TEST_F(...)` runs in a **separate process**
- Test 1 runs in Process A → exits
- Test 2 runs in Process B → exits
- Test 3 runs in Process C → exits

**Implication:** Tests that modify global state (like shutting down lifecycle) appear to work because each test gets a fresh process.

### Direct Execution
```bash
./build/tests/test_layer2_lifecycle
```
**Behavior:** All `TEST_F(...)` run in the **same process**
- Test 1 runs → Test 2 runs → Test 3 runs (all in Process A)

**Implication:** If Test 1 shuts down lifecycle, Test 2 and Test 3 will fail because lifecycle cannot be reinitialized.

---

## Why This Matters for Pattern Selection

### Hidden Pattern 3 Requirement: Non-Reversible State Changes

Some tests MUST use Pattern 3 (WorkerProcess) not because they crash, but because they **modify singleton/global state in a non-reversible way**.

#### Example: `test_concurrent_lifecycle_chaos`
```cpp
int test_concurrent_lifecycle_chaos(const std::string &log_path_str) {
    // Register and initialize lifecycle
    RegisterModule(Logger::GetLifecycleModule());
    LifecycleManager::instance().initialize(std::source_location::current());

    // ... spawn threads, do chaos testing ...

    // THIS IS THE PROBLEM:
    LifecycleManager::instance().finalize(std::source_location::current());

    // Lifecycle is now FINALIZED and cannot be reinitialized
    return 0;
}
```

**What happens:**
- **With CTest:** ✅ Works fine - test runs in separate process, finalize() doesn't affect other tests
- **Direct execution:** ❌ FAILS - subsequent tests try to use Logger but lifecycle is finalized

#### Example: Lifecycle shutdown tests
```cpp
TEST_F(LoggerTest, ShutdownIdempotency) {
    WorkerProcess proc(g_self_exe_path, "logger.test_shutdown_idempotency", {...});
    proc.wait_for_exit();
}

// Worker implementation:
int test_shutdown_idempotency(const std::string &log_path_str) {
    // ... test shutdown ...
    Logger::instance().shutdown();
    Logger::instance().shutdown();  // Should be idempotent

    // Logger is now SHUTDOWN
    return 0;
}
```

**Why Pattern 3 is required:**
- Test explicitly shuts down Logger
- Logger cannot be restarted in the same process
- If this ran as Pattern 2, all subsequent tests would fail

---

## Updated Pattern 3 Decision Criteria

Use Pattern 3 (Multi-Process) when ANY of these apply:

### 1. Process crash/abort testing
Test expects `abort()`, `assert()`, or segfault
- Example: `LifecycleTest.RegisterAfterInitAborts`

### 2. Pre-initialization state testing
Test needs to check state BEFORE lifecycle initialization
- Example: `LifecycleTest.IsInitializedFlag`

### 3. True multi-process IPC
Testing communication/coordination between independent processes
- Example: `FileLockTest.MultiProcessBlockingContention`

### 4. **Non-reversible state modification** ⭐ NEW
Test modifies singleton/global state that cannot be reset
- Example: `test_concurrent_lifecycle_chaos` - calls `finalize()`
- Example: `test_shutdown_idempotency` - calls `shutdown()`
- Example: Tests that unload dynamic modules
- Example: Tests that modify global configuration that can't be reset

### 5. Lifecycle isolation
Test needs completely fresh module graph
- Example: All dynamic lifecycle load/unload tests

---

## How to Detect Pattern 3 Requirements

### Red Flags - Test MUST use Pattern 3:
```cpp
// Finalizing lifecycle
LifecycleManager::instance().finalize(...)

// Shutting down modules
Logger::instance().shutdown()

// Unloading dynamic modules
UnloadModule("SomeModule")

// Aborting/crashing
abort()
assert(false)
std::terminate()

// Testing pre-init state
if (IsAppInitialized()) { ... }  // Before any LifecycleGuard
```

### Safe for Pattern 2:
```cpp
// Using already-initialized services
LOGGER_INFO("message")
FileLock lock(path, ...)
JsonConfig::Get("key")

// Thread creation
std::thread t([](){ ... })

// Flushing/temporary state changes
Logger::instance().flush()
Logger::instance().set_log_level(LogLevel::Debug)

// RAII that cleans up properly
{
    FileLock lock(...);
    // lock released at end of scope
}
```

---

## Real-World Examples

### ❌ WRONG - Would fail on direct execution
```cpp
class LoggerTest : public ::testing::Test {};

TEST_F(LoggerTest, BasicLogging) {
    LOGGER_INFO("Test 1");
    // Works fine
}

TEST_F(LoggerTest, ShutdownBehavior) {
    LOGGER_INFO("Test 2");
    Logger::instance().shutdown();  // PROBLEM!
    // Test ends, but Logger is now shutdown
}

TEST_F(LoggerTest, AnotherTest) {
    LOGGER_INFO("Test 3");  // FAILS - Logger is shutdown from previous test
}
```

**CTest hides the problem:** Each test runs in separate process
**Direct execution reveals it:** Test 3 fails because Logger was shutdown by Test 2

### ✅ CORRECT - Use Pattern 3 for shutdown test
```cpp
class LoggerTest : public ::testing::Test {};

TEST_F(LoggerTest, BasicLogging) {
    LOGGER_INFO("Test 1");  // Pattern 2 - fine
}

TEST_F(LoggerTest, ShutdownBehavior) {
    // Pattern 3 - runs in separate process
    WorkerProcess proc(g_self_exe_path, "logger.test_shutdown", {...});
    proc.wait_for_exit();
}

TEST_F(LoggerTest, AnotherTest) {
    LOGGER_INFO("Test 3");  // Pattern 2 - fine, Logger still initialized
}
```

---

## Testing Strategy

### During Development
**Always test with direct execution** to catch Pattern issues:
```bash
# Build the test
cmake --build build --target test_layer2_logger

# Run directly (not through CTest)
./build/stage-debug/tests/test_layer2_logger

# If any tests fail due to "Logger not initialized" or similar,
# those tests need to be converted to Pattern 3
```

### In CI/CD
Run both ways to ensure correctness:
```bash
# Standard CTest run
ctest --test-dir build --output-on-failure

# Direct execution run (catches Pattern issues)
for test_exe in build/stage-debug/tests/test_layer2_*; do
    echo "Running $test_exe directly..."
    $test_exe || echo "FAILED: $test_exe"
done
```

---

## Refactoring Checklist

When evaluating if a test should use Pattern 2 or Pattern 3:

1. ☐ Does the test call `finalize()`, `shutdown()`, or `abort()`?
   - **YES** → Pattern 3 required

2. ☐ Does the test modify singleton state that can't be reset?
   - **YES** → Pattern 3 required

3. ☐ Does the test check state before lifecycle initialization?
   - **YES** → Pattern 3 required

4. ☐ Does the test need TRUE multi-process IPC?
   - **YES** → Pattern 3 required

5. ☐ Run the test executable directly - do subsequent tests fail?
   - **YES** → Pattern 3 required

6. ☐ None of the above?
   - → Pattern 2 is safe to use

---

## Summary

| Aspect | CTest Execution | Direct Execution |
|--------|----------------|------------------|
| Process model | One process per test | All tests in same process |
| State isolation | Perfect - each test is fresh | Shared - tests can affect each other |
| Hides Pattern issues | ✅ Yes - makes Pattern 3 issues invisible | ❌ No - exposes incorrect patterns |
| Performance | Slower (process spawning overhead) | Faster (no spawning) |
| Use for | CI/CD, normal testing | Development, pattern validation |

**Key Takeaway:** CTest's per-test process spawning masks Pattern 3 requirements. Always verify tests work with direct execution to ensure correct pattern usage.
