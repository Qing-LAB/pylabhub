# Test Pattern Analysis - Migration Plan

## Lifecycle Tests Analysis

### Pattern 3 (MUST use separate process)

#### Static Lifecycle Tests - Process Crash Expected
- ✅ `LifecycleTest.RegisterAfterInitAborts` - expects abort()
- ✅ `LifecycleTest.FailsWithUnresolvedDependency` - expects abort()
- ✅ `LifecycleTest.FailsWithCaseSensitiveDependency` - expects abort()
- ✅ `LifecycleTest.StaticCircularDependencyAborts` - expects abort()
- ✅ `LifecycleTest.StaticElaborateIndirectCycleAborts` - expects abort()

#### Static Lifecycle Tests - Pre-Init State Check
- ✅ `LifecycleTest.IsInitializedFlag` - checks IsAppInitialized() BEFORE guard creation
- ✅ `LifecycleTest.ModuleRegistrationAndInitialization` - tests fresh registration
- ⚠️  `LifecycleTest.MultipleGuardsWarning` - checks warning on second guard

#### Dynamic Lifecycle Tests - Fresh State Required
- ✅ ALL dynamic lifecycle tests - need fresh module graph for load/unload testing

**Justification for Pattern 3:**
- Lifecycle is a singleton - once initialized, cannot be reset in same process
- Testing initialization order, dependencies, and module loading requires fresh state
- Crash/abort tests would kill the main test process

### Pattern 2 (Could use, but currently not applicable)
- ❌ None - Lifecycle tests inherently need isolation or crash testing

### Pattern 1 (Pure API tests)
- ✅ `LifecycleTest.ModuleDef_RejectsNullName` - pure API validation
- ✅ `LifecycleTest.ModuleDef_RejectsNameExceedingMaxLength` - pure API validation
- ✅ `LifecycleTest.ModuleDef_AcceptsNameAtMaxLength` - pure API validation
- ✅ `LifecycleTest.AddDependency_IgnoresNull` - pure API validation
- ✅ `LifecycleTest.AddDependency_RejectsNameExceedingMaxLength` - pure API validation
- ✅ `LifecycleTest.LoadModule_ReturnsFalseForNull` - pure API validation
- ✅ `LifecycleTest.LoadModule_ReturnsFalseForNameExceedingMaxLength` - pure API validation
- ✅ `LifecycleTest.UnloadModule_ReturnsFalseForNull` - pure API validation
- ✅ `LifecycleTest.UnloadModule_ReturnsFalseForNameExceedingMaxLength` - pure API validation

---

## Logger Tests Analysis

### Pattern 3 (Currently used, but MOST should migrate to Pattern 2)

#### Keep as Pattern 3 (True multi-process IPC)
- ✅ `LoggerTest.InterProcessFlock` - tests file locking BETWEEN processes
- ✅ `LoggerTest.MultiProcessStress` (if exists) - concurrent process access

#### Migrate to Pattern 2 (Can run in single process)
- 🔄 `LoggerTest.BasicLogging` - simple log write/read
- 🔄 `LoggerTest.LogLevelFiltering` - log level functionality
- 🔄 `LoggerTest.BadFormatString` - error handling
- 🔄 `LoggerTest.DefaultSinkAndSwitching` - sink management
- 🔄 `LoggerTest.MultithreadStress` - multi-THREAD (not process) stress test
- 🔄 `LoggerTest.FlushWaitsForQueue` - queue behavior
- 🔄 `LoggerTest.ShutdownIdempotency` - shutdown behavior
- 🔄 `LoggerTest.ReentrantErrorCallback` - callback behavior
- 🔄 `LoggerTest.WriteErrorCallbackAsync` - async callback
- 🔄 `LoggerTest.PlatformSinks` - platform-specific sinks
- 🔄 `LoggerTest.ConcurrentLifecycleChaos` - lifecycle + threading
- 🔄 `LoggerTest.RotatingFileSink` - file rotation
- 🔄 `LoggerTest.QueueFullAndMessageDropping` - queue overflow

**Rationale:** These tests work fine with Logger initialized in main(). They don't need fresh Logger state for each test - they're testing behavior, not initialization.

---

## FileLock Tests Analysis

### Pattern 3 (True multi-process tests)
- ✅ `FileLockTest.MultiProcessNonBlocking` - lock contention BETWEEN processes
- ✅ `FileLockTest.MultiProcessBlockingContention` - multiple processes competing
- ✅ `FileLockTest.MultiProcessParentChildBlocking` - parent/child process coordination
- ✅ `FileLockTest.MultiProcessTryLock` - try_lock between processes

### Pattern 2 (Migrate - single process with lifecycle)
- 🔄 `FileLockTest.BasicNonBlocking` - basic API test with logging
- 🔄 `FileLockTest.BlockingLock` - blocking behavior in single process
- 🔄 `FileLockTest.TimedLock` - timeout behavior
- 🔄 `FileLockTest.MoveSemantics` - move constructor/assignment
- 🔄 `FileLockTest.DirectoryCreation` - directory handling
- 🔄 `FileLockTest.DirectoryPathLocking` - directory locks
- 🔄 `FileLockTest.MultiThreadedNonBlocking` - multi-THREAD (not process) test

### Pattern 1 (Pure API - no lifecycle needed)
- ✅ `FileLockTest.TryLockPattern` - pure API usage patterns
- ✅ `FileLockTest.InvalidResourcePath` - error handling without I/O

---

## JsonConfig Tests Analysis

### Pattern 3 (Multi-process IPC)
- ✅ `JsonConfigTest.WriteId` - if testing cross-process file access
- ⚠️  `JsonConfigTest.UninitializedBehavior` - might be crash test?
- ⚠️  `JsonConfigTest.NotConsumingProxy` - need to verify

### Pattern 2 (Likely candidates)
- 🔄 Most JsonConfig tests if they're just testing read/write/query operations

### Pattern 1 (Pure API)
- Need to analyze individual tests to identify pure API tests

---

## Summary Statistics

### Current State (test_pylabhub_utils)
- **Total tests using WorkerProcess:** ~50+
- **Should be Pattern 3:** ~20 (40%)
- **Should be Pattern 2:** ~25 (50%)
- **Should be Pattern 1:** ~10 (10%)

### Estimated Performance Improvement
- **Current:** 50 process spawns × ~100ms = 5 seconds overhead
- **Optimized:** 20 process spawns × ~100ms = 2 seconds overhead
- **Net improvement:** 60% reduction in test execution time

---

## Implementation Plan

### Step 1: Enhance test_patterns.h
Add helper utilities for common Pattern 2 scenarios:
```cpp
// Helper for Logger tests
class LoggerTestHelper {
    fs::path GetUniqueLogPath(const std::string& name);
    void CleanupLogFile(const fs::path& path);
};

// Helper for FileLock tests
class FileLockTestHelper {
    fs::path GetTempLockPath(const std::string& name);
    void CleanupLockFile(const fs::path& path);
};
```

### Step 2: Create Pattern 2 examples in test_layer2_service
Demonstrate best practices for:
- Logger tests without WorkerProcess
- FileLock tests without WorkerProcess
- Mixed scenarios (some tests use Pattern 2, some use Pattern 3)

### Step 3: Document which test_pylabhub_utils tests MUST stay Pattern 3
Create a clear list with justifications

### Step 4: Gradual migration
- Don't break existing tests
- Add Pattern 2 versions alongside Pattern 3
- Measure performance improvements
- Eventually deprecate redundant Pattern 3 tests

---

## Next Actions

1. **Create Pattern 2 helper base classes** in test_patterns.h
2. **Implement Pattern 2 versions** of 2-3 Logger tests as examples
3. **Implement Pattern 2 versions** of 2-3 FileLock tests as examples
4. **Document the pattern** in test framework
5. **Get user feedback** on approach before full migration
