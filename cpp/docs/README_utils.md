# `pylabhub-utils` Library

The `pylabhub-utils` library is a shared library that provides a collection of common, robust utilities for C++ applications. These modules are designed to be thread-safe, process-safe, and integrated into a common lifecycle management system.

**Core Design Principles:**

*   **Lifecycle Management**: All major utilities are managed as lifecycle modules. They must be registered and initialized at application startup to ensure proper functioning and cleanup.
*   **ABI Stability**: The library is built as a shared library with a stable Application Binary Interface (ABI), enforced through the Pimpl idiom and explicit symbol visibility control.
*   **Safety**: Utilities are designed with thread and process safety as a primary concern, using native OS primitives for robust locking.
*   **Modern C++**: The library is written in C++20.

---

## Core Utility Modules

These modules form the core of the `pylabhub-utils` library and often depend on the Lifecycle manager.

### `Lifecycle` (C++ Namespace: `pylabhub::utils`)

The `Lifecycle` module provides a dependency-aware manager for application startup and shutdown. It ensures that components are initialized in the correct order and shut down gracefully.

*   **Design Principles**:
    *   Uses a `LifecycleManager` singleton to manage all modules.
    *   Modules are defined using `ModuleDef`, specifying a name, dependencies, and startup/shutdown callbacks.
    *   A topological sort of the dependency graph determines the correct initialization order. Shutdown occurs in the reverse order.
    *   Supports both **static modules** (core components registered before `initialize()`) and **dynamic modules** (components registered and loaded at runtime).
    *   Shutdown callbacks have timeouts to prevent the application from hanging on exit.

*   **Usage**:
    ```cpp
    #include "utils/Lifecycle.hpp"
    #include "utils/Logger.hpp" // Example module

    int main(int argc, char* argv[]) {
        // Create a scope guard to ensure finalize() is called on exit.
        pylabhub::utils::LifecycleManager::Scope lifecycle_scope;

        // Register all static modules before initialization.
        pylabhub::utils::LifecycleManager::instance().register_module(
            pylabhub::utils::Logger::GetLifecycleModule());
        // ... register other modules

        // Initialize all static modules.
        pylabhub::utils::LifecycleManager::instance().initialize();

        // ... application logic ...

        return 0; // finalize() is called automatically here
    }
    ```

### `Logger` (C++ Namespace: `pylabhub::utils`)

The `Logger` is a high-performance, asynchronous, thread-safe logging framework.

*   **Design Principles**:
    *   Operates as a singleton (`Logger::instance()`).
    *   Log messages are pushed to a queue and processed by a dedicated background worker thread, minimizing application latency.
    *   A sink-based architecture allows routing logs to different outputs (`ConsoleSink`, `FileSink`, `RotatingFileSink`, etc.).
    *   Uses the `{fmt}` library for efficient, compile-time-checked format strings.

*   **Usage**:
    ```cpp
    #include "utils/Logger.hpp"

    // In main(), register Logger::GetLifecycleModule() with LifecycleManager.

    void do_work() {
        LOGGER_INFO("Starting work on task {}", 42);
        try {
            // ...
        } catch (const std::exception& e) {
            LOGGER_ERROR("An error occurred: {}", e.what());
        }
    }
    ```

### `FileLock` (C++ Namespace: `pylabhub::utils`)

The `FileLock` utility provides a robust, RAII-style mechanism for cross-process and cross-thread file and directory locking.

*   **Design Principles**:
    *   RAII-based: The lock is acquired upon construction and automatically released upon destruction.
    *   Uses native OS-level advisory locking primitives (`flock` on POSIX, `LockFileEx` on Windows) for true process safety.
    *   Supports `Blocking`, `NonBlocking`, and `Timed` lock acquisition.

*   **Usage**:
    ```cpp
    #include "utils/FileLock.hpp"

    void access_shared_resource(const std::filesystem::path& path) {
        pylabhub::utils::FileLock lock(path, pylabhub::utils::ResourceType::File);
        if (lock.valid()) {
            // Safely access the resource
        } else {
            LOGGER_ERROR("Failed to acquire lock for {}", path.string());
        }
        // Lock is automatically released here.
    }
    ```

### `JsonConfig` (C++ Namespace: `pylabhub::utils`)

`JsonConfig` is a thread-safe and process-safe manager for JSON configuration files.

