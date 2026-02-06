# `pylabhub-utils` Library

The `pylabhub-utils` library is a shared library that provides a collection of common, robust utilities for C++ applications. These modules are designed to be thread-safe, process-safe, and integrated into a common lifecycle management system.

**Core Design Principles:**

*   **Lifecycle Management**: All major utilities are managed as lifecycle modules. They must be registered and initialized at application startup to ensure proper functioning and cleanup.
*   **ABI Stability**: The library is built as a shared library with a stable Application Binary Interface (ABI), enforced through the Pimpl idiom and explicit symbol visibility control.
*   **Safety**: Utilities are designed with thread and process safety as a primary concern, using native OS primitives for robust locking.
*   **Modern C++**: The library is written in C++20.

---

**⚠️ Thread Safety and Concurrency Considerations:**

- **FileLock**: Process-safe and thread-safe. Uses both OS-level file locks and in-process synchronization.
- **Logger**: Fully thread-safe. Uses lock-free queue for log message submission.
- **JsonConfig**: Thread-safe for concurrent reads. Write operations are exclusive and process-safe via FileLock.
- **Lifecycle**: Module registration is NOT thread-safe and must occur before `initialize()`. Dynamic module loading/unloading IS thread-safe.
- **RecursionGuard**: Thread-local only. Does NOT prevent cross-thread recursion.

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
    *   Shutdown callbacks have configurable timeouts to prevent the application from hanging. The timeout is provided in milliseconds when defining a module with `ModuleDef::set_shutdown()`. This timeout is respected during both application finalization and dynamic module unloading.

*   **Usage (Static Modules)**:
    The recommended way to manage the application lifecycle is with the `LifecycleGuard` RAII helper. It ensures that `InitializeApp()` is called once when the first guard is created, and `FinalizeApp()` is called when that first "owner" guard goes out of scope.

    ```cpp
    #include "utils/Lifecycle.hpp"
    #include "utils/Logger.hpp" // Example module

    int main(int argc, char* argv[]) {
        // Create a LifecycleGuard and pass it the modules to register.
        // The first guard becomes the "owner" and manages the lifecycle.
        pylabhub::utils::LifecycleGuard app_lifecycle(
            pylabhub::utils::Logger::GetLifecycleModule()
            // ... add other static modules via MakeModDefList() if needed
        );

        // The constructor of the guard calls InitializeApp(), which starts all
        // modules in the correct dependency order. It is now safe to use them.
        LOGGER_INFO("Application started successfully with static modules.");

        // ... main application logic runs here ...

        return 0; // app_lifecycle destructor calls FinalizeApp() automatically
    }
    ```

### Dynamic Modules

Dynamic modules allow for runtime loading and unloading of application components, providing flexibility for optional features, plugins, or mode-specific functionality. They are integrated into the same dependency graph as static modules but offer on-demand lifecycle management.

*   **Key Characteristics**:
    *   **Runtime Registration**: Can be registered *after* `InitializeApp()` has been called.
    *   **On-demand Loading**: Loaded and started via `LoadModule()` when needed.
    *   **On-demand Unloading**: Unloaded and shut down via `UnloadModule()`.
    *   **Dependency-aware**: Automatically loads and unloads their dynamic dependencies.
    *   **Reference-counted**: The `LifecycleManager` tracks how many other currently loaded dynamic modules depend on a given dynamic module.
    *   **De-registration on Unload**: A successful `UnloadModule()` call completely removes the module from the `LifecycleManager`. To be loaded again, it must be re-registered.

*   **Usage (Dynamic Modules)**:

    First, define a dynamic module. Note that dynamic modules must be registered *after* the static application core is initialized.

    ```cpp
    #include "utils/Lifecycle.hpp"
    #include "utils/Logger.hpp"

    // Define a simple dynamic module
    namespace MyDynamicModule {
        void startup_func(const char* data) {
            LOGGER_INFO("MyDynamicModule started. Data: {}", data ? data : "N/A");
        }
        void shutdown_func(const char* data) {
            LOGGER_INFO("MyDynamicModule shut down. Data: {}", data ? data : "N/A");
        }
        pylabhub::utils::ModuleDef GetLifecycleModule() {
            pylabhub::utils::ModuleDef def("MyDynamicModule");
            def.set_startup(startup_func, "Startup Data", 12);
            def.set_shutdown(shutdown_func, 1000, "Shutdown Data", 13);
            return def;
        }
    } // namespace MyDynamicModule

    int main() {
        // Initialize the static application core
        pylabhub::utils::LifecycleGuard app_lifecycle(
            pylabhub::utils::Logger::GetLifecycleModule()
        );

        LOGGER_INFO("Static application core initialized.");

        // Register the dynamic module
        if (pylabhub::utils::RegisterDynamicModule(MyDynamicModule::GetLifecycleModule())) {
            LOGGER_INFO("MyDynamicModule registered successfully.");
        } else {
            LOGGER_ERROR("Failed to register MyDynamicModule.");
            return 1;
        }

        // Load the dynamic module
        if (pylabhub::utils::LoadModule("MyDynamicModule")) {
            LOGGER_INFO("MyDynamicModule loaded successfully.");
        } else {
            LOGGER_ERROR("Failed to load MyDynamicModule.");
            return 1;
        }

        // Use the dynamic module...
        LOGGER_INFO("MyDynamicModule is active now!");

        // Unload the dynamic module
        // This will succeed as no other modules depend on MyDynamicModule.
        if (pylabhub::utils::UnloadModule("MyDynamicModule")) {
            LOGGER_INFO("MyDynamicModule unloaded successfully and de-registered.");
        } else {
            LOGGER_ERROR("Failed to unload MyDynamicModule (it might still be referenced).");
            return 1;
        }

        // Attempting to load it again without re-registering will fail:
        if (!pylabhub::utils::LoadModule("MyDynamicModule")) {
            LOGGER_INFO("Attempt to load MyDynamicModule again failed as expected (it was de-registered).");
        }

        return 0; // app_lifecycle destructor calls FinalizeApp()
    }
    ```

*   **Error Handling and Important Considerations for Dynamic Modules**:
    *   **Loading Failure**: If `LoadModule()` returns `false`, the module (and its dependencies) failed to start. This is not fatal and allows the application to recover.
    *   **Unloading Failure**: `UnloadModule()` returns `false` if the module cannot be unloaded because it is still a dependency for other currently `LOADED` dynamic modules. You must unload its dependents first.
    *   **Re-registration**: After a successful `UnloadModule()`, the module definition is removed. If you wish to load it again, you must call `RegisterDynamicModule()` for it once more.
    *   **Persistent Modules**: A module can be marked as persistent during its definition (`ModuleDef::set_as_persistent(true)`). Persistent dynamic modules cannot be unloaded via `UnloadModule()`; they will only be shut down during the application's finalization.

### `Logger` (C++ Namespace: `pylabhub::utils`)

The `Logger` is a high-performance, asynchronous, thread-safe logging framework designed for applications where logging latency must not impact the performance of critical threads.

*   **Design Principles**:
    *   **Asynchronous Command Queue**: Logging calls are non-blocking. They format a message and place it on a lock-free queue, allowing the application thread to continue immediately.
    *   **Dedicated Worker Thread**: A single background thread processes the queue, handling all I/O operations (like writing to files or the console). This isolates I/O latency and serializes access to resources like file handles.
    *   **Sink-based Architecture**: A `Sink` abstraction allows routing logs to different outputs. The logger is easily extensible with new sinks.
    *   **Lifecycle Managed**: The logger's lifetime is managed by the `LifecycleManager`, ensuring it is started and shut down gracefully.
    *   **Compile-Time Format Strings**: Uses the `{fmt}` library with `fmt::format_string` in the default macros (`LOGGER_INFO`, etc.) to provide compile-time validation of format strings and arguments, preventing runtime formatting errors.

*   **Available Sinks**:
    *   `set_console()`: (Default) Logs to the standard error stream.
    *   `set_logfile()`: Logs to a single, specified file.
    *   `set_rotating_logfile()`: Logs to a file that automatically rotates when it reaches a certain size.
    *   `set_syslog()`: (POSIX-only) Logs to the system's `syslog` service.
    *   `set_eventlog()`: (Windows-only) Logs to the Windows Event Log.

*   **Logging Macro Variants**:
    *   **Compile-Time (`LOGGER_INFO(...)`)**: The standard and recommended macros. They provide maximum performance and safety by validating format strings at compile time.
    *   **Runtime (`LOGGER_INFO_RT(...)`)**: These macros accept a `fmt::string_view` and parse it at runtime. They are more flexible but slightly less performant and lack compile-time safety checks.
    *   **Synchronous (`LOGGER_INFO_SYNC(...)`)**: These macros bypass the queue and block the calling thread to write the log message immediately. They contend directly for the internal sink mutex, which means they can block if the asynchronous worker thread is currently writing. They are useful for critical errors or for logging just before an expected crash, but should be used sparingly to avoid performance impact.

*   **Usage**:
    The `Logger` module must be registered with the `LifecycleManager`. Configuration and logging can then be performed via the `Logger::instance()` singleton.

    ```cpp
    #include "utils/Lifecycle.hpp"
    #include "utils/Logger.hpp"

    int main() {
        // Register the Logger module with the application's lifecycle.
        pylabhub::utils::LifecycleGuard app_lifecycle(
            pylabhub::utils::Logger::GetLifecycleModule()
        );

        LOGGER_INFO("Logger started. Defaulting to Console sink.");

        // --- Switch to a rotating file sink at runtime ---
        std::error_code ec;
        const auto log_path = std::filesystem::temp_directory_path() / "my_app.log";
        const size_t max_size = 1024 * 1024; // 1 MB
        const size_t max_files = 5;

        // set_rotating_logfile is a blocking call that waits for the sink switch to complete.
        if (pylabhub::utils::Logger::instance().set_rotating_logfile(
                log_path, max_size, max_files, ec))
        {
            LOGGER_SYSTEM("Successfully switched to a rotating file sink at '{}'", log_path.string());
        }
        else
        {
            // If the switch fails (e.g., due to permissions), an error is returned.
            LOGGER_ERROR("Failed to set rotating log file: {}", ec.message());
            return 1;
        }

        for (int i = 0; i < 100; ++i) {
            LOGGER_DEBUG("Logging message #{}", i);
        }

        // --- Flush the queue ---
        // 'flush()' is a blocking call that ensures all messages currently in the
        // queue are processed and written to the active sink before it returns.
        LOGGER_INFO("Flushing log queue before exiting...");
        pylabhub::utils::Logger::instance().flush();
        LOGGER_SYSTEM_SYNC("Flush complete. All pending logs written.");

        return 0; // LifecycleGuard destructor will gracefully shut down the logger.
    }
    ```

### `FileLock` (C++ Namespace: `pylabhub::utils`)

The `FileLock` utility provides a robust, RAII-style mechanism for cross-process and cross-thread file and directory locking. It is crucial for ensuring data integrity when multiple processes or threads access shared filesystem resources like configuration files or data files.

*   **Design Principles**:
    *   **RAII-based**: The lock is acquired upon construction and automatically released upon destruction, guaranteeing resource cleanup even during exceptions.
    *   **Two-Layer Locking Model**: `FileLock` provides consistent synchronization for both:
        *   **Inter-Process**: Uses OS-level file locks (`flock` on POSIX, `LockFileEx` on Windows) on a dedicated `.lock` file to ensure exclusive access across different processes.
        *   **Intra-Process**: Employs a process-local registry (managed by a `std::mutex` and `std::condition_variable`) to serialize access between threads within the same process. This ensures consistent `Blocking` and `NonBlocking` behavior.
    *   **Advisory Lock**: It relies on cooperating processes to use the `FileLock` mechanism. It does not prevent non-cooperating processes from directly accessing the target resource.
    *   **Separate Lock File**: Instead of locking the target resource directly, a separate `.lock` file (e.g., `resource.txt.lock`) is used, simplifying implementation and avoiding interference with resource content.
    *   **Path Canonicalization**: All resource paths are resolved to a canonical form (e.g., `/path/to/file.txt` from `/path/./to/file.txt`) before generating the lock file path, ensuring different path representations contend for the same lock.

*   **Usage**:
    `FileLock` is a lifecycle-managed utility. Its module (`FileLock::GetLifecycleModule()`) must be initialized via the `LifecycleManager` before `FileLock` objects can be constructed.

    There are two primary ways to acquire a lock, each with different error-handling characteristics.

    **1. Using the `try_lock` Static Factory (Convenience)**

    The `try_lock` method is best for cases where you only need to know *if* the lock was acquired, not *why* it failed. It returns a `std::optional<FileLock>`, making it ideal for simple `if` statements.

    ```cpp
    #include "utils/Lifecycle.hpp"
    #include "utils/FileLock.hpp"
    #include "utils/Logger.hpp"

    void simple_lock_attempt(const std::filesystem::path& path) {
        // Initialization would happen in main()
        // pylabhub::utils::LifecycleGuard app_lifecycle( ... );

        if (auto lock = pylabhub::utils::FileLock::try_lock(path,
                                                          pylabhub::utils::ResourceType::File,
                                                          pylabhub::utils::LockMode::NonBlocking))
        {
            // Lock successfully acquired. `lock` is a valid optional.
            LOGGER_INFO("Lock acquired for {}", lock->get_locked_resource_path()->string());
            // ... Safely access the resource ...
        }
        else
        {
            // Lock could not be acquired. Note that the reason for failure is not
            // available in this pattern.
            LOGGER_WARN("Failed to acquire lock for {}", path.string());
        }
    }
    ```

    **2. Using the Constructor (Detailed Error Reporting)**

    ```cpp
    void detailed_lock_attempt(const std::filesystem::path& path) {
        pylabhub::utils::FileLock lock(path,
                                    pylabhub::utils::ResourceType::File,
                                    std::chrono::seconds(2));

        if (lock.valid()) {
            LOGGER_INFO("Acquired lock for resource: {}", lock.get_locked_resource_path()->string());
        } else {
            // Specific error codes to handle:
            auto ec = lock.error_code();
            if (ec == std::errc::resource_unavailable_try_again) {
                LOGGER_WARN("Lock busy: {}", path.string());
            } else if (ec == std::errc::timed_out) {
                LOGGER_ERROR("Lock timeout after 2s: {}", path.string());
            } else {
                LOGGER_ERROR("Lock error: {} - {}", path.string(), ec.message());
            }
        }
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

### `Data Exchange Hub` (C++ Namespace: `pylabhub::hub`)

The `Data Exchange Hub` is designed as a high-performance, cross-process, and cross-language communication framework based on shared memory (`DataBlock`) and a central message broker (`MessageHub`). It aims to provide efficient data streaming and coordination between independent processes.

**Design Principles:**

*   **Shared Memory Centric**: Primary data transfer occurs via `DataBlock` shared memory segments, minimizing data copying and maximizing throughput.
*   **Broker-Mediated Coordination**: `MessageHub` facilitates discovery, registration, and signaling between `DataBlock` producers and consumers, acting as a control plane.
*   **Two-Tiered Synchronization**: A sophisticated locking mechanism ensures both robust internal management and efficient user-facing data access.
*   **Fixed and Flexible Memory Areas**: Each `DataBlock` is structured to provide predictable control regions and adaptable data buffering zones.

#### Memory Model (`DataBlock` Structure)

Each `DataBlock` shared memory segment is organized into distinct, fixed-size, and flexible areas to ensure efficient and predictable access:

1.  **`SharedMemoryHeader` (Fixed Area - Control Plane)**:
    *   Located at the very beginning of the shared memory segment.
    *   Contains essential metadata for identification, security, and synchronization:
        *   `magic_number`, `version`, `shared_secret`: For validation and compatibility.
        *   `active_consumer_count`: Atomic counter for tracking active consumers.
        *   `write_index`, `commit_index`, `read_index`, `current_slot_id`: Atomic indices for managing data flow within flexible buffers (e.g., ring buffers).
        *   **Internal Management Mutex (`management_mutex_storage`)**: A single, OS-specific, robust mutex (PTHREAD_PROCESS_SHARED on POSIX, named kernel mutex on Windows) dedicated to protecting critical operations on the `SharedMemoryHeader` itself, especially the allocation/deallocation of user-facing spinlocks.
        *   **User-facing Spinlock Pool (`shared_spinlocks`)**: An array of `SharedSpinLockState` structs (up to `MAX_SHARED_SPINLOCKS`) used by application logic to coordinate access to data within the flexible areas. These are simple, atomic-based spin-locks implemented directly within the shared memory.
        *   **Spinlock Allocation Map (`spinlock_allocated`)**: An array of `std::atomic_flag`s to track which `SharedSpinLockState` units are currently allocated or free. This map is protected by the Internal Management Mutex.

2.  **Flexible Data Zone**:
    *   Immediately follows the `SharedMemoryHeader`.
    *   A variable-sized region for flexible data storage, typically used for variable-length messages or metadata. Its size is configurable during `DataBlock` creation.

3.  **Structured Data Buffer Area**:
    *   Follows the Flexible Data Zone.
    *   A variable-sized region for structured data buffering, intended for high-throughput data streams, often implemented as queues or ring-buffers. Its size is configurable. This area will expose mechanisms for predefined structures such as queues, and ring buffers.

#### Mutex Management (`DataBlock` Synchronization)

The `Data Exchange Hub` employs a two-tiered synchronization strategy tailored for its shared memory model:

1.  **Internal Management Mutex (`DataBlockMutex`)**:
    *   **Purpose**: Protects access to the `SharedMemoryHeader`'s metadata, particularly the `spinlock_allocated` map and the initialization/destruction of `SharedSpinLockState` units.
    *   **Implementation**:
        *   **POSIX**: Uses `pthread_mutex_t` initialized with `PTHREAD_PROCESS_SHARED` and `PTHREAD_MUTEX_ROBUST` attributes, residing directly within the `management_mutex_storage` in the `SharedMemoryHeader`. Access is managed via dynamic address calculation from the current process's mapped shared memory base address.
        *   **Windows**: Uses a named kernel mutex (`CreateMutex`/`OpenMutex`), with the name derived from the `DataBlock`'s unique identifier. It is a robust, OS-managed object.
    *   **Usage**: Accessed internally by `DataBlock` helper functions (e.g., `acquire_shared_spinlock`, `release_shared_spinlock`) via RAII `DataBlockLockGuard`.

2.  **User-Facing Spinlock Pool (`SharedSpinLock`)**:
    *   **Purpose**: Provides simple, efficient, cross-process synchronization for application-level data access within the `DataBlock`'s flexible memory areas.
    *   **Implementation**: An array of `SharedMemoryHeader::SharedSpinLockState` structs directly embedded in the `SharedMemoryHeader`. Each `SharedSpinLockState` uses atomic variables (`owner_pid`, `generation`, `recursion_count`, `owner_thread_id`) to manage lock ownership.
    *   **Robustness**: Features dead PID detection and generation counters to handle process crashes and PID reuse, ensuring the spin-lock can be reclaimed if the owning process dies. Supports recursive locking by the same thread.
    *   **Usage**: Acquired by `IDataBlockProducer` methods (returning a `SharedSpinLockGuard`) and accessed by `IDataBlockConsumer` methods (returning a `SharedSpinLock` object). These provide a fixed, interpretable memory footprint for simple coordination.

#### Components

-   **`MessageHub`**: Provides encrypted communication with a remote broker using ZeroMQ and CurveZMQ. This serves as the control plane for discovery and registration of `DataBlock` channels.
-   **`DataBlock`**: The core shared memory segment manager. Exposes factory functions `create_datablock_producer()` and `find_datablock_consumer()` to obtain interfaces for interacting with the shared memory.

**Missing Components (Current Development Focus)**:

-   Actual producer/consumer read/write APIs (`IDataBlockProducer`/`IDataBlockConsumer` methods).
-   Broker registration protocol (how `DataBlock` channels are advertised and discovered via `MessageHub`).
-   Slot management implementations for structured data buffers (e.g., queues, ring buffers) using the `write_index`, `commit_index`, `read_index`, and `current_slot_id` in the `SharedMemoryHeader` and leveraging `SharedSpinLock` for fine-grained access.

---

## Specialized and Header-Only Utilities

This section covers foundational, header-focused utilities that are broadly used across the entire codebase. They have minimal dependencies and are primarily located in `src/include/`.

### Header-Only Concurrency Guards (C++ Namespace: `pylabhub::basics`)

The library includes several header-only, RAII-style guards for managing concurrency and scope.

#### `ScopeGuard`

`ScopeGuard` is a general-purpose RAII guard that robustly executes a callable object (like a lambda) upon scope exit. It is a critical tool for ensuring that cleanup actions are always performed, even when functions exit early due to exceptions. It enforces unique ownership of the cleanup action as it is movable but not copyable.

*   **Design and Features**:
    *   The guard holds a callable and an `active` flag. The callable is executed by the destructor only if the guard is active.
    *   It is best created with the `make_scope_guard` factory function, which simplifies template deduction.
    *   The guard's state can be checked via an `explicit operator bool()`. This allows for idioms like `if (guard) { ... }`.
    *   The cleanup action can be cancelled by calling `dismiss()` or its alias `release()`. This is useful when resource ownership is successfully transferred.

*   **Correct Usage**:
    `ScopeGuard` is ideal for managing raw resources that lack their own RAII wrappers, such as C-style file handles, raw pointers, or complex application state.

    ```cpp
    #include "utils/scope_guard.hpp"

    void process_resource() {
        ResourceType* res = create_resource();

        // The guard ensures the resource is destroyed even if an error occurs later.
        auto guard = pylabhub::basics::make_scope_guard([&res]() {
            if (res) {
                destroy_resource(res);
                res = nullptr;
            }
        });

        // If any of these operations throw an exception, the guard guarantees
        // that destroy_resource() is still called during stack unwinding.
        res->do_something();
        res->do_another_thing();

        // If processing is successful, we can dismiss the guard if we are
        // intentionally transferring ownership of the resource.
        // guard.dismiss();
    } // `guard` executes here if not dismissed, cleaning up the resource.
    ```

*   **Exception Handling**:
    *   To prevent program termination (`std::terminate`), the `ScopeGuard` destructor is `noexcept` and will **catch and swallow** any exceptions thrown by its callable. This is a safe default but can mask issues in cleanup logic.
    *   **Best Practice**: Ensure cleanup logic is simple and non-throwing. If failure is possible within the cleanup action, handle it inside the callable (e.g., by logging an error) rather than letting an exception escape.
    *   If you need to manually trigger the guard and handle potential exceptions, use the `invoke_and_rethrow()` method, which is *not* `noexcept` and will propagate any exceptions.

*   **Bad Practices and Pitfalls**:
    *   **Dangling References**: Be extremely careful with lambda captures. Capturing a local variable by reference (`[&]`) that is destroyed before the guard executes will result in undefined behavior.
        ```cpp
        auto make_bad_guard() {
            int local_var = 42;
            // BAD: `local_var` will be destroyed when this function returns.
            return pylabhub::basics::make_scope_guard([&]() {
                // Accessing `local_var` here is a use-after-free error.
                std::cout << local_var;
            });
        } // Undefined Behavior when the returned guard is destructed!
        ```
    *   **Premature Dismissal**: Only call `dismiss()` when you are intentionally canceling the cleanup action, for example after successfully transferring ownership of a resource.
    *   **Misunderstanding Move Semantics**: When a `ScopeGuard` is moved, the source guard becomes inactive, and the moved-to guard takes over responsibility. The destructor of a moved-from guard has no effect.

#### `AtomicGuard`

A fast, stateless, spinlock-like guard for protecting critical sections in high-contention, multi-threaded scenarios. It operates on an `AtomicOwner` object, which is a wrapper around a `std::atomic_flag`.

*   **Usage**: `AtomicOwner owner; ... AtomicGuard guard(&owner, /*tryAcquire=*/true); if (guard.active()) { ... }`

#### `RecursionGuard`

A thread-local, RAII-based guard to detect and prevent unwanted re-entrant function calls on a per-object basis. It works by pushing a unique key (typically a pointer to an object instance) onto a thread-local stack upon construction and popping it upon destruction.

*   **Key Features & Semantics**:
    *   **Thread-Local**: Each thread maintains its own independent recursion stack, initialized with a reserved capacity (e.g., 16 elements) to optimize for common recursion depths. A guard in one thread has no effect on another, preventing interference.
    *   **Object-Specific**: The guard is associated with a specific key (e.g., a `this` pointer), allowing it to track recursion on a per-instance basis.
    *   **RAII**: Guarantees that the key is popped from the stack upon scope exit, even if exceptions occur. The destructor handles both LIFO and non-LIFO destruction orders for robustness.
    *   **Non-LIFO Support**: Correctly handles cases where guards are destroyed out of order (e.g., when managed by `std::unique_ptr`).
    *   **Movable**: Supports move construction, allowing it to be created and returned from factory functions. It is **not** copyable or move-assignable.
    *   **Efficient Checks**: `is_recursing()` uses a fast-path check for the most recent entry on the stack before performing a full scan to determine if a key is already present.

*   **Primary Use Case**: Preventing stack overflows from infinite recursion in complex call chains. This is especially useful in callback-driven systems or when processing graph-like data structures where call cycles can occur.

    ```cpp
    #include "plh_base.hpp" // Provides RecursionGuard (via pylabhub::basics)

    class Node {
    public:
        void process() {
            // Prevent infinite loops if nodes form a cycle (e.g., A->B->A)
            // `is_recursing` efficiently checks if this object is already on the current thread's stack.
            if (pylabhub::basics::RecursionGuard::is_recursing(this)) {
                // The current object is already being processed on this thread's stack.
                // Log an error, return, or handle the re-entrancy appropriately.
                return;
            }
            // The guard is active for the remainder of this scope, adding `this` to the stack.
            pylabhub::basics::RecursionGuard guard(this);

            // ... process this node and potentially call process() on neighbors ...
        }
    };
    ```

*   **Notes on Usage and Potential Issues**:
    *   **`std::bad_alloc` Exception**: The `RecursionGuard` constructor may throw `std::bad_alloc`. This can happen during the initial allocation of the thread-local stack (via `get_recursion_stack()::reserve(16)`) or if the recursion depth exceeds the current capacity and reallocation fails. Callers in memory-critical contexts should be prepared to handle this exception.
    *   **Pointer Lifetime (Dangling Pointers)**: The `key` pointer passed to the guard's constructor **must** remain valid for the entire lifetime of the `RecursionGuard` instance. Passing a pointer to a local variable that goes out of scope before the guard is a serious bug that will lead to undefined behavior. Using `this` or pointers to heap-allocated objects (whose lifetime is managed) is generally safe.
    *   **Not for Cross-Thread Protection**: This guard does **not** prevent race conditions. It only detects recursion *within a single thread*. For protecting shared data from simultaneous access by multiple threads, use `std::mutex`, `AtomicGuard`, or other suitable synchronization primitives.

## Platform-Specific Behaviors and Limitations

### FileLock
- **POSIX**: Uses `flock()` for advisory locks. Other processes can ignore these locks if not cooperating.
- **Windows**: Uses `LockFileEx` for mandatory locks with retry logic for sharing violations.
- **Edge Case**: On Windows, antivirus software may cause temporary `ERROR_SHARING_VIOLATION` during file access.

### JsonConfig - Atomic Writes
- **POSIX**: Uses `mkstemp` + `rename`. Atomic on local filesystems, may not be atomic on NFS. Now includes retry logic for transient errors (`EBUSY`, `ETXTBSY`, `EINTR`).
- **Windows**: Uses `ReplaceFileW` with 5 retry attempts. May fail if file is held open by another process for >500ms. Now logs warnings for `ERROR_SHARING_VIOLATION` retries.

### Logger - Event Logging
- **Windows**: Requires registry configuration for Event Log source name.
- **POSIX**: Syslog requires daemon to be running; may silently drop messages if daemon is unavailable.

### Debug Utilities (C++ Namespace: `pylabhub::debug`)

These utilities provide cross-platform support for debugging and handling fatal errors.

*   `print_stack_trace(bool use_external_tools = false)`: Prints the current call stack to `stderr`. On POSIX systems, setting `use_external_tools` to `true` will attempt to use tools like `addr2line` or `atos` for more detailed line number information, though this is slower and carries a minor risk of hanging in some contexts. The parameter has no effect on Windows.
    *   **WARNING**: This function is **NOT** async-signal-safe and is not guaranteed to be thread-safe for concurrent calls. Do not call it from a signal handler.
*   `panic()` / `PLH_PANIC` macro: Handles fatal, unrecoverable errors by printing a message and stack trace, then aborting the program. Features compile-time format string checks.
*   `debug_msg()` / `PLH_DEBUG` macro: Prints debug messages (if `PYLABHUB_ENABLE_DEBUG_MESSAGES` is defined) with compile-time format string checks and automatic source location reporting.
*   `debug_msg_rt()` / `PLH_DEBUG_RT` macro: Similar to `debug_msg()`, but accepts a runtime format string (e.g., `std::string_view`).

### Platform Abstractions (C++ Namespace: `pylabhub::platform`)

This namespace contains functions that abstract away platform-specific OS calls, allowing for more portable code.

*   `get_pid()`: Returns the current process ID.
*   `get_native_thread_id()`: Returns the native, OS-specific ID for the current thread.
*   `get_executable_name(bool include_path = false)`: Returns the filename or full path of the current application executable.

### Formatting Tools (C++ Namespace: `pylabhub::format_tools`)

This is a collection of utilities and template specializations that integrate with the `{fmt}` library to provide custom formatting for project-specific types, such as converting `std::filesystem::path` to a string.

---

## Advanced Topics and Implementation Details

### Logger: Detailed Architecture and Queue Management

#### Queue Semantics and Overflow Behavior

The Logger uses a **tiered dropping strategy** to balance performance with reliability:

**Soft Limit (`m_max_queue_size`)**: When the queue reaches this limit, **only LogMessage commands** are dropped. Control commands (SetSinkCommand, FlushCommand) continue to be queued. This ensures that administrative operations like log rotation can complete even under load.

**Hard Limit (`2 * m_max_queue_size`)**: When the queue exceeds this threshold, **all commands** including control operations are dropped. This is a last-resort safety mechanism to prevent unbounded memory growth.

**Drop Recovery**: When dropping ends (queue drains below soft limit), the Logger automatically enqueues a warning message reporting how many messages were dropped and for how long.

```cpp
// Example: Configuring queue size
Logger::instance().set_max_queue_size(5000);
// Soft limit: 5000 messages
// Hard limit: 10000 messages (automatic)

