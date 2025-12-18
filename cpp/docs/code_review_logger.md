# Logger Deadlock Analysis and Resolution (`test_write_error_callback`)

This document provides a detailed analysis of the deadlock issue discovered in the `test_write_error_callback` unit test for the `pylabub::utils::Logger`.

## 1. Symptom

The test suite hangs indefinitely when executing the `test_write_error_callback` case. This test is designed to verify the logger's behavior when a file write operation fails. The test forces this failure by making the log file read-only. The hang indicates a deadlock within the logger's asynchronous worker thread.

## 2. Root Cause Analysis

The investigation revealed a classic deadlock scenario caused by a **recursive lock attempt on a non-recursive `std::mutex`** within the logger's background worker thread.

Here is the sequence of events leading to the deadlock:

1.  **Lock Acquisition:** The logger's worker thread pulls a message from the queue and calls `do_write()` to perform the I/O. `do_write()` acquires a `std::unique_lock` on the `Impl::mtx` mutex. This mutex protects the logger's internal state, including file handles and configuration, from concurrent access.

2.  **Write Error:** The worker thread then calls `write_to_file_internal()`. Since the test has made the log file read-only, the underlying `::write()` or `WriteFile()` system call fails.

3.  **Flawed Error Handling Path:** The code correctly detects the write failure, but this is where the error lies. In the faulty implementation, the code calls the `record_write_error()` function **while still holding the lock on `Impl::mtx`**.

4.  **Recursive Lock Attempt (The Deadlock):** The `record_write_error()` function also needs to modify the logger's internal state (e.g., update the error counter, store the error message). To do this safely, its first action is to acquire a lock on the very same `Impl::mtx` mutex.

5.  **Conclusion:** The worker thread, which already owns the lock on `mtx`, attempts to lock it again. Because `std::mutex` is non-recursive, the thread deadlocks, waiting forever for a lock it already holds to be released.

Because the worker thread is now completely hung, it can never process any more messages nor can it signal the main thread that the logging queue has been flushed. As a result, the call to `L.flush()` in the main test thread never returns, causing the entire test application to hang.

## 3. The Fix

The solution is to ensure the mutex is released *before* entering the secondary error-handling function that also needs to acquire it.

The correct implementation involves modifying all internal I/O functions (like `write_to_file_internal` and `write_to_console_internal`) to explicitly unlock the mutex before invoking `record_write_error`.

**Example (Corrected Logic):**

```cpp
// Inside a function like write_to_file_internal(..., std::unique_lock<std::mutex> &lk)

// ... write operation fails ...
if (write_failed)
{
    // 1. Release the lock that was acquired in the parent function.
    lk.unlock();

    // 2. Now it is safe to call the error recording function,
    //    which will acquire its own lock on the same mutex.
    pImpl->record_write_error(err, "write() failed");

    return; // Exit the I/O function.
}
```

This change breaks the recursive dependency and resolves the deadlock. The worker thread can release the lock, call the error handler (which briefly re-acquires and releases the lock), and then continue its execution, eventually completing the flush operation and allowing the main test thread to proceed.

## 4. Implementation Plan

My attempts to apply this fix revealed that the source files in the provided context already contain the necessary `lk.unlock()` calls. This indicates that the compiled test executable is likely stale and was not built from the latest (fixed) version of the code.

Therefore, the plan is as follows:

1.  **Clean Build:** Perform a clean build of the project to ensure there are no stale object files or executables. This involves removing the `build/` directory.
2.  **Rebuild:** Configure and build the project again using CMake.
3.  **Verify:** Run the `test_logger` executable. With the correctly built binary, the `test_write_error_callback` test is expected to pass without deadlocking.