*   **Design Principles**:
    *   Uses `FileLock` internally to ensure process-safe file I/O.
    *   Employs a `std::shared_mutex` to allow concurrent in-memory reads and exclusive writes.
    *   Features an **atomic write** mechanism (write-to-temp then rename) to prevent file corruption.
    *   Access is managed through RAII guards (`ReadLock`, `WriteLock`) or a fluent transaction API.

*   **Usage**:
    ```cpp
    #include "utils/JsonConfig.hpp"

    void use_config(pylabhub::utils::JsonConfig& config) {
        // Reading a value
        if (auto r = config.lock_for_read()) {
            auto name = r->json().value("name", "default");
        }

        // Writing a value
        if (auto w = config.lock_for_write()) {
            w->json()["last_run"] = std::time(nullptr);
            w->commit();
        }
    }
    ```

### `SharedMemoryHub` (C++ Namespace: `pylabhub::hub`)

The Data Exchange Hub provides a high-performance inter-process communication (IPC) framework based on a central broker for service discovery.

*   **Important Notice**: This is a **dynamic lifecycle module**. It must be registered and loaded *after* the `LifecycleManager` has been initialized.

*   **Design Principles**:
    *   **Hub**: The primary entry point, created via `Hub::connect()`, which manages the connection to the broker.
    *   **Channels**: Supports high-performance shared memory (`SharedMemoryProducer`/`SharedMemoryConsumer`) and general-purpose ZeroMQ messaging (Pub/Sub, Req/Rep).

*   **Usage**:
    ```cpp
    #include "utils/Lifecycle.hpp"
    #include "utils/SharedMemoryHub.hpp"

    void use_hub(pylabhub::hub::BrokerConfig& config) {
        // In main(), register and load the dynamic module.
        pylabhub::utils::LifecycleManager::instance().register_dynamic_module(
            pylabhub::hub::GetLifecycleModule());
        pylabhub::utils::LifecycleManager::instance().load_module("pylabhub::hub::DataExchangeHub");

        auto hub = pylabhub::hub::Hub::connect(config);
        if (!hub) return;

        auto producer = hub->create_shm_producer("my_channel", 1024 * 1024);
        if (producer) {
            void* buffer = producer->begin_publish();
            // ... write to buffer ...
            producer->end_publish(512, 0.0, 0, {});
        }
    }
    ```

---

## Specialized and Header-Only Utilities

These utilities are also available in `src/include/utils/` and provide more specific functionality.

### `PythonLoader` (C++ Namespace: `pylabhub::utils`)

The `PythonLoader` provides a C-style, ABI-stable interface for managing an embedded Python interpreter. This utility is designed specifically for integration with C-based frameworks like the Igor Pro XOP toolkit.

*   **Key Features**:
    *   **Dynamic Loading**: Does not link against a specific Python library at compile time, instead loading it from a user-specified path at runtime.
    *   **State Management**: Provides explicit `extern "C"` functions to initialize (`PyLoader_init`), configure (`PySetPython`), execute (`PyExec`), and shut down (`PyCleanup`) the interpreter.
    *   **Igor Pro Integration**: The function signatures and `#pragma pack(2)` data structures are designed for direct compatibility with Igor Pro's XOP API.

*   **Usage Context**: This is a specialized utility. Its primary use case is within the `src/IgorXOP` project to enable Python scripting inside Igor Pro.

### Header-Only Concurrency Guards (C++ Namespace: `pylabhub::basics`)

The library includes several header-only, RAII-style guards for managing concurrency and scope.

*   **`ScopeGuard`**: A general-purpose guard that executes a callable object (lambda) upon scope exit. This is useful for ensuring cleanup actions are always performed, even if exceptions are thrown.
    *   **Usage**: `auto guard = make_scope_guard([&]{ ... cleanup code ... });`

*   **`AtomicGuard`**: A fast, stateless, spinlock-like guard for protecting critical sections in high-contention, multi-threaded scenarios. It operates on an `AtomicOwner` object.
    *   **Usage**: `AtomicOwner owner; ... AtomicGuard guard(&owner, /*tryAcquire=*/true); if (guard.active()) { ... }`

*   **`RecursionGuard`**: A thread-local guard to detect and prevent re-entrant function calls on a per-object basis.
    *   **Usage**: `if (RecursionGuard::is_recursing(this)) { return; } RecursionGuard guard(this);`
