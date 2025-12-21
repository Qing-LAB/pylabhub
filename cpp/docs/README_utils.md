# C++ Utilities (`pylabub::utils`) Documentation

This document provides design and usage notes for the core C++ utilities found in the `pylabub::utils` namespace.

---

## Library Lifecycle Management

To ensure robust behavior and graceful shutdown of all components, particularly asynchronous systems like the `Logger`, the utility library provides explicit lifecycle functions. All applications using this library should call them.

-   `pylabhub::utils::Initialize()`: Call this once at the beginning of your application (e.g., at the start of `main`). It ensures all subsystems are ready and runs any registered initializers.
-   `pylabhub::utils::Finalize()`: Call this once at the very end of your application (e.g., before returning from `main`). It runs all registered finalizers and then guarantees that all pending operations, such as writing log messages, are completed before the program exits.

**Important Note on Explicit Shutdown:** Calling `Finalize()` is mandatory for ensuring a robust and graceful shutdown.

If `Finalize()` is not called, the destructors of static objects (like the `Logger`) will be invoked by the C++ runtime during program termination. While a warning will be printed to `stderr`, `shutdown()` is **not** automatically called. Attempting to perform a `shutdown()` during static deinitialization can cause deadlocks and crashes, particularly on Windows. This is because the operating system holds internal locks (the "loader lock") during process teardown, and complex operations like thread synchronization are unsafe in this context.

Therefore, the only safe and portable way to shut down the library is to explicitly call `Finalize()` from `main()` before the program exits. The RAII pattern (e.g., creating a lifecycle management object on the stack in `main`) is the recommended way to guarantee this.

### Extensibility with Registration Hooks

You can register your own functions to be executed during the `Initialize` and `Finalize` phases.

-   `RegisterInitializer(func)`: Registers a function to be run during initialization.
-   `RegisterFinalizer(name, func, timeout)`: Registers a named cleanup function to be run during finalization. Finalizers are executed in **Last-In, First-Out (LIFO)** order.

### Usage

```cpp
#include "utils/Lifecycle.hpp"
#include "utils/Logger.hpp"

void my_cleanup_function() {
    // ... clean up custom application resources ...
}

int main(int argc, char* argv[]) {
    // Register a finalizer to be called during shutdown.
    pylabhub::utils::RegisterFinalizer("MyCleanup", my_cleanup_function, std::chrono::seconds(2));

    // Initialize the utility library at the start.
    pylabhub::utils::Initialize();

    // ... Application logic ...
    LOGGER_INFO("Application is running.");

    // Finalize the library at the end for graceful shutdown.
    pylabhub::utils::Finalize();

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

-   **RAII (Resource Acquisition Is Initialization):** The lock is acquired in the constructor and automatically released when the object goes out of scope, preventing leaked locks.
-   **Advisory Nature:** It only prevents contention between processes that *also* use `FileLock`. It does not use mandatory OS-level locks.
-   **Cross-Platform:** It provides a unified interface over `flock()` on POSIX systems and `LockFileEx()` on Windows.
-   **Two-Level Locking:** It correctly handles both multi-process and multi-thread contention by using a combination of an OS-level file lock and a process-local mutex registry.

---

## `AtomicGuard`

`AtomicGuard` is a high-performance, token-based ownership guard for managing exclusive access to a resource in a concurrent environment.

### Design Philosophy

-   **Hybrid Concurrency**: It uses a lock-free fast path (a single atomic compare-and-swap) for the common cases of `acquire()` and `release()`, ensuring minimal overhead. Complex operations like ownership transfer (`transfer_to`) use a blocking mutex to ensure correctness.
-   **RAII and Movability**: The guard follows RAII principles for automatic lock release. It is movable (`std::move`) to support modern C++ patterns like returning from factory functions, but it is not copyable.
-   **ABI Stability**: It uses the Pimpl idiom to guarantee ABI stability for the shared library.

It is a sophisticated utility designed for building higher-level concurrency primitives.

