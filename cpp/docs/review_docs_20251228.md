After reviewing the `README_*.md` files in the `docs/` directory and comparing them with the findings from the recent code review, I have identified several significant inconsistencies between the documented design and the actual behavior of the code.

The documentation describes a suite of robust, thread-safe, and process-safe utilities. However, the implementation contains critical bugs that violate these guarantees. Furthermore, there are conflicting and seemingly unimplemented refactoring plans recorded in the design history.

Here is a detailed breakdown of the inconsistencies:

### 1. Direct Contradictions Between `README_utils.md` and Code Behavior

This is the most critical category, as the primary design document for the `utils` library makes claims that are directly contradicted by the code's implementation.

| Utility | Documented Behavior (`README_utils.md`) | Actual Behavior (from Code Review) | Inconsistency & Impact |
| :--- | :--- | :--- | :--- |
| **`FileLock`** | Claims to provide a unified interface over `flock()` on POSIX and **`LockFileEx()` on Windows**. | The Windows implementation **never calls `LockFileEx()`**. It only opens a file handle, providing no inter-process locking at all. | **Critical.** The core functionality of the class is completely missing on one of its target platforms. The documentation is dangerously misleading. |
| **`JsonConfig`** | Claims to be a "**process-safe**" interface that uses an advisory `FileLock` for inter-process safety. | Because `FileLock` is non-functional on Windows, `JsonConfig` is **not process-safe on Windows**. | **Critical.** The guarantee of process-safety is false. This can lead to configuration file corruption in any multi-process application on Windows. |
| **`Logger`** | Claims "Graceful shutdown is managed by the library's `Finalize()` function." | The shutdown logic has a race condition that can **cause messages to be silently lost.** | **Major.** The shutdown is not fully "graceful" or reliable. Critical log messages may be lost without any warning. |
| **`Logger`** | Claims its design "eliminat[es] complex locking around file handles." | This is true for *thread-safety* but misleading for *process-safety*. The logger uses **no inter-process locking**, leading to file corruption when used by multiple processes. | **Major.** The documentation implies the design is sufficient for all concurrency, but it is not process-safe, a crucial omission for a general-purpose utility library. |

### 2. Conflicting and Unimplemented Design Plans

The documentation trail reveals a history of identified bugs and planned refactorings. Several of these plans appear to be incomplete or contradict each other, indicating architectural churn.

1.  **Contradictory `FileLock` Fixes:**
    *   `docs/FileLock_macOS_Bug_20251223.md` states the correct fix for a POSIX bug is to use a **"persistent file descriptor"** throughout the lock attempt.
    *   `docs/fix_filelock_multiprocess_worker.md` describes a fix for a similar bug by stating the opposite: "on every single attempt... a **new file descriptor is acquired**" and then closed on failure.
    *   **Inconsistency:** These two documents propose mutually exclusive solutions to the same underlying problem. This indicates confusion about the correct locking semantics on POSIX. The "persistent file descriptor" approach is the correct one.

2.  **Unimplemented `JsonConfig` Refactoring:**
    *   `docs/jsonconfig_revision_20251223.md` details a major architectural change for `JsonConfig` to solve a "lost update" problem. It describes a new, explicit API where users must call `cfg.lock()` and `cfg.unlock()`, and where write operations are guarded.
    *   **Inconsistency:** The code review from `code_review_20251228.md` indicates `JsonConfig` still uses an *internal* `FileLock` and does not mention this explicit locking API. This suggests the major refactoring described in the `jsonconfig_revision` document was **never implemented.** The "lost update" problem likely still exists.

3.  **Unconfirmed `Logger` Deadlock Fix:**
    *   `docs/logger_fix_20251223.md` identifies a deadlock on Windows caused by creating file sinks on the logger's background thread. It outlines a fix where sink creation is moved to the calling thread.
    *   **Inconsistency:** It is unclear from the subsequent code review if this fix was ever implemented. The review focused on different `Logger` bugs (multi-process safety and shutdown data loss), suggesting other critical issues were present or discovered later.

### Summary

The documentation describes a high-quality, robust set of utilities that would be safe to use in any context. The reality is that the core concurrency-related components (`FileLock`, `Logger`, `JsonConfig`) are broken in critical ways and are not safe for multi-process use on at least one major platform. The historical documents show an awareness of some of these issues, but also reveal conflicting fix strategies and large-scale refactorings that appear to have been abandoned.