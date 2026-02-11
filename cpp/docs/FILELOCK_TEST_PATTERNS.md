# FileLock Test Patterns - When to Use Multi-Process

## Pattern 3 (MUST use separate processes)

### True Multi-Process Lock Contention
These tests verify that OS-level file locks work correctly **between independent processes**:

#### ✅ `FileLockTest.MultiProcessNonBlocking`
**Why Pattern 3:** Tests that Process A holding a lock prevents Process B from acquiring it
```cpp
TEST_F(FileLockTest, MultiProcessNonBlocking) {
    auto resource_path = temp_dir() / "multiprocess_nonblocking.txt";

    // Main process acquires lock
    FileLock main_lock(resource_path, ResourceType::File);
    ASSERT_TRUE(main_lock.valid());

    // Spawn child process that tries to acquire same lock
    WorkerProcess proc(g_self_exe_path, "filelock.nonblocking_acquire", {resource_path.string()});
    ASSERT_TRUE(proc.valid());

    // Child should fail because main process holds lock
    EXPECT_NE(proc.wait_for_exit(), 0);
}
```

#### ✅ `FileLockTest.MultiProcessBlockingContention`
**Why Pattern 3:** Tests lock release coordination between multiple competing processes
```cpp
TEST_F(FileLockTest, MultiProcessBlockingContention) {
    auto resource_path = temp_dir() / "contention.txt";

    // Spawn 5 worker processes that all compete for same lock
    std::vector<std::unique_ptr<WorkerProcess>> procs;
    for (int i = 0; i < 5; ++i) {
        procs.push_back(std::make_unique<WorkerProcess>(
            g_self_exe_path, "filelock.contention_log_access",
            {resource_path.string(), log_path.string(), std::to_string(i)}
        ));
    }

    // All processes should eventually acquire lock and write to log
    for (auto& proc : procs) {
        EXPECT_EQ(proc->wait_for_exit(), 0);
    }
}
```

#### ✅ `FileLockTest.MultiProcessParentChildBlocking`
**Why Pattern 3:** Tests parent-child process coordination (parent holds lock, child waits)
```cpp
TEST_F(FileLockTest, MultiProcessParentChildBlocking) {
    auto resource_path = temp_dir() / "parent_child.txt";

    // Parent acquires lock
    FileLock parent_lock(resource_path, ResourceType::File);
    ASSERT_TRUE(parent_lock.valid());

    // Spawn child that will block waiting for lock
    WorkerProcess child_proc(g_self_exe_path, "filelock.parent_child_block",
                            {resource_path.string()});

    std::this_thread::sleep_for(100ms);  // Let child start blocking

    // Release parent lock - child should now acquire and exit successfully
    parent_lock.unlock();
    EXPECT_EQ(child_proc.wait_for_exit(), 0);
}
```

---

## Pattern 2 (Can run in single process)

### Multi-Thread Tests (NOT multi-process)
These test thread safety within a single process:

#### 🔄 `FileLockTest.MultiThreadedNonBlocking`
**Why Pattern 2:** Testing thread safety, not process isolation
```cpp
// CURRENT (wasteful - uses WorkerProcess):
TEST_F(FileLockTest, MultiThreadedNonBlocking) {
    WorkerProcess proc(g_self_exe_path, "filelock.test_multithreaded_non_blocking", {path});
    proc.wait_for_exit();
}

// BETTER (Pattern 2 - run in same process):
TEST_F(FileLockTest, MultiThreadedNonBlocking) {
    auto resource_path = temp_dir() / "multithread.txt";

    std::atomic<int> success_count{0};
    std::atomic<int> fail_count{0};

    // Spawn 10 threads that all try to acquire lock
    std::vector<std::thread> threads;
    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([&, i]() {
            auto lock_opt = FileLock::try_lock(resource_path, ResourceType::File,
                                               LockMode::NonBlocking);
            if (lock_opt.has_value()) {
                success_count++;
                std::this_thread::sleep_for(10ms);  // Hold briefly
            } else {
                fail_count++;
            }
        });
    }

    for (auto& t : threads) t.join();

    // Exactly one thread should succeed, others should fail
    EXPECT_EQ(success_count.load(), 1);
    EXPECT_EQ(fail_count.load(), 9);
}
```

### Basic API Tests
These test lock behavior in isolation:

