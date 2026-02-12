# Test Patterns Analysis

## Current Problem

The existing `test_pylabhub_utils` tests **overuse multi-process workers** for tests that don't actually need separate processes. This causes:

- **Performance overhead**: Spawning processes for every test is expensive
- **Code complexity**: Worker functions + dispatcher + main test = 3 files to maintain
- **Resource inefficiency**: Each process duplicates memory and setup

## The Three Test Patterns

### Pattern 1: Pure API Tests
**Use when**: Testing pure functions/APIs that don't require lifecycle initialization

**Characteristics**:
- No lifecycle dependencies (Logger, FileLock, JsonConfig, etc.)
- Fast, isolated unit tests
- Can run in any environment

**Base class**: `::testing::Test` or `PureApiTest`

**Examples**:
- `FileLockTest.TryLockPattern` - Tests API without needing Logger
- `FileLockTest.InvalidResourcePath` - Tests error handling
- Math utilities, string parsing, data structure operations

**Current implementation**:
```cpp
class FileLockTest : public ::testing::Test { };

TEST_F(FileLockTest, TryLockPattern) {
    auto lock = FileLock::try_lock(...);  // No lifecycle needed
    ASSERT_TRUE(lock.has_value());
}
```

---

### Pattern 2: Lifecycle-Managed Single-Process Tests
**Use when**: Testing functionality that requires lifecycle modules, but **doesn't need fresh lifecycle state per test**

**Characteristics**:
- Requires Logger, FileLock, JsonConfig, or other lifecycle-managed modules
- Can share lifecycle state across multiple tests
- Lifecycle initialized once in `main()`, all tests share it
- Can test thread safety, correctness, integration

**Base class**: `LifecycleManagedTest` (or just rely on main())

**Examples - Currently using WorkerProcess but SHOULDN'T**:
- `LoggerTest.BasicLogging` - Just needs Logger initialized, doesn't care about fresh state
- `LoggerTest.LogLevelFiltering` - Tests log level API with Logger running
- `LoggerTest.MultithreadStress` - Tests thread safety with Logger running
- `FileLockTest.BasicNonBlocking` - Just needs FileLock to work, doesn't care about fresh state
- `FileLockTest.BlockingLock` - Tests blocking behavior within same process

**Why these DON'T need separate processes**:
- Logger can handle multiple tests writing different messages
- FileLock tests that don't involve multi-process contention can run in same process
- No need for "fresh" lifecycle state - tests are independent

**Current (wasteful) implementation**:
```cpp
TEST_F(LoggerTest, BasicLogging) {
    // Spawns entire new process just to test basic logging!
    WorkerProcess proc(g_self_exe_path, "logger.test_basic_logging", {...});
    proc.wait_for_exit();
    expect_worker_ok(proc);
}
```

**Should be (efficient)**:
```cpp
class LoggerTest : public LifecycleManagedTest {
protected:
    void SetUp() override {
        // Lifecycle already initialized in main()
        // Just do test-specific setup
    }
};

TEST_F(LoggerTest, BasicLogging) {
    // Logger already initialized, just use it directly
    LOGGER_INFO("Test message");
    // Verify message was logged
}
```

---

### Pattern 3: Multi-Process Tests
**Use when**: TRULY need separate processes

**Required for**:
1. **Testing lifecycle initialization behavior**
   - Tests that check state BEFORE lifecycle init (`IsAppInitialized()`)
   - Tests that verify multiple LifecycleGuards behavior

2. **Testing abort/crash scenarios**
   - Circular dependency detection (causes abort)
   - Invalid module registration (causes abort)
   - Any test expecting process termination

3. **Testing multi-process IPC**
   - FileLock contention between processes
   - DataBlock shared memory access
   - Cross-process synchronization

**Base class**: `MultiProcessTest`