size_t current_max = Logger::instance().get_max_queue_size();    // Returns 5000
size_t dropped = Logger::instance().get_dropped_message_count(); // Returns total drops
```

**Best Practices**:
- Set `m_max_queue_size` based on your application's peak logging rate × acceptable buffering time
- Monitor `get_dropped_message_count()` in production to detect undersized queues
- For burst-heavy applications, increase queue size rather than decreasing log level

#### Synchronous Logging: When and How

The `LOGGER_*_SYNC` macros bypass the queue and write directly to the sink, acquiring the `m_sink_mutex`. This provides two guarantees:
1. Message is written immediately (not deferred)
2. Message order is preserved relative to async messages enqueued before the sync call

**When to Use Synchronous Logging**:
- Immediately before `std::abort()` or other termination
- In signal handlers (with extreme caution - see warnings)
- When debugging timing-sensitive race conditions
- For critical audit events that must not be dropped

**Performance Characteristics**:
```cpp
// Async logging (typical case)
LOGGER_INFO("User {} logged in", user_id);  
// Time: ~100ns (format + enqueue only)

// Synchronous logging
LOGGER_INFO_SYNC("Critical error, about to abort!");
// Time: ~1-10ms (includes disk I/O or console write)
```

**Warning**: Overuse of synchronous logging defeats the purpose of the async architecture and can cause severe performance degradation.

#### Sink Switching and Lifecycle

When you call a sink-change method like `set_rotating_logfile()`, the command is enqueued and executed by the worker thread. The calling thread **blocks** until the sink switch completes. This provides synchronous error reporting:

```cpp
std::error_code ec;
if (!Logger::instance().set_rotating_logfile(path, max_size, max_files, ec)) {
    // ec contains the specific error (e.g., permission denied)
    LOGGER_ERROR("Failed to switch to rotating log: {}", ec.message());
}
// At this point, if the call succeeded, the new sink is active
```

**Sink Transition Messages**: The Logger automatically logs "Switching log sink to: [description]" in the *old* sink and "Log sink switched from: [old]" in the *new* sink. These can be disabled with `set_log_sink_messages_enabled(false)`.

---

### FileLock: Implementation Deep Dive

#### Lock File Naming and Canonicalization

`FileLock` generates lock file names using the following algorithm:

1. **Canonicalization**: Convert the target path to canonical form using `std::filesystem::canonical()`. If the file doesn't exist yet, fall back to `std::filesystem::absolute().lexically_normal()`.
2. **Lock File Generation**:
   - For files: Append `.lock` to the full path (e.g., `/data/config.json.lock`)
   - For directories: Append `.dir.lock` to the directory name (e.g., `/data/.dir.lock`)
3. **Special Case**: If the directory name is empty, `.`, or `..`, use `pylabhub_root.dir.lock` instead

**Example Transformations**:
```cpp
FileLock::get_expected_lock_fullname_for("/data/./config.json", ResourceType::File);
// Returns: /data/config.json.lock