#### 🔄 `FileLockTest.BasicNonBlocking`
**Why Pattern 2:** Just testing basic acquire/release, no contention
```cpp
// CURRENT (wasteful):
TEST_F(FileLockTest, BasicNonBlocking) {
    WorkerProcess proc(g_self_exe_path, "filelock.test_basic_non_blocking", {path});
    proc.wait_for_exit();
}

// BETTER (Pattern 2):
TEST_F(FileLockTest, BasicNonBlocking) {
    auto resource_path = temp_dir() / "basic.txt";

    {
        FileLock lock(resource_path, ResourceType::File, LockMode::NonBlocking);
        ASSERT_TRUE(lock.valid());
        EXPECT_TRUE(lock.is_locked());
    }  // Lock released

    // Can acquire again after release
    FileLock lock2(resource_path, ResourceType::File, LockMode::NonBlocking);
    ASSERT_TRUE(lock2.valid());
}
```

#### 🔄 `FileLockTest.BlockingLock`
**Why Pattern 2:** Testing timeout behavior, no other processes involved
```cpp
// BETTER (Pattern 2):
TEST_F(FileLockTest, BlockingLock) {
    auto resource_path = temp_dir() / "blocking.txt";

    // Hold lock in main thread
    FileLock main_lock(resource_path, ResourceType::File);
    ASSERT_TRUE(main_lock.valid());

    // Spawn thread that tries to acquire with timeout
    std::atomic<bool> acquired{false};
    std::thread t([&]() {
        auto start = std::chrono::steady_clock::now();
        FileLock lock(resource_path, ResourceType::File,
                     LockMode::Blocking, std::chrono::milliseconds(100));
        auto elapsed = std::chrono::steady_clock::now() - start;

        acquired = lock.valid();
        EXPECT_FALSE(acquired);  // Should timeout
        EXPECT_GE(elapsed, std::chrono::milliseconds(100));
    });

    t.join();
    EXPECT_FALSE(acquired.load());
}
```

---

## Pattern 1 (Pure API - no lifecycle)

#### ✅ `FileLockTest.TryLockPattern`
**Why Pattern 1:** Pure API test, no I/O, no logging
```cpp
TEST_F(FileLockTest, TryLockPattern) {
    auto resource_path = temp_dir() / "try_lock_pattern.txt";

    // Success case
    {
        auto lock_opt = FileLock::try_lock(resource_path, ResourceType::File,
                                           LockMode::NonBlocking);
        ASSERT_TRUE(lock_opt.has_value());
        EXPECT_TRUE(lock_opt->valid());
    }

    // Failure case - resource already locked
    {
        FileLock main_lock(resource_path, ResourceType::File);
        auto lock_opt = FileLock::try_lock(resource_path, ResourceType::File,
                                           LockMode::NonBlocking);
        EXPECT_FALSE(lock_opt.has_value());
    }
}
```

---

## Summary Table

| Test Name | Current | Should Be | Reason |
|-----------|---------|-----------|--------|
| MultiProcessNonBlocking | Pattern 3 | Pattern 3 ✅ | True IPC - tests lock between processes |
| MultiProcessBlockingContention | Pattern 3 | Pattern 3 ✅ | True IPC - multiple processes competing |
| MultiProcessParentChildBlocking | Pattern 3 | Pattern 3 ✅ | True IPC - parent/child coordination |
| MultiProcessTryLock | Pattern 3 | Pattern 3 ✅ | True IPC - try_lock between processes |
| MultiThreadedNonBlocking | Pattern 3 | Pattern 2 🔄 | Thread safety, not process isolation |
| BasicNonBlocking | Pattern 3 | Pattern 2 🔄 | Basic API, no contention |
| BlockingLock | Pattern 3 | Pattern 2 🔄 | Timeout behavior, no IPC |
| TimedLock | Pattern 3 | Pattern 2 🔄 | Timeout behavior |
| MoveSemantics | Pattern 3 | Pattern 2 🔄 | Testing move constructor/assignment |
| DirectoryCreation | Pattern 3 | Pattern 2 🔄 | Directory handling |
| DirectoryPathLocking | Pattern 3 | Pattern 2 🔄 | Directory locks |
| TryLockPattern | Pattern 3 | Pattern 1 🔄 | Pure API, no I/O |
| InvalidResourcePath | Pattern 3 | Pattern 1 🔄 | Error handling, no I/O |

---

## Key Decision Criteria

**Use Pattern 3 (Multi-Process) ONLY when:**
- ✅ Testing OS-level file lock behavior **between independent processes**
- ✅ Verifying that Process A holding lock prevents Process B from acquiring
- ✅ Testing lock coordination between parent/child processes
- ✅ Validating lock release is visible to other processes

**Use Pattern 2 (Single Process) when:**
- Testing thread safety (multiple threads in same process)
- Testing basic lock acquire/release behavior
- Testing timeout/blocking behavior
- Testing lock lifecycle (RAII, move semantics)

**Use Pattern 1 (Pure API) when:**
- No actual file I/O needed
- Testing error handling with invalid inputs
- Testing API contracts without side effects
