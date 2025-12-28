# C++ Utilities (`pylabub::utils`) Documentation

This document provides design and usage notes for the core C++ utilities found in the `pylabub::utils` namespace.

---

## Application Lifecycle Management (`pylabhub::utils`)

The application lifecycle management system provides a robust, dependency-aware framework for controlling the application's startup and shutdown sequences. It is designed to give developers fine-grained control over the order of operations, ensuring a predictable, safe, and diagnosable application lifecycle.

The system is governed by a central singleton that enforces these core principles:

1.  **Module-Based Registration**: Instead of separate initializers and finalizers, the system manages "modules". A module is a logical unit with a unique name, a startup function, a shutdown function, and a list of dependencies.

2.  **Dependency-Driven Order**: The execution order is not based on registration order or simple priority, but on an explicitly declared dependency graph. You declare which modules must be started before others, and the system resolves the correct startup sequence using a topological sort. The shutdown sequence is automatically determined to be the exact reverse of the startup sequence.

3.  **Strict Lifecycle Phases**:
    *   **Registration Phase**: At the start of the program, all modules are registered along with their dependencies.
    *   **Execution Phase**: `InitializeApplication()` is called. This locks the system, resolves the dependency graph, and runs all module startup functions in the correct order. The application then enters its main operational state.
    *   **Finalization Phase**: `FinalizeApplication()` is called at program exit to run all module shutdown functions in the correct, reversed order.

4.  **Early Error Detection**: The system is designed to fail fast. Circular dependencies in the graph are detected during the initialization phase. Attempting to register a module after initialization has begun is a fatal error that terminates the program. This prevents silent failures and hard-to-debug runtime issues.

### The Three Phases in Detail

#### 1. The Registration Phase

This is the only phase where modules can be registered.

-   `pylabhub::utils::RegisterModule(Module lifecycle)`: Registers a module with the lifecycle system. The `Module` struct contains all necessary metadata:
    -   `name`: A unique `std::string` identifier for the module (e.g., "ConfigLoader", "Database").
    -   `dependencies`: A `std::vector<std::string>` of module names that this module depends on. They will be started *before* this module. An empty vector indicates no dependencies.
    -   `startup`: The `std::function<void()>` to be called during initialization.
    -   `shutdown`: A struct containing:
        -   `func`: The `std::function<void()>` to be called for cleanup during finalization.
        -   `timeout`: A `std::chrono::milliseconds` value for the shutdown function.

#### 2. The Execution Phase

This phase is initiated by a single function call.

-   `pylabhub::utils::InitializeApplication()`: This function marks the end of the Registration Phase. It should be called once, at the start of `main()`. Its responsibilities are to:
    1.  Permanently lock the registration system.
    2.  Build a dependency graph of all registered modules.
    3.  Perform a topological sort to calculate the startup sequence. If a cycle is detected (e.g., A depends on B, and B depends on A), it will terminate with a fatal error.
    4.  Store the calculated startup sequence and its reverse for finalization.
    5.  Execute the module `startup` functions one by one, in the resolved order.

#### 3. The Finalization Phase

This phase is initiated at the end of the program's life.

-   `pylabhub::utils::FinalizeApplication()`: This function should be called once at the very end of `main()`. It runs the module `shutdown` functions according to the sequence determined during initialization (the reverse of the startup order).

### Full Example

```cpp
#include "utils/Lifecycle.hpp"
#include "utils/Logger.hpp"

// A logging module that has no dependencies
void setup_logging() { /*...*/ }
void teardown_logging() { /*...*/ }

// A config module that depends on logging
void load_config() { /*...*/ }
void save_config() { /*...*/ }

int main(int argc, char* argv[]) {
    // === 1. REGISTRATION PHASE ===
    pylabhub::utils::RegisterModule({
        .name = "Logging",
        .startup = setup_logging,
        .shutdown = {.func = teardown_logging, .timeout = std::chrono::seconds(1)}
    });

    pylabhub::utils::RegisterModule({
        .name = "Config",
        .dependencies = {"Logging"}, // Depends on the Logging module
        .startup = load_config,
        .shutdown = {.func = save_config, .timeout = std::chrono::seconds(2)}
    });

    // === 2. EXECUTION PHASE ===
    // Locks registration, resolves dependencies, and runs startup functions.
    // Here, setup_logging() is guaranteed to run before load_config().
    pylabhub::utils::InitializeApplication();

    LOGGER_INFO("Application is running.");

    // === 3. FINALIZATION PHASE ===
    // Runs shutdown functions in reverse order of startup.
    // Here, save_config() is guaranteed to run before teardown_logging().
    pylabhub::utils::FinalizeApplication();

    return 0;
}
```

---

## `Logger`

The `Logger` provides a high-performance, asynchronous, and thread-safe logging utility.

### Design Philosophy

The logger is built on a **command-queue pattern** to ensure that logging calls from application threads have minimal performance impact.