FileLock::get_expected_lock_fullname_for("/data/../data/dir/", ResourceType::Directory);
// Returns: /data/dir/dir.dir.lock

FileLock::get_expected_lock_fullname_for("C:\\", ResourceType::Directory);
// Returns: C:\pylabhub_root.dir.lock
```

#### Intra-Process Lock Registry

The process-local registry (`g_proc_locks`) ensures that multiple threads within the same process serialize their lock attempts. Here's how it works:

```cpp
struct ProcLockState {
    int owners = 0;            // Number of threads holding the lock
    int waiters = 0;           // Number of threads waiting for the lock
    std::condition_variable cv; // For blocking wait
};
```

When a thread attempts to acquire a lock:
1. It looks up the canonical lock file path in `g_proc_locks`
2. If `owners > 0`, another thread holds it:
   - NonBlocking: Fail immediately
   - Blocking: Wait on the condition variable
3. When the lock is available, increment `owners` and proceed to OS-level lock

This mechanism prevents the same process from deadlocking itself (which is allowed by some OS file locking APIs).

#### POSIX vs Windows: Behavioral Differences

| Aspect | POSIX (`flock`) | Windows (`LockFileEx`) |
|--------|-----------------|------------------------|
| **Lock Type** | Advisory | Mandatory (with sharing) |
| **Lock Granularity** | Whole file only | Byte-range (we use whole file) |
| **Inheritance** | Not inherited by child processes | Can be inherited |
| **NFS Behavior** | Inconsistent across implementations | N/A (Windows doesn't use NFS) |
| **Upgrade/Downgrade** | Not supported | Not supported by FileLock |
| **Retry on Sharing Violation** | N/A | Yes (5 retries, 100ms apart) |

**Portability Notes**:
- Code using `FileLock` will have consistent behavior across platforms
- Advisory vs mandatory lock distinction is abstracted away (all cooperating processes must use FileLock)
- NFS: Use local filesystem for critical locks

---

### JsonConfig: Transaction Model and Atomicity

#### The Transaction Proxy Pattern

`JsonConfig` uses a clever rvalue-proxy pattern to enforce proper transaction usage at compile time:

```cpp
// This compiles - proxy is consumed immediately
config.transaction(AccessFlags::ReloadFirst).read([](const auto& j) {
    return j.value("key", "default");
});

