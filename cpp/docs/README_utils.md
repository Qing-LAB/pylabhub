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
    *   **Synchronous (`LOGGER_INFO_SYNC(...)`)**: These macros bypass the queue and block the calling thread to write the log message immediately. They are useful for critical errors or for logging just before an expected crash, but should be used sparingly to avoid performance impact.

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

    To get detailed error information on a lock failure, you must use the constructor directly and then check the object's state with `valid()` and `error_code()`.

    ```cpp
    #include "utils/Lifecycle.hpp"
    #include "utils/FileLock.hpp"
    #include "utils/Logger.hpp"

    void detailed_lock_attempt(const std::filesystem::path& path) {
        // Initialization would happen in main()
        // pylabhub::utils::LifecycleGuard app_lifecycle(pylabhub::utils::MakeModDefList(
        //     pylabhub::utils::FileLock::GetLifecycleModule(true),
        //     pylabhub::utils::Logger::GetLifecycleModule()
        // ));

        // Attempt to acquire a lock with a 2-second timeout.
        pylabhub::utils::FileLock lock(path,
                                     pylabhub::utils::ResourceType::File,
                                     std::chrono::seconds(2));

        if (lock.valid())
        {
            // Lock successfully acquired.
            LOGGER_INFO("Acquired lock for resource: {}", lock.get_locked_resource_path()->string());
            // ... Safely access the resource ...
        }
        else
        {
            // Lock could not be acquired. Now we can get the specific error.
            LOGGER_ERROR("Failed to acquire lock for {}. Error: {}", path.string(),
                         lock.error_code().message());
        }
        // The lock is automatically released here when the `lock` object goes out of scope.
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