1.  **Non-Blocking API**: Calls like `LOGGER_INFO(...)` are lightweight. They simply package the log message into a command and place it on a queue.
2.  **Dedicated Worker Thread**: A single background thread is the sole consumer of this queue. It handles all I/O (writing to the console or files), isolating I/O latency from the main application and eliminating complex locking around file handles.
3.  **Sink Abstraction**: A `Sink` base class allows for easy extension to different logging destinations (e.g., `ConsoleSink`, `FileSink`, `SyslogSink`).
4.  **Asynchronous Error Handling**: To prevent deadlocks, sink I/O errors are handled by a second dispatcher thread. This makes it safe for a user's error callback to make re-entrant calls back into the logger without hanging the application.

This asynchronous design makes the logger safe for high-throughput applications and prevents logging from becoming a performance bottleneck. Graceful shutdown is managed by the library's `Finalize()` function.

---

## `JsonConfig`

The `JsonConfig` class provides a robust, thread-safe, and process-safe interface for managing JSON configuration files.

### Design Philosophy

-   **Safety and Concurrency**: The class is designed for heavy concurrent use. To guarantee safety, it is **non-movable and non-copyable**. For ownership transfer (e.g., from a factory function), `std::unique_ptr<JsonConfig>` should be used. This design choice completely eliminates a class of potential race conditions.
-   **Robustness**: On-disk file writes are atomic (via a write-to-temporary-and-rename pattern), ensuring the configuration file is never left in a corrupt state, even if the application crashes.
-   **Exception Safety**: The public API is `noexcept`, catching internal errors (e.g., from file I/O or JSON parsing) and translating them into simple boolean return values.
-   **ABI Stability**: The class uses the Pimpl idiom (`std::unique_ptr<Impl>`) and controlled symbol exports to maintain a stable Application Binary Interface (ABI), which is critical for a shared library.

### Concurrency Model

`JsonConfig` uses a two-level locking strategy:

1.  **Inter-Process Safety**: An advisory `FileLock` is used for all disk operations to coordinate safely between multiple processes.
2.  **Intra-Process Safety**:
    -   A coarse-grained `std::mutex` (`initMutex`) serializes structurally significant operations like `init`, `save`, and `reload`.
    -   A fine-grained `std::shared_mutex` (`rwMutex`) protects the internal JSON data for all accessor methods (`get`, `set`, `with_json_read`, etc.), allowing for high-performance, concurrent reads.

---

## `FileLock`

`FileLock` is a cross-platform, RAII-style utility for creating *advisory* inter-process and inter-thread locks.

### Design Philosophy & Usage

-   **RAII (Resource Acquisition Is Initialization):** The lock is acquired in the constructor and automatically released when the object goes out of scope. This prevents leaked locks, even in the presence of exceptions or errors.

-   **Advisory Nature:** It only prevents contention between processes that *also* use this `FileLock` class for the same resource. It does not use mandatory OS-level locks and will not prevent a non-cooperating process from accessing the file.

-   **Path Canonicalization:** To be effective, the locking mechanism must ensure that different paths pointing to the same file (e.g., `../file.txt` vs. `/abs/path/to/file.txt`, or paths involving symlinks) resolve to the *same lock*. `FileLock` handles this automatically by canonicalizing the resource path upon construction. It uses `std::filesystem::canonical` to resolve symlinks, falling back to `std::filesystem::absolute` if the resource doesn't exist yet (to allow locking before creation).

-   **Cross-Platform:** It provides a unified interface over `flock()` on POSIX systems and `LockFileEx()` on Windows, handling the platform-specific details internally.

-   **Two-Level Locking:** It correctly handles both multi-process (inter-process) and multi-thread (intra-process) contention by using a combination of an OS-level file lock and a process-local mutex registry. This ensures that even on Windows, where `LockFileEx` locks are per-process, multiple threads in the same process will correctly wait for each other.

### Public API Highlights

-   `FileLock(path, type, mode)`: Constructor that acquires the lock.
-   `valid()`: Checks if the lock is held.
-   `get_expected_lock_fullname_for(path, type)`: A `static` utility to predict the canonical lock file name for a resource without creating a lock.
-   `get_canonical_lock_file_path()`: An instance method to get the actual lock file path being used by an active lock.

---

## `AtomicGuard`

`AtomicGuard` is a high-performance, token-based ownership guard for managing exclusive access to a resource in a concurrent environment.

### Design Philosophy

-   **Hybrid Concurrency**: It uses a lock-free fast path (a single atomic compare-and-swap) for the common cases of `acquire()` and `release()`, ensuring minimal overhead. Complex operations like ownership transfer (`transfer_to`) use a blocking mutex to ensure correctness.
-   **RAII and Movability**: The guard follows RAII principles for automatic lock release. It is movable (`std::move`) to support modern C++ patterns like returning from factory functions, but it is not copyable.
-   **ABI Stability**: It uses the Pimpl idiom to guarantee ABI stability for the shared library.

It is a sophisticated utility designed for building higher-level concurrency primitives.