// This does NOT compile - proxy cannot be stored
auto tx = config.transaction();  // ERROR: rvalue proxy cannot be assigned
tx.read([](const auto& j) { ... });
```

The `&&`-qualified methods on `TransactionProxy` ensure it can only be called on temporaries, preventing:
- Accidentally holding a transaction open for too long
- Forgetting to call `read()` or `write()`
- Resource leaks from uncompleted transactions

#### Access Flags in Detail

**`Default` / `UnSynced`**: Operates on the in-memory cache only. Fastest option. Use when:
- You've already reloaded manually
- You're the only process accessing the file
- You'll commit manually later

**`ReloadFirst`**: Acquires a `FileLock`, reads from disk, then releases the lock before executing your lambda. Use when:
- You need fresh data from disk
- Another process may have modified the file
- You want to read atomically with respect to other processes

**`CommitAfter`**: After your write lambda returns `CommitDecision::Commit`, acquires a `FileLock` and atomically writes to disk. Use when:
- Changes must be immediately persisted
- You want atomic write-after-modify

**`FullSync` (ReloadFirst | CommitAfter)**: Provides a complete atomic read-modify-write cycle. This is the slowest but safest option. Use when:
- Multiple processes are actively modifying the config
- Data integrity is critical
- Performance is not the primary concern

**Example: Conditional Commit**
```cpp
config.transaction(AccessFlags::CommitAfter).write([](nlohmann::json& j) {
    int counter = j.value("counter", 0);
    if (counter < 100) {
        j["counter"] = counter + 1;
        return CommitDecision::Commit;  // Persist the change
    }
    return CommitDecision::SkipCommit;  // Don't write to disk
});
```

#### Atomic Write Algorithm

JsonConfig uses a crash-safe atomic write algorithm:

**POSIX**:
1. Create temporary file with `mkstemp()` in same directory as target
2. Write JSON to temp file
3. `fsync()` the temp file
4. `rename()` temp file to target (atomic operation)
5. `fsync()` the parent directory (ensures directory entry is persisted)

**Windows**:
1. Create temporary file with unique name in same directory
2. Write JSON to temp file
3. `FlushFileBuffers()` (fsync equivalent)
4. `ReplaceFileW()` to atomically swap files (retries on `ERROR_SHARING_VIOLATION`)
5. Fallback to `MoveFileExW()` if target doesn't exist

**Crash Safety**: If the process crashes at any point before step 4/5, the original file is untouched. The temporary file is orphaned but can be cleaned up.

---

### Lifecycle: Reference Counting and Dependency Management

#### How Dynamic Module Reference Counting Works

The `LifecycleManager` maintains a reference count (`ref_count`) for each non-persistent dynamic module. This count represents how many other **currently loaded** dynamic modules depend on it.

**Recalculation Algorithm** (called after every load/unload operation):
```
1. Set all dynamic module ref_counts to 0
2. For each LOADED dynamic module M:
   3. For each dependency D of M:
      4. If D is a non-persistent dynamic module:
         5. Increment D.ref_count
```

**Example Scenario**:
```
Modules:
- Static: Logger (always loaded)
- Dynamic: PluginA (depends on Logger, PluginB)
- Dynamic: PluginB (depends on Logger)
- Dynamic: PluginC (depends on PluginB)

Initial state: All dynamic modules UNLOADED

After LoadModule("PluginA"):
- PluginA: LOADED, ref_count=0
- PluginB: LOADED (auto-loaded), ref_count=1 (referenced by PluginA)
- PluginC: UNLOADED

After LoadModule("PluginC"):
- PluginA: LOADED, ref_count=0
- PluginB: LOADED, ref_count=2 (referenced by PluginA and PluginC)
- PluginC: LOADED, ref_count=0

Attempt UnloadModule("PluginB"):
- Fails! ref_count=2, still referenced by PluginA and PluginC
- Must unload PluginA and PluginC first

After UnloadModule("PluginA"):
- PluginA: UNLOADED, removed from graph
- PluginB: ref_count=1 (now only referenced by PluginC)
- PluginC: LOADED, ref_count=0

