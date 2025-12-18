# C++ Utilities (`pylabub::utils`) Documentation

This document provides design and usage notes for the core C++ utilities found in the `pylabub::utils` namespace.

---

## `JsonConfig`

The `JsonConfig` class provides a robust, thread-safe, and process-safe interface for managing JSON configuration files.

### Design Philosophy

- **Concurrency:** The class is designed for heavy concurrent use, both from multiple threads within a single process and from multiple cooperating processes.
- **Robustness:** On-disk file writes are atomic, ensuring that the configuration file is never left in a partially-written or corrupt state, even if the application crashes.
- **Safety:** The public API is `noexcept`, catching internal errors and translating them into simple boolean return values to prevent exceptions from propagating into consumer code.
- **ABI Stability:** The library is designed to maintain a stable Application Binary Interface (ABI), which is critical for a shared library that may be updated without forcing consumers to recompile. This is achieved through two complementary techniques:
    1.  **Pimpl Idiom (`std::unique_ptr<Impl>`)**: All private data members are hidden within a forward-declared `Impl` struct. This means the size and memory layout of the public class (`JsonConfig`) never changes, even if internal variables are added or removed.
    2.  **Controlled Symbol Export**: The library's build system automatically generates a `pylabhub_utils_export.h` header. Every public class intended for use by consumers is explicitly marked with the `PYLABHUB_UTILS_EXPORT` macro. On Windows, this expands to `__declspec(dllexport)` or `__declspec(dllimport)`, and on other platforms, it controls symbol visibility. This practice ensures that only the intended classes are part of the public API, preventing internal implementation details from accidentally becoming part of the library's ABI.

### Concurrency Model

`JsonConfig` uses a sophisticated two-level locking strategy to balance performance and safety.

---

## `FileLock`

`FileLock` is a cross-platform, RAII-style utility for creating *advisory* inter-process and inter-thread locks.

### Design Philosophy & Usage

- **RAII (Resource Acquisition Is Initialization):** The lock is acquired in the constructor and automatically released when the object goes out of scope. This prevents leaked locks, even in the presence of exceptions or errors.

- **Explicit Resource Type:** The constructor requires you to explicitly state whether you are locking a `File` or a `Directory`. This makes the API safer and more self-documenting.

  ```cpp
  using pylabhub::utils::FileLock;
  using pylabhub::utils::ResourceType;

  {
      // Lock a file resource
      FileLock lock("/path/to/resource.txt", ResourceType::File);
      if (lock.valid()) {
          // Lock acquired, proceed with critical section
      } else {
          // Failed to acquire lock, handle error
      }
  } // Lock is automatically released here
  ```

- **Advisory Nature:** `FileLock` only prevents contention between processes that *also* use `FileLock`. It does not use mandatory OS-level locks and will not prevent a non-cooperating process from accessing the target resource.

- **Cross-Platform:** It provides a unified interface over `flock()` on POSIX systems and `LockFileEx()` on Windows.

- **Locking Strategy:** To avoid interfering with the target resource, `FileLock` operates on a separate, empty lock file. The naming convention is designed to be unambiguous and prevent collisions between file and directory targets based on the `ResourceType` provided:
  - **`ResourceType::File`:** For a target file like `/path/to/data.json`, the lock file is created as `/path/to/data.json.lock`.
  - **`ResourceType::Directory`:** For a target directory like `/path/to/my_dir`, the lock file is created as `/path/to/my_dir.dir.lock`.

- **Locking Modes:** The constructor accepts a `LockMode` to control its behavior when a lock is already held:
  - `LockMode::Blocking`: (Default) Waits indefinitely until the lock can be acquired.
  - `LockMode::NonBlocking`: Returns immediately if the lock cannot be acquired. `valid()` will be `false`.
  - **Timed:** A constructor overload accepts a `std::chrono::milliseconds` duration, attempting to acquire the lock until the timeout expires.

### Concurrency Model

`FileLock` uses a two-level locking strategy to ensure correctness for both multi-threaded and multi-process scenarios:

1.  **Process-Local Lock:** An in-memory, thread-safe registry (`std::mutex` and `std::condition_variable`) is used to serialize lock attempts *within the same process*. This is critical for providing consistent behavior on platforms like Windows where native file locks are per-process and do not block other threads in the same process.

2.  **OS-Level Lock:** Once the process-local lock is acquired, the class attempts to acquire the system-wide OS lock (`flock`/`LockFileEx`) on the designated `.lock` or `.dir.lock` file. This handles contention between different processes.

#### Critical Risk Analysis: Concurrent Move Operations

The high-performance locking model (using `rwMutex` for simple accessors) introduces a critical rule for users of the class:

**WARNING: You MUST externally synchronize move operations.**


If one thread attempts to move a `JsonConfig` object while another thread is calling any method on it, a **use-after-free** memory error will occur, leading to a crash.

**Scenario:**
1.  **Thread A** calls `config.get("key")` and passes the initial checks.
2.  **Thread B** executes `JsonConfig new_config = std::move(config);`. This action transfers the internal implementation pointer from `config` to `new_config` and deallocates the memory `config` was managing.
3.  **Thread A** resumes execution and attempts to access its now-dangling internal pointer, causing a crash.

This is a deliberate design trade-off that prioritizes performance. The responsibility is on the consumer to ensure that an object is not being used by one thread while being moved by another. For long-lived, shared objects (like a global configuration singleton), consider making the object `const` or avoiding move operations after initialization.
