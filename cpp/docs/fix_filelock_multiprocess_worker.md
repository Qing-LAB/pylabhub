# Summary and Fix for `FileLock` Multi-Process Contention

This document summarizes the investigation into the `FileLockTest.MultiProcessBlockingContention` test failure and outlines the necessary steps to fix it.

## 1. The Problem

The `FileLockTest.MultiProcessBlockingContention` test was failing with a severe "lost update" error. In this test, multiple worker processes are spawned to repeatedly acquire a lock and increment a counter in a shared file. The final value in the counter file was consistently much lower than expected (e.g., 19 instead of 1600), indicating a significant race condition where most updates were being lost.

## 2. Diagnostics & Root Cause Analysis

The investigation revealed two distinct but related problems: a flaw in the test worker's logic and a deeper, underlying bug in the `FileLock` implementation on POSIX systems.

### Part A: Flawed Worker Logic

The primary logical flaw was in the `worker::filelock::contention_increment` function located in `tests/helpers/workers.cpp`. This function was not correctly honoring the advisory lock protocol.

-   **What it did:** It would correctly call `FileLock lock(resource_path, ...)` which, by design, created and locked a separate sidecar file (e.g., `counter.txt.lock`).
-   **The flaw:** Immediately after acquiring the lock, the worker would use `ifstream` and `ofstream` to read and write directly to the original `resource_path` (`counter.txt`).
-   **The result:** The lock on `counter.txt.lock` did nothing to protect `counter.txt` from simultaneous access, leading to a classic read-modify-write race condition and data loss.

### Part B: Deeper `FileLock` Implementation Bug

A more fundamental bug was discovered in the POSIX implementation of `FileLock` itself. The `FileLockTest.MultiProcessNonBlocking` test, which only attempts to acquire locks without performing any file I/O, was also failing. Multiple processes were able to simultaneously acquire the same "exclusive" lock, proving that `FileLock` was not providing true inter-process exclusivity.

This bug was traced to the `run_os_lock_loop` function in `src/utils/FileLock.cpp`, where the file descriptor for the lock file was being reused incorrectly across polling attempts in a way that defeated the exclusivity guarantees of `flock(2)` when used by multiple independent processes.

## 3. The Fix (A Two-Part Solution)

To resolve the test failures, both issues must be addressed.

### Part 1: Correcting the Worker Protocol (Completed)

First, we made the `FileLock` API more explicit and corrected the worker's logic.

1.  **New Method:** A new public method was added to the `FileLock` class:
    ```cpp
    // In include/utils/FileLock.hpp
    std::optional<std::filesystem::path> get_locked_resource_path() const noexcept;
    ```
    This function returns the absolute, normalized path of the resource that the lock is intended to protect, but only if the lock is valid.

2.  **Updated Worker:** The `contention_increment` worker was updated to use this new method, ensuring it performs I/O on the correct resource only after a lock is acquired.

    **Before:**
    ```cpp
    // BROKEN: Locks one file, writes to another.
    FileLock lock(counter_path, ...);
    if (lock.valid()) {
        std::ifstream ifs(counter_path);
        //...
        std::ofstream ofs(counter_path);
        //...
    }
    ```

    **After:**
    ```cpp
    // CORRECTED: Honors the advisory lock protocol.
    FileLock lock(resource_path, ...);
    if (lock.valid()) {
        auto locked_path_opt = lock.get_locked_resource_path();
        if (locked_path_opt) {
            std::ifstream ifs(*locked_path_opt);
            //...
            std::ofstream ofs(*locked_path_opt);
            //...
        }
    }
    ```

### Part 2: Fixing the `FileLock` Exclusivity Bug (Next Step)

Even with the corrected worker, the tests will fail because the underlying locking primitive is not working correctly. The next and final step is to fix the implementation bug in `FileLock.cpp`.

-   **Action:** Modify the POSIX implementation of the `run_os_lock_loop` function in `src/utils/FileLock.cpp`.
-   **Instruction:** The logic must be changed so that on every single attempt within the polling loop, a **new file descriptor is acquired by calling `::open()`**. If the lock attempt on that descriptor fails, **it must be closed immediately with `::close()`** before the next attempt. This "open-lock-close_on_fail" cycle for each attempt guarantees that each process is dealing with a fresh, independent file description, which is required for `flock(2)` to provide true inter-process exclusivity.