After UnloadModule("PluginC"):
- PluginC: UNLOADED, removed from graph
- PluginB: ref_count=0 (no longer referenced)
- PluginB: UNLOADED automatically (cascade unload)
```

**Key Insight**: The reference counting ensures a dependency is never unloaded while modules depending on it are still loaded.

#### Persistent Modules

Marking a module as persistent (`set_as_persistent(true)`) has two effects:
1. The module cannot be explicitly unloaded via `UnloadModule()`
2. The module is excluded from reference counting (it doesn't prevent its dependents from unloading)

Use persistent modules for:
- Plugins that should remain active once loaded
- Services that are expensive to restart
- Components with external state that shouldn't be reset

#### Topological Sort and Dependency Resolution

The `LifecycleManager` uses **Kahn's algorithm** for topological sorting:

```
Input: Set of modules with dependencies
Output: Linear ordering such that dependencies come before dependents

Algorithm:
1. Calculate in-degree (number of dependencies) for each module
2. Add all modules with in-degree 0 to a queue
3. While queue is not empty:
   a. Remove module from queue, add to sorted list
   b. For each dependent of this module:
      - Decrement its in-degree
      - If in-degree becomes 0, add to queue
4. If sorted list size < total modules:
   - Circular dependency detected!
   - Report cycle and abort
```

**Circular Dependency Detection**: The algorithm detects cycles by checking if all modules were processed. If not, the remaining modules form a cycle. The error message reports all modules involved in the cycle.

---

## Advanced Usage Patterns

### Multi-Process Configuration Management with JsonConfig

When multiple processes need to coordinate via a shared JSON config:

```cpp
class ConfigCoordinator {
    pylabhub::utils::JsonConfig config_;
    
public:
    ConfigCoordinator(const std::filesystem::path& config_path) {
        std::error_code ec;
        if (!config_.init(config_path, true, &ec)) {
            throw std::runtime_error("Config init failed: " + ec.message());
        }
    }
    
    // Acquire a distributed lock on a named resource
    bool try_acquire_lock(const std::string& lock_name, int process_id) {
        return config_.transaction(AccessFlags::FullSync).write([&](nlohmann::json& j) {
            auto& locks = j["distributed_locks"];
            if (locks.contains(lock_name)) {
                int current_owner = locks[lock_name]["owner"];
                if (current_owner != process_id) {
                    return CommitDecision::SkipCommit;  // Lock held by another process
                }
            }
            locks[lock_name]["owner"] = process_id;
            locks[lock_name]["timestamp"] = std::time(nullptr);
            return CommitDecision::Commit;
        });
    }
    
    bool release_lock(const std::string& lock_name, int process_id) {
        return config_.transaction(AccessFlags::FullSync).write([&](nlohmann::json& j) {
            auto& locks = j["distributed_locks"];
            if (!locks.contains(lock_name)) {
                return CommitDecision::SkipCommit;  // Lock doesn't exist
            }
            int current_owner = locks[lock_name]["owner"];
            if (current_owner != process_id) {
                return CommitDecision::SkipCommit;  // Not our lock
            }
            locks.erase(lock_name);
            return CommitDecision::Commit;
        });
    }
};
```

### Structured Logging with Context

Build a logging wrapper that automatically includes context:

```cpp
class ContextualLogger {
    std::string context_;
    
public:
    explicit ContextualLogger(std::string ctx) : context_(std::move(ctx)) {}
    
    template<typename... Args>
    void info(fmt::format_string<Args...> fmt, Args&&... args) {
        LOGGER_INFO("[{}] {}", context_, fmt::format(fmt, std::forward<Args>(args)...));
    }
    
    template<typename... Args>
    void error(fmt::format_string<Args...> fmt, Args&&... args) {
        LOGGER_ERROR("[{}] {}", context_, fmt::format(fmt, std::forward<Args>(args)...));
    }
    
    // Create child logger with extended context
    ContextualLogger child(const std::string& sub_context) const {
        return ContextualLogger(context_ + "::" + sub_context);
    }
};

// Usage
void process_user_request(int user_id) {
    ContextualLogger log(fmt::format("User{}", user_id));
    log.info("Processing request");
    
    try {
        validate_user(user_id);
        ContextualLogger db_log = log.child("Database");
        db_log.info("Fetching user data");
        // ...
    } catch (const std::exception& e) {
        log.error("Request failed: {}", e.what());
    }
}
```

### Dynamic Plugin System with Hot-Reload

Implement a plugin system that can reload plugins without restarting:

```cpp
class PluginManager {
    struct PluginInfo {
        std::string name;
        std::filesystem::path lib_path;
        std::time_t last_modified;
    };
    
