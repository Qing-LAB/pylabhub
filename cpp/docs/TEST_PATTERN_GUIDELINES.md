# Test Pattern Guidelines

This document provides clear guidance on choosing the appropriate test pattern for PyLabHub C++ tests.

## The Three Test Patterns

### Pattern 1: Pure API Test (`PureApiTest`)
**When to use:**
- Testing pure functions with no side effects
- Testing API contracts (error handling, return values, edge cases)
- No lifecycle or module dependencies required
- No file I/O, logging, or IPC needed

**Characteristics:**
- Fast execution (no process spawning, no lifecycle overhead)
- High isolation between tests
- No shared state
- Each test is completely independent

**Example scenarios:**
- Testing `FileLock::try_lock()` API with invalid paths
- Testing guard classes (AtomicGuard, RecursionGuard, ScopeGuard)
- Testing utility functions (format strings, conversions)
- Testing data structure APIs without I/O

**Code example:**
```cpp
class MyApiTest : public pylabhub::tests::PureApiTest {
protected:
    void SetUp() override {
        PureApiTest::SetUp();
        // Pure setup - no lifecycle
    }
};

TEST_F(MyApiTest, InvalidInputReturnsError) {
    auto result = my_pure_function(nullptr);
    EXPECT_FALSE(result.has_value());
}
```

---

### Pattern 2: Lifecycle-Managed Test (`LifecycleManagedTest`)
**When to use:**
- Testing functionality that requires Logger, FileLock, JsonConfig
- Testing thread safety and concurrency within a single process
- Testing normal operation paths (no crashes/aborts expected)
- Can tolerate shared lifecycle state across tests

**Characteristics:**
- Lifecycle initialized once in `main()` for all tests
- Moderate execution speed (no process spawning)
- Tests share lifecycle modules (Logger, FileLock, etc.)
- Suitable for most service-layer tests

**Example scenarios:**
- Testing Logger with different log levels and sinks
- Testing FileLock contention with multiple threads in same process
- Testing JsonConfig read/write operations
- Testing normal DataBlock operations

**Code example:**
```cpp
class MyServiceTest : public pylabhub::tests::LifecycleManagedTest {
protected:
    void SetUp() override {
        // Register modules needed for this test
        RegisterModule(Logger::GetLifecycleModule());
        RegisterModule(FileLock::GetLifecycleModule());

        // Initialize lifecycle (call at END of SetUp)
        LifecycleManagedTest::SetUp();

        // Test-specific setup
    }
};

TEST_F(MyServiceTest, LoggerWritesToFile) {
    Logger::SetSink(/* ... */);
    LOGGER_INFO("Test message");
    // Verify log file contents
}
```

**NOTE:** Currently, `test_entrypoint.cpp` main() already initializes Logger, FileLock, and JsonConfig. Tests can simply inherit from `::testing::Test` and use these modules directly without explicit RegisterModule calls.

---

### Pattern 3: Multi-Process Test (`MultiProcessTest`)
**When to use (ONLY these scenarios):**
- **Process crash/abort testing**: Test expects abort(), assert(), or crash
- **Pre-initialization testing**: Test needs to check state BEFORE lifecycle initialization
- **True multi-process IPC**: Testing cross-process communication, shared memory, or file locks between processes
- **Lifecycle isolation**: Test needs completely fresh lifecycle state (e.g., testing module registration order)
- **Chaos/robustness testing**: Testing shutdown under extreme conditions that might crash/deadlock

**Characteristics:**
- Slowest execution (spawns separate processes)
- Full isolation - each worker gets fresh process state
- Required for crash testing and IPC validation
- Higher resource usage

**Example scenarios:**
- **Lifecycle crash tests**: `RegisterAfterInitAborts`, `CircularDependencyAborts`
- **Lifecycle pre-init tests**: `IsInitializedFlag` (checks before LifecycleGuard exists)
- **Multi-process FileLock**: Testing lock contention between separate processes
- **Multi-process DataBlock**: Testing producer/consumer across processes
- **Lifecycle state isolation**: Testing module load/unload sequences

**Code example:**
```cpp
// In test file:
class MyMultiProcessTest : public ::testing::Test {};

TEST_F(MyMultiProcessTest, ProcessCrashDetection) {
    WorkerProcess proc(g_self_exe_path, "lifecycle.test_register_after_init_aborts", {});
    ASSERT_TRUE(proc.valid());
    ASSERT_NE(proc.wait_for_exit(), 0);  // Expect failure
    ASSERT_THAT(proc.get_stderr(), HasSubstr("FATAL"));
}

// In lifecycle_workers.cpp:
int test_register_after_init_aborts() {
    LifecycleGuard guard;
    ModuleDef module_a("LateModule");
    RegisterModule(std::move(module_a)); // Should abort
    return 1; // Should not be reached
}
```

---

## Decision Tree

```
Does your test expect process crash/abort?
├─ YES → Use Pattern 3 (MultiProcessTest)
└─ NO
   └─ Does your test need to check state BEFORE lifecycle init?
      ├─ YES → Use Pattern 3 (MultiProcessTest)
      └─ NO
         └─ Does your test need TRUE multi-process IPC (cross-process communication)?
            ├─ YES → Use Pattern 3 (MultiProcessTest)
            └─ NO
               └─ Does your test need lifecycle modules (Logger, FileLock, JsonConfig)?
                  ├─ YES → Use Pattern 2 (LifecycleManagedTest or plain ::testing::Test)
                  └─ NO → Use Pattern 1 (PureApiTest)
```

---

## Common Anti-Patterns to Avoid

### ❌ Anti-Pattern 1: Overusing Pattern 3
**Problem:** Using WorkerProcess for tests that don't need process isolation

**Example:**
```cpp
// BAD: Logger test that doesn't need separate process
TEST_F(LoggerTest, BasicLogging) {
    WorkerProcess proc(g_self_exe_path, "logger.test_basic_logging", {log_path});
    proc.wait_for_exit();
    expect_worker_ok(proc);
}
```

**Fix:** Use Pattern 2 instead
```cpp
// GOOD: Run in same process with lifecycle from main()
TEST_F(LoggerTest, BasicLogging) {
    auto log_path = GetUniqueLogPath("basic_logging");
    Logger::SetSink(CreateFileSink(log_path));
    LOGGER_INFO("Test message");

    auto content = ReadFile(log_path);
    EXPECT_THAT(content, HasSubstr("Test message"));
}
```

### ❌ Anti-Pattern 2: Using Pattern 2 for crash tests
**Problem:** Testing abort/crash in the main test process

**Example:**
```cpp
// BAD: This will crash the entire test suite
TEST_F(LifecycleTest, CircularDependencyAborts) {
    ModuleDef a("A"), b("B");
    a.add_dependency("B");
    b.add_dependency("A");
    LifecycleGuard guard(MakeModDefList(std::move(a), std::move(b)));
    // CRASH - entire test suite dies
}
```

**Fix:** Use Pattern 3
```cpp
// GOOD: Crash happens in separate process
TEST_F(LifecycleTest, CircularDependencyAborts) {
    WorkerProcess proc(g_self_exe_path, "lifecycle.test_circular_dependency", {});
    ASSERT_NE(proc.wait_for_exit(), 0);  // Expect failure
}
```

---

## Migration Strategy

### Phase 1: Identify candidates
For each test in `test_pylabhub_utils/`, classify:
- **Keep as Pattern 3**: Crash tests, pre-init tests, true IPC tests
- **Migrate to Pattern 2**: Most Logger, FileLock, JsonConfig tests
- **Migrate to Pattern 1**: Pure API tests (no I/O, no lifecycle)

### Phase 2: Implement in-process versions
Create in-process test implementations for Pattern 2 candidates

### Phase 3: Update test framework
- Add better documentation to `test_patterns.h`
- Add compile-time checks to prevent misuse
- Add runtime warnings for anti-patterns

### Phase 4: Deprecate redundant workers
Remove worker implementations for tests migrated to Pattern 1 or 2

---

## Current Status

### test_pylabhub_utils (OLD approach)
- **Pattern 3 overuse**: ~80% of tests use WorkerProcess unnecessarily
- **Performance impact**: Slow test execution due to process spawning
- **Complexity**: Worker implementations spread across multiple files

### test_layer2_service (NEW approach - in progress)
- ✅ Support for Pattern 3 via worker dispatcher
- ⚠️  Need to add Pattern 1 and Pattern 2 examples
- ⚠️  Need to migrate applicable tests from Pattern 3 to Pattern 2

### Recommended Actions
1. Create Pattern 2 examples in test_layer2_service for Logger, FileLock, JsonConfig
2. Document which tests MUST use Pattern 3 and why
3. Provide helper utilities for common Pattern 2 scenarios
4. Add static analysis to detect Pattern 3 overuse
