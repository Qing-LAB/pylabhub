| Property       | Value                                        |
| -------------- | -------------------------------------------- |
| **HEP**        | `core-0003`                                  |
| **Title**      | Cross-Platform RAII File Locking (FileLock)  |
| **Author**     | Gemini AI                                    |
| **Status**     | Draft                                        |
| **Category**   | Core                                         |
| **Created**    | 2026-01-30                                   |
| **C++-Standard** | C++20                                        |

## Abstract

This Hub Enhancement Proposal (HEP) outlines the design and implementation of the `FileLock` module, a cross-platform, RAII-style advisory file locking mechanism. `FileLock` provides robust synchronization for both multi-process and multi-threaded applications interacting with shared filesystem resources, ensuring data integrity and preventing race conditions.

## Motivation

In complex applications, multiple processes or threads often need to access and modify shared files (e.g., configuration files, data caches). Without proper synchronization, concurrent access can lead to data corruption, inconsistent states, and application crashes. Existing platform-specific locking primitives can be cumbersome to use correctly and portably.

The `FileLock` module aims to address these challenges by providing:
- A simple, RAII-based API that guarantees lock release.
- Consistent cross-platform behavior for both inter-process and intra-process synchronization.
- Support for blocking, non-blocking, and timed lock acquisition.
- Integration with the `LifecycleManager` for controlled startup and shutdown, including optional cleanup of stale lock files.

## Rationale and Design

The `FileLock` module is designed for reliability, ease of use, and portability, adhering to modern C++ best practices.

### Core Principles

-   **RAII (Resource Acquisition Is Initialization)**: The lock is acquired in the constructor and automatically released in the destructor. This idiom is central to `FileLock`'s safety, preventing deadlocks by ensuring locks are always released, even during exception propagation.

-   **Two-Layer Locking Model**: To provide consistent semantics across different operating systems for both inter-process and intra-process synchronization, `FileLock` employs a dual-layer approach:
    -   **Inter-Process (OS-Level)**: Utilizes native OS advisory file locking primitives (`flock` on POSIX systems, `LockFileEx` on Windows) on a dedicated `.lock` file. This layer guarantees that only one process can hold the exclusive lock at any given time.
    -   **Intra-Process (Application-Level)**: A process-local registry (a `std::unordered_map` mapping canonical lock paths to `ProcLockState` objects) manages contention between threads within the same application process. This registry, protected by a `std::mutex` and `std::condition_variable`, ensures that threads within the same process respect the blocking/non-blocking semantics consistently, as OS-level file locks can behave inconsistently for threads of the same process.

-   **Advisory Lock**: `FileLock` implements an *advisory* locking mechanism. This means that all cooperating processes and threads must explicitly use `FileLock` to respect the lock. It does not prevent non-cooperating applications from directly accessing the locked resource and potentially corrupting data.

-   **Separate Lock File**: Instead of directly locking the target resource (e.g., `data.json`), `FileLock` operates on a separate, dedicated lock file (e.g., `data.json.lock`). This design choice simplifies implementation by avoiding potential conflicts with read/write operations on the actual data file and allows for more flexible cleanup strategies.

-   **Path Canonicalization**: To ensure that logically identical but syntactically different paths (e.g., `/a/./b` vs `/a/b`, or paths involving symlinks) contend for the same lock, `FileLock` canonicalizes the resource path. It uses `std::filesystem::canonical` if the path exists, falling back to `std::filesystem::absolute` for non-existent paths (enabling locking of resources before creation). This prevents "phantom locks" or multiple locks for the same resource.

-   **Lifecycle Integration**: `FileLock` is designed as a module for the `LifecycleManager`. This integration ensures:
    -   **Controlled Initialization**: `FileLock`'s internal data structures are properly set up via a startup callback before any `FileLock` objects are constructed.
    -   **Optional Cleanup**: A configurable shutdown callback (`cleanup_on_shutdown` flag in `GetLifecycleModule`) allows the application to attempt removal of stale `.lock` files (left by crashed processes) during graceful shutdown.

-   **ABI Stability (Pimpl Idiom)**: The public `FileLock` class uses the Pimpl (Pointer to Implementation) idiom. All platform-specific details, internal data members, and complex types are hidden within a private `FileLockImpl` struct, ensuring a stable Application Binary Interface (ABI) for the shared library.

### API Specification

#### `FileLock` Class