    std::vector<PluginInfo> plugins_;
    
public:
    void watch_and_reload() {
        for (auto& plugin : plugins_) {
            auto current_mod_time = std::filesystem::last_write_time(plugin.lib_path);
            auto current_time_t = std::chrono::system_clock::to_time_t(
                std::chrono::file_clock::to_sys(current_mod_time));
            
            if (current_time_t > plugin.last_modified) {
                LOGGER_INFO("Plugin {} changed, reloading", plugin.name);
                
                // Unload old version
                if (!pylabhub::utils::UnloadModule(plugin.name)) {
                    LOGGER_ERROR("Failed to unload plugin {}", plugin.name);
                    continue;
                }
                
                // Re-register and load new version
                // (In real implementation, you'd dlopen the new .so/.dll)
                auto new_module_def = load_plugin_from_file(plugin.lib_path);
                if (pylabhub::utils::RegisterDynamicModule(std::move(new_module_def))) {
                    if (pylabhub::utils::LoadModule(plugin.name)) {
                        plugin.last_modified = current_time_t;
                        LOGGER_INFO("Plugin {} reloaded successfully", plugin.name);
                    }
                }
            }
        }
    }
};
```

---

## Performance Characteristics

### Logger

| Operation | Latency (typical) | Notes |
|-----------|-------------------|-------|
| `LOGGER_INFO()` (async) | 50-200 ns | Format + enqueue only |
| `LOGGER_INFO_SYNC()` | 1-10 ms | Includes I/O wait |
| `flush()` | 10-500 ms | Waits for queue to drain |
| `set_rotating_logfile()` | 10-100 ms | Synchronous, includes validation |

**Scalability**: Logger handles up to ~10M messages/sec on modern hardware with default queue size (10,000).

### FileLock

| Operation | Latency (typical) | Notes |
|-----------|-------------------|-------|
| Lock acquisition (no contention) | 100-500 µs | File creation + flock/LockFileEx |
| Lock acquisition (contention, POSIX) | 20ms average | Polling interval |
| Lock acquisition (contention, Windows) | Immediate | OS-level wait queue |
| Lock release | 10-50 µs | Close FD + map erase |

**Best Practice**: For high-frequency locking (>100 Hz), consider alternative synchronization (shared memory + atomics).

### JsonConfig

| Operation | Latency | Notes |
|-----------|---------|-------|
| `transaction().read()` | 1-10 µs | In-memory, shared lock |
| `transaction(ReloadFirst).read()` | 1-10 ms | File I/O + JSON parse |
| `transaction(CommitAfter).write()` | 5-50 ms | JSON serialize + atomic write |
| `transaction(FullSync).write()` | 10-100 ms | Reload + modify + commit |

**Optimization**: For frequent reads, keep a long-lived `ReadLock` if your data is stable.

---

## Known Issues and Limitations

1. **Lifecycle Dynamic Module Unloading**: Reference counting does not account for static dependencies holding references to dynamic modules. A static module using a dynamic module will prevent unloading.

2. **Logger Queue Overflow**: Unbounded memory growth is prevented by a tiered dropping strategy. Log messages are dropped if the queue exceeds `m_max_queue_size`. If the queue reaches `2 * m_max_queue_size`, *queued control commands* (e.g., `SetSinkCommand`, explicit `FlushCommand`s) are also dropped to prevent memory exhaustion. The logger's main shutdown mechanism is handled out-of-band and is not subject to queue dropping, and it always performs a final flush of remaining messages.

3. **DataBlock Windows Support**: Incomplete - mutex synchronization not implemented for Windows shared memory, currently only zero-initialized as a placeholder. This is a **known critical limitation**. Do not use DataBlock in production on Windows.

4. **FileLock Performance**: On POSIX systems with high contention, polling interval is fixed at 20ms. This may cause delays in lock acquisition. For high-frequency locking scenarios, consider application-level optimizations or alternative synchronization primitives.

5. **RecursionGuard Memory**: First call on each thread allocates thread-local storage via `reserve(16)`. This allocation cannot be freed until thread termination. In thread-pool scenarios, this may accumulate memory over time.

6. **JsonConfig on NFS**: The atomic write guarantee (rename operation) may not hold on some NFS configurations. For critical configuration files, use local filesystem.

7. **Logger Message Ordering**: While messages from a single thread are strictly ordered, relative ordering between threads is based on queue enqueue time. High contention may cause reordering.

---

## Troubleshooting Guide

### Logger Issues

**Problem**: "Logger method called before initialization"
- **Cause**: You called a configuration method (e.g., `set_level()`) before registering the Logger module with `LifecycleManager`.
- **Solution**: Ensure `Logger::GetLifecycleModule()` is included in your `LifecycleGuard` constructor.

**Problem**: Messages are being dropped
- **Diagnosis**: Call `Logger::instance().get_dropped_message_count()` to check.
- **Solutions**:
  1. Increase queue size with `set_max_queue_size()`
  2. Reduce logging volume (filter at source)
  3. Switch to a faster sink (console → file, or disable flock)

**Problem**: Application hangs on shutdown
- **Cause**: Worker thread is stuck in a sink's `write()` or `flush()` method.
- **Solution**: Check if you're using a custom sink implementation with blocking I/O. Reduce shutdown timeout in `ModuleDef` if needed.

### FileLock Issues

**Problem**: Lock acquisition always times out
- **Diagnosis**: Check `error_code()` - if it's `resource_unavailable_try_again`, another process holds the lock.
- **Solutions**:
  1. Verify lock file path with `get_expected_lock_fullname_for()`
  2. Check for stale lock files (orphaned from crashes)
  3. Call `FileLock::cleanup()` manually if `cleanup_on_shutdown` was disabled

**Problem**: Different processes aren't seeing the same lock
- **Cause**: Path canonicalization might be producing different lock file names due to symlinks or relative paths.
- **Diagnosis**: Print the result of `get_canonical_lock_file_path()` from both processes.
- **Solution**: Always use absolute paths or ensure all processes resolve symlinks the same way.

### JsonConfig Issues

**Problem**: Changes not visible to other processes
- **Cause**: You're not using `CommitAfter` or manually calling `overwrite()`.
- **Solution**: Use `transaction(AccessFlags::CommitAfter).write(...)` or call `overwrite()` after modifications.

**Problem**: "resource_deadlock_would_occur" error
- **Cause**: You're trying to acquire a nested transaction on the same `JsonConfig` object.
- **Solution**: Complete the first transaction before starting a second one.

**Problem**: File corruption after crash
- **Cause**: This shouldn't happen - atomic writes prevent corruption. If it does:
  - Check filesystem type (e.g., NFS might not support atomic rename)
  - Verify no other code is directly writing to the file without `JsonConfig`
  
### Lifecycle Issues

**Problem**: "Circular dependency detected"
- **Cause**: Module A depends on B, which depends on A (directly or indirectly).
- **Solution**: Refactor modules to break the cycle. Often this means extracting a third module containing shared functionality.

**Problem**: Dynamic module won't unload
- **Diagnosis**: Check if `UnloadModule()` returns `false`. If so, another dynamic module still depends on it.
- **Solution**: Call `UnloadModule()` on dependents first, or make the module persistent if it should remain loaded.

---

## Migration Guide

### From Manual Initialization to Lifecycle-Managed

**Old code (manual initialization)**:
```cpp
int main() {
    initialize_logger();  // Hypothetical old function
    
    // Application logic
    log_message("Hello");
    
    cleanup_logger();
    return 0;
}
```

**New code (lifecycle-managed)**:
```cpp
int main() {
    pylabhub::utils::LifecycleGuard lifecycle(
        pylabhub::utils::Logger::GetLifecycleModule()
    );
    
    // Application logic
    LOGGER_INFO("Hello");
    
    // Automatic cleanup when lifecycle goes out of scope
    return 0;
}
```

### From Direct File Locking to FileLock

**Old code (POSIX-specific)**:
```cpp
int fd = open(file_path, O_RDWR);
if (flock(fd, LOCK_EX) == 0) {
    // Critical section
    flock(fd, LOCK_UN);
}
close(fd);
```

**New code (cross-platform)**:
```cpp
if (auto lock = FileLock::try_lock(file_path, ResourceType::File)) {
    // Critical section
    // Automatic unlock when lock goes out of scope
}
```

---

## API Reference Quick Links

For detailed API documentation, see the header files:

- **Lifecycle**: `cpp/src/include/utils/Lifecycle.hpp`
- **Logger**: `cpp/src/include/utils/Logger.hpp`
- **FileLock**: `cpp/src/include/utils/FileLock.hpp`
- **JsonConfig**: `cpp/src/include/utils/JsonConfig.hpp`
- **ScopeGuard**: `cpp/src/include/utils/scope_guard.hpp`
- **RecursionGuard**: `cpp/src/include/utils/recursion_guard.hpp`
- **AtomicGuard**: `cpp/src/include/utils/atomic_guard.hpp`

Each header contains comprehensive inline documentation with usage examples and design rationale.
