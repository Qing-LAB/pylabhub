# JsonConfig Refactoring and Test Failure Analysis - 2025-12-23

This document summarizes the investigation into the project's build and test failures on macOS and outlines the final, agreed-upon refactoring plan for the `JsonConfig` class.

## 1. Initial Problem: Build Failure

*   **Symptom:** The initial build failed during the CMake configuration step, specifically when trying to run the `gtest_discover_tests` command.
*   **Root Cause:** A complex and fragile dependency graph for the project's "staging" architecture. The order of operations was not guaranteed, causing tests to run before their shared library dependencies were staged correctly.
*   **Resolution:**
    1.  The staging dependency graph was refactored to be linear and robust. Each module that defines a `stage_*` target is now responsible for declaring its own dependency on the `create_staging_dirs` target.
    2.  The test build process in `tests/CMakeLists.txt` was corrected to enforce that `stage_pylabhub_utils` runs before test discovery.
    3.  The `BUILD_RPATH` for the test executables was set on macOS/Linux to ensure the dynamic linker could find the staged shared libraries.
    4.  The project's design document (`README_CMake_Design.md`) was updated to reflect these new best practices.

## 2. Current Problem: Multi-Process Test Failures

*   **Symptom:** After fixing the build, `ctest` was run. While most tests passed, a specific subset of multi-process tests failed consistently:
    *   `FileLockTest.MultiProcessNonBlocking`
    *   `FileLockTest.MultiProcessBlockingContention`
    *   `FileLockTest.MultiProcessParentChildBlocking`
    *   `JsonConfigTest.MultiProcessContention`
*   **Analysis:** All failing tests involved child processes that crashed (`libc++abi: terminating`) while interacting with `FileLock` or `JsonConfig`. We determined that child processes, being new OS processes, were not correctly initializing the `pylabub::utils` library.
*   **Intermediate Fix:** We deduced that the `main()` function of the test entrypoint and all worker functions executed in child processes must manage their own `Initialize()` and `Finalize()` calls.

## 3. Final Problem: `JsonConfigTest` "Lost Update" Flaw

*   **Symptom:** Even with the lifecycle calls in place, `JsonConfigTest.MultiProcessContention` remained flaky. The test expected all 16 worker processes to successfully write to the config file, but some were failing.
*   **Root Cause (Your Insight):** We identified a critical "lost update" problem. The `JsonConfig` API was not safe for a multi-process read-modify-write pattern. Each process would read the file (`init()`), modify its own stale in-memory copy, and then overwrite the results of other processes, even when using a `FileLock`.
*   **The Agreed-Upon Design:** We designed a new, robust architecture for `JsonConfig` based on an **explicit, stateful locking model**.

## 4. The New `JsonConfig` Architectural Design

1.  **Philosophy:** `JsonConfig` manages in-memory data. The **caller** manages file locking and I/O transactions.
2.  **Stateful `FileLock`:** A `FileLock` object is a private member of the `JsonConfig` class, representing its `locked`/`unlocked` state.
3.  **Explicit API:** The user must now call new public methods (`cfg.lock()`, `cfg.unlock()`) to obtain and release exclusive, process-safe write access to the file.
4.  **Safety Guards:** All write methods (`save()`, `replace()`, etc.) are guarded. They will fail if the `JsonConfig` object is not in a `locked` state.
5.  **Reload on Lock:** A successful call to `lock()` will **automatically reload** the data from disk, ensuring the caller is always operating on the freshest possible data and preventing the "lost update" problem.
6.  **Read Method Freedom:** Read-only methods (`get()`, `as_json()`) remain lock-free, operating on the current in-memory data, as per your design.

## 5. Action Plan to Complete Refactoring (Next Session)

This is the step-by-step plan to pick up where we left off.

1.  **Refactor `include/utils/JsonConfig.hpp`**:
    *   Add `std::unique_ptr<FileLock> fileLock;` to the private `Impl` struct.
    *   Add the new public API methods to the class definition: `lock(LockMode)`, `lock_for(...)`, `unlock()`, and `is_locked()`.
    *   Update the documentation for write methods (`save`, `replace`, etc.) to state that they now require the object to be locked.
    *   Rename the internal private helper methods to clarify their purpose (e.g., `reload_under_lock_io`).

2.  **Refactor `src/utils/JsonConfig.cpp`**:
    *   Implement the new public locking methods. `lock()` and `lock_for()` are the core of the new design and must call the internal reload method upon acquiring a lock.
    *   Remove all internal `FileLock` acquisitions from the old public methods (`init`, `save`, `reload`).
    *   Add the `if (!is_locked())` guard to all public write methods.
    *   Implement the raw I/O helper methods (`reload_under_lock_io`, `save_under_lock_io`).
    *   Simplify `init()` to only set the file path and handle the `createIfMissing` flag without locking or reloading.

3.  **Refactor `tests/helpers/workers.cpp`**:
    *   Update the `worker::jsonconfig::write_id` function to use the new, explicit API. The logic will be a retry loop that calls `cfg.lock()`. Upon success, it will perform the read-modify-write for the accumulative retry count, call `cfg.save()`, and then `cfg.unlock()`.

4.  **Refactor `tests/test_jsonconfig.cpp`**:
    *   Update the `JsonConfigTest.MultiProcessContention` test to use the new `JsonConfig` API and to assert the final accumulated `total_retries` value.
    *   **Crucially, review all other tests in this file.** Any test that calls a write method like `save()` or `replace()` will now fail because they don't use the new locking mechanism. These tests must be updated to use the `cfg.lock()` / `cfg.save()` / `cfg.unlock()` pattern.

5.  **Final Verification**:
    *   Perform a clean build: `rm -rf build && cmake -S . -B build && cmake --build build`.
    *   Run all tests: `cd build && ctest --output-on-failure`.
    *   Confirm that all tests pass.