**Examples - Correctly using WorkerProcess**:
- `LifecycleTest.IsInitializedFlag` - MUST check before any lifecycle exists
- `LifecycleTest.RegisterAfterInitAborts` - Expects abort()
- `LifecycleTest.CircularDependencyAborts` - Expects abort()
- `FileLockTest.MultiProcessBlockingContention` - Tests cross-process locking
- `DataBlockTest.MultiProcessAccess` - Tests shared memory IPC

**Current (correct) implementation**:
```cpp
TEST_F(LifecycleTest, IsInitializedFlag) {
    // MUST spawn separate process because:
    // 1. Needs to check IsAppInitialized() BEFORE creating LifecycleGuard
    // 2. Main process already initialized lifecycle
    WorkerProcess proc(g_self_exe_path, "lifecycle.test_is_initialized_flag", {});
    ASSERT_EQ(proc.wait_for_exit(), 0);
}
```

---

## Decision Tree

```
Does the test require ANY lifecycle modules?
├─ NO  → Pattern 1: Pure API Test
│         Base: ::testing::Test
│         No WorkerProcess needed
│
└─ YES → Does it need to test lifecycle initialization itself?
          ├─ YES → Does it expect abort/crash?
          │        ├─ YES → Pattern 3: Multi-Process
          │        │        (RegisterAfterInitAborts, CircularDependency)
          │        │
          │        └─ NO  → Does it check pre-init state?
          │                 ├─ YES → Pattern 3: Multi-Process
          │                 │        (IsInitializedFlag)
          │                 │
          │                 └─ NO  → Pattern 2: Single-Process
          │
          └─ NO  → Does it test multi-process IPC?
                   ├─ YES → Pattern 3: Multi-Process
                   │        (FileLock contention, DataBlock IPC)
                   │
                   └─ NO  → Pattern 2: Single-Process
                            Base: LifecycleManagedTest
                            Lifecycle initialized in main()
```

---

## Concrete Examples from Current Tests

### Pattern 1 Examples (Currently Correct)
```cpp
// tests/test_layer1_base/test_scopeguard.cpp
class ScopeGuardTest : public ::testing::Test { };

TEST_F(ScopeGuardTest, BasicCleanup) {
    bool cleaned = false;
    {
        auto guard = make_scope_guard([&]() { cleaned = true; });
    }
    EXPECT_TRUE(cleaned);
}
```

### Pattern 2 Examples (Currently WRONG - using WorkerProcess unnecessarily)

**Current**:
```cpp
// tests/test_pylabhub_utils/test_logger.cpp
TEST_F(LoggerTest, BasicLogging) {
    auto log_path = GetUniqueLogPath("basic_logging");
    WorkerProcess proc(g_self_exe_path, "logger.test_basic_logging", {log_path.string()});
    ASSERT_TRUE(proc.valid());
    proc.wait_for_exit();
    expect_worker_ok(proc);
}
```

**Should be**:
```cpp
// tests/test_layer2_service/test_logger.cpp
class LoggerBasicTest : public LifecycleManagedTest {
protected:
    fs::path log_path;

    void SetUp() override {
        // Lifecycle already initialized by main()
        log_path = fs::temp_directory_path() / "test_basic_logging.log";
        if (fs::exists(log_path)) fs::remove(log_path);
    }

    void TearDown() override {
        if (fs::exists(log_path)) fs::remove(log_path);
    }
};

TEST_F(LoggerBasicTest, BasicLogging) {
    // Logger already running, just use it
    LOGGER_INFO("Test message");
    LOGGER_WARN("Warning message");

    // Verify log file contents
    ASSERT_TRUE(fs::exists(log_path));
    // Read and verify...
}
```

### Pattern 3 Examples (Currently Correct)