```cpp
class PYLABHUB_UTILS_EXPORT FileLock {
public:
    enum class LockMode { Blocking, NonBlocking };
    enum class ResourceType { File, Directory };

    static ModuleDef GetLifecycleModule(bool cleanup_on_shutdown = false);
    static bool lifecycle_initialized() noexcept;
    static std::filesystem::path get_expected_lock_fullname_for(const std::filesystem::path &path, ResourceType type) noexcept;

    // Constructors for direct lock acquisition
    explicit FileLock(const std::filesystem::path &path, ResourceType type, LockMode mode = LockMode::Blocking) noexcept;
    explicit FileLock(const std::filesystem::path &path, ResourceType type, std::chrono::milliseconds timeout) noexcept;

    // Factory methods for optional lock acquisition (modern C++ idiom)
    [[nodiscard]] static std::optional<FileLock> try_lock(const std::filesystem::path &path, ResourceType type, LockMode mode = LockMode::Blocking) noexcept;
    [[nodiscard]] static std::optional<FileLock> try_lock(const std::filesystem::path &path, ResourceType type, std::chrono::milliseconds timeout) noexcept;

    // Move semantics (non-copyable)
    FileLock(FileLock &&other) noexcept;
    FileLock &operator=(FileLock &&other) noexcept;
    FileLock(const FileLock &) = delete;
    FileLock &operator=(const FileLock &) = delete;

    // Destructor (releases lock)
    ~FileLock();

    // State and Error Accessors
    bool valid() const noexcept;
    std::error_code error_code() const noexcept;
    std::optional<std::filesystem::path> get_locked_resource_path() const noexcept;
    std::optional<std::filesystem::path> get_canonical_lock_file_path() const noexcept;

    // Internal cleanup function (called by LifecycleManager if configured)
    static void cleanup();
};
```

#### Enums

-   `LockMode`:
    -   `Blocking`: The constructor/`try_lock` call will wait indefinitely until the lock is acquired.
    -   `NonBlocking`: The constructor/`try_lock` call will return immediately if the lock cannot be acquired.
-   `ResourceType`: Used to generate unique lock file names for different resource types.
    -   `File`: The target resource is a file (e.g., `resource.txt.lock`).
    -   `Directory`: The target resource is a directory (e.g., `resource.dir.lock`).

### Lifecycle Integration

-   **Startup**: The `FileLock` module's startup callback initializes internal data structures and sets an atomic flag (`g_filelock_initialized`) to `true`. Attempts to construct `FileLock` objects before this flag is set will result in a `PLH_PANIC`.
-   **Shutdown (`cleanup_on_shutdown`)**: The `GetLifecycleModule` static factory function accepts a `cleanup_on_shutdown` boolean. If `true`, the `FileLock::cleanup()` static method is registered as a shutdown callback. `cleanup()` attempts a best-effort removal of `.lock` files that *might* have been left by a previous, crashed instance of the application. It acquires a non-blocking lock on each candidate `.lock` file; if successful, it means no other process holds the lock, and the file is safely deleted.

## Risk Analysis and Mitigations

-   **Risk**: `FileLock` relies on *advisory* locking, which can be ignored by non-cooperating processes.
    -   **Mitigation**: This is an inherent limitation of advisory locks. The `FileLock` design assumes all interacting components are cooperative. For mandatory locking, OS-specific mechanisms would be required, sacrificing portability.
-   **Risk**: Performance overhead due to polling in `Timed` or `NonBlocking` modes (especially on POSIX systems without direct timed `flock`).
    -   **Mitigation**: The `LOCK_POLLING_INTERVAL` (20ms) is chosen as a reasonable balance between responsiveness and CPU usage. Applications with extremely high contention might observe higher CPU usage during lock waiting periods.
-   **Risk**: Stale lock files can be left behind if a process crashes without `cleanup_on_shutdown` enabled.
    -   **Mitigation**: The `cleanup_on_shutdown` option mitigates this during graceful application exits. However, for hard crashes, manual cleanup or a dedicated watchdog process might be necessary. The trade-off (potential for interfering with other processes) is explicitly documented for `cleanup_on_shutdown`.
-   **Risk**: Unreliable behavior on network filesystems (e.g., NFS).
    -   **Mitigation**: A `WARNING` is explicitly included in the `file_lock.hpp` documentation. `FileLock` is designed primarily for local filesystem synchronization.
-   **Risk**: Deadlocks if client code attempts to acquire a lock twice within the same process/thread.
    -   **Mitigation**: The two-layer locking model (specifically the intra-process registry) handles this. A thread attempting to re-acquire an already held lock for the same path will either block (in `Blocking` mode) or fail immediately (in `NonBlocking` mode), preventing self-deadlock. It won't cause an actual system deadlock, but rather a programmatic one or an acquisition failure.

## Copyright

This document is placed in the public domain or under the CC0-1.0-Universal license, whichever is more permissive.
