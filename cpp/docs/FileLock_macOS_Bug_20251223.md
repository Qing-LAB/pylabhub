# Investigation into `FileLock` Race Condition on macOS - 2025-12-23

## 1. Executive Summary

The `JsonConfig` refactoring, designed to prevent "lost update" race conditions, has uncovered a critical, latent bug in the POSIX implementation of the underlying `FileLock` utility. While the `JsonConfig` logic is sound, its multi-process contention test consistently fails. The failure manifests as a lost update, where a worker process reports a successful save, but its data is missing from the final file.

Detailed log analysis has provided definitive proof that the `FileLock` implementation does not guarantee inter-process exclusivity under certain high-contention scenarios on macOS. We have observed two processes acquiring the same "exclusive" `flock` at the same time. The root cause is an incorrect implementation of the polling strategy in `src/utils/FileLock.cpp`.

## 2. Failure Mode and Symptoms

1.  **Primary Symptom:** The `JsonConfigTest.MultiProcessContention` test fails with an error like "Worker posix-N failed to write", even though all worker processes exit with a success code (0). This indicates their `save()` call succeeded, but the write was subsequently lost.

2.  **Secondary Symptom:** After attempting a fix on `FileLock.cpp`, the more fundamental `FileLockTest.MultiProcessBlockingContention` test also began to fail, showing massive data loss (e.g., a final counter of 12 instead of the expected 1600). This confirmed the bug is in `FileLock` itself.

## 3. Proof of Failure: Simultaneous Lock Ownership

Your insight to correlate the timing of locks with `atomic_write_json` was key. The logs from the test run provide a "smoking gun"—a clear timeline where two processes hold the same exclusive lock simultaneously.

**Timeline from Logs:**
1.  `[...41.521976] (posix-4)`: **Acquires Exclusive Lock**.
2.  `[...41.524186] (posix-3)`: **Acquires the same Exclusive Lock**, while `posix-4` is still holding it.
3.  `[...41.524758] (posix-4)`: **Releases its Lock**.

This overlap proves the lock is not exclusive. As you predicted, this leads to both processes reading stale data and one overwriting the other's changes.

## 4. Root Cause Analysis

The `flock()` system call itself is reliable. The bug is in how our C++ wrapper calls it.

*   **The Bug:** The current POSIX implementation in `FileLock.cpp` uses a "re-open-on-busy" polling strategy. When it tries to acquire a lock and finds it busy, it immediately **closes** its file descriptor (`fd`) and then loops to try again. In the next loop, it gets a **brand new `fd`** by calling `open()` again.
*   **Why It's Wrong:** An OS file lock is associated with a specific file descriptor session. By repeatedly closing and re-opening the `fd`, the code creates a race condition that confuses the kernel's locking mechanism, leading to the observed failure of exclusivity.

## 5. The Corrective Action Plan (For Next Session)

The fix is to rewrite the POSIX locking logic in `src/utils/FileLock.cpp` to use a **persistent file descriptor**.

1.  **Open Once:** The code must `open()` the lock file only **once** to get a single, persistent `fd`.
2.  **Hold `fd` and Poll:** It must hold this `fd` open for the entire duration of the lock attempt.
3.  **Correct `flock` Usage:**
    *   For a pure blocking lock, it should call `flock(fd, LOCK_EX)` once and let the OS handle the blocking.
    *   For a timed or non-blocking lock, it should call `flock(fd, LOCK_EX | LOCK_NB)` in a loop, checking for `EWOULDBLOCK` and sleeping between retries, all while using the same `fd`.
4.  **Close `fd` on Finish:** The file descriptor should only be closed when the lock is finally released (in the `FileLock` destructor) or if a non-recoverable error occurs.

**Next Step:** The immediate next action is to apply this corrected, robust `flock` implementation to `src/utils/FileLock.cpp` and re-run the full test suite.