```cpp
// tests/test_layer2_service/test_lifecycle.cpp
TEST_F(LifecycleTest, IsInitializedFlag) {
    // MUST use separate process:
    // - Worker checks IsAppInitialized() BEFORE creating LifecycleGuard
    // - Main process already has lifecycle initialized
    WorkerProcess proc(g_self_exe_path, "lifecycle.test_is_initialized_flag", {});
    ASSERT_EQ(proc.wait_for_exit(), 0);
}

TEST_F(LifecycleTest, RegisterAfterInitAborts) {
    // MUST use separate process:
    // - Test expects process to abort()
    // - Can't abort main test process
    WorkerProcess proc(g_self_exe_path, "lifecycle.test_register_after_init_aborts", {});
    ASSERT_NE(proc.wait_for_exit(), 0);  // Expect non-zero exit
    ASSERT_THAT(proc.get_stderr(), HasSubstr("FATAL"));
}

// tests/test_pylabhub_utils/test_filelock.cpp
TEST_F(FileLockTest, MultiProcessBlockingContention) {
    // MUST use separate processes:
    // - Testing cross-process file locking
    // - Needs true multi-process IPC
    auto resource_path = GetUniqueResourcePath("mp_blocking_contention");

    std::vector<std::unique_ptr<WorkerProcess>> procs;
    for (int i = 0; i < 3; ++i) {
        procs.push_back(std::make_unique<WorkerProcess>(
            g_self_exe_path, "filelock.contention_log_access", {...}));
    }

    for (auto& proc : procs) {
        proc->wait_for_exit();
        expect_worker_ok(*proc);
    }
}
```

---

## Migration Strategy

### Phase 1: Fix the Framework (Already Done)
✓ `test_patterns.h` provides three base classes
✓ `test_entrypoint.cpp` initializes lifecycle in main()
✓ Worker infrastructure available for Pattern 3

### Phase 2: Migrate Tests (TODO)

1. **Identify Pattern 1 tests** (no lifecycle)
   - Already correct: test_layer0_platform, test_layer1_base
   - Keep as-is

2. **Identify Pattern 2 tests** (lifecycle but no special needs)
   - Most of test_logger.cpp (except crash tests)
   - Most of test_filelock.cpp (except multi-process tests)
   - Most of test_jsonconfig.cpp
   - **Action**: Rewrite to use direct testing without WorkerProcess

3. **Identify Pattern 3 tests** (truly need separate process)
   - All of test_lifecycle.cpp (tests lifecycle initialization itself)
   - All of test_lifecycle_dynamic.cpp (dynamic module loading)
   - Multi-process tests in test_filelock.cpp
   - Multi-process tests in test_datablock.cpp
   - **Action**: Keep WorkerProcess, ensure worker dispatcher is properly configured

### Phase 3: Performance Validation
- Measure test execution time before/after migration
- Expected improvement: 3-5x faster for Pattern 2 tests (no process spawning)

---

## Recommendations for Improvement

### 1. Make Pattern Choice Obvious in Test Names
```cpp
// Pattern 1: Pure API
TEST(FileLockApi, TryLockReturnsOptional) { ... }

// Pattern 2: Lifecycle-managed
TEST_F(FileLockIntegration, AcquireAndRelease) { ... }

// Pattern 3: Multi-process
TEST_F(FileLockMultiProcess, CrossProcessContention) { ... }
```

### 2. Add Comments to Pattern 3 Tests
```cpp
TEST_F(LifecycleTest, IsInitializedFlag) {
    // REQUIRES SEPARATE PROCESS:
    // - Must check IsAppInitialized() before any LifecycleGuard exists
    // - Main process already initialized lifecycle
    WorkerProcess proc(...);
}
```

### 3. Update test_patterns.h Documentation
Add usage guidelines and decision tree to the header file.

### 4. Create Migration Guidelines
Document how to convert Pattern 3 tests to Pattern 2 when appropriate.

---

## Summary

**Current state**: test_pylabhub_utils uses Pattern 3 (multi-process) for ~90% of tests

**Correct usage**:
- Pattern 1: ~20% (pure API tests)
- Pattern 2: ~60% (lifecycle-managed, single process)
- Pattern 3: ~20% (truly need separate process)

**Benefit of migration**:
- 3-5x faster test execution
- Simpler test code (no worker functions)
- Easier debugging (tests run in main process)
