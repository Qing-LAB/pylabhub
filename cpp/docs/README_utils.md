# C++ Utilities (`pylabhub-utils`) Documentation

This document provides design and usage notes for the core C++ utilities found in the `pylabhub-utils` shared library. The components are primarily located within the `pylabhub::utils` C++ namespace.

---

## 1. `pylabhub-utils` Library Design

`pylabhub-utils` is the primary **shared library** for the pyLabHub C++ application. It contains high-level, application-aware components that provide core functionalities like logging, configuration management, and inter-process synchronization. In the build system, it is exposed via the `pylabhub::utils` CMake alias target.

### Core Design Principles

*   **Shared and Dynamic**: As a shared library (`.dll` on Windows, `.so` on Linux), it is loaded at runtime by the main application.
*   **ABI Stability**: This is the most critical design constraint. All public interfaces are designed to be ABI-stable using the Pimpl idiom, C-style interfaces, and controlled symbol exports.
*   **Lifecycle Aware**: Utilities in this library hook into the `LifecycleManager` to ensure they are initialized and shut down in the correct order.

---

## 2. Application Lifecycle Management (C++ Namespace: `pylabhub::utils`)

The `LifecycleManager` is the **foundational component** of the pyLabHub C++ application's utility layer. It is designed to be self-sufficient and independent of other `pylabhub::utils` modules (such as `Logger` or `JsonConfig`) for its internal operations and error reporting. This ensures it can reliably manage the startup and shutdown of all other components, even before they are fully initialized or during their finalization. For internal diagnostics and debugging, `LifecycleManager` utilizes dedicated `PLH_DEBUG` messages and, for fatal errors, `PLH_PANIC`, both of which print directly to `stderr` without relying on any external logging infrastructure.

The application lifecycle is managed by the `pylabhub::utils::LifecycleGuard` RAII object. It ensures that modules are started in the correct order based on their declared dependencies and shut down in the reverse order.

### How to Use
In `main()`, create a `LifecycleGuard` instance on the stack, passing it the `ModuleDef` objects from all the utilities your application will use. The `LifecycleGuard`'s constructor initializes the application, and its destructor handles graceful shutdown.

### Full Example

```cpp
#include "utils/Lifecycle.hpp"
#include "utils/Logger.hpp"
#include "utils/FileLock.hpp"
#include "utils/JsonConfig.hpp"

int main(int argc, char* argv[]) {
    // Construct the guard with all necessary module definitions
    pylabhub::utils::LifecycleGuard app_lifecycle(
        pylabhub::utils::Logger::GetLifecycleModule(),
        pylabhub::utils::FileLock::GetLifecycleModule(true), // Enable cleanup on shutdown
        pylabhub::utils::JsonConfig::GetLifecycleModule()
    );

    LOGGER_INFO("Application started successfully.");
    // ... main application logic ...
    return 0; // app_lifecycle destructor handles shutdown here
}
```

---

## 3. Dynamic Module Management

In addition to the static lifecycle, the `LifecycleManager` also supports **dynamic modules**. These are components that can be loaded on-demand at any point during the application's runtime and unloaded to free up resources. This is ideal for optional features, plugins, or resource-intensive tools that are not needed for the entire application session.

### How to Use Dynamic Modules

Dynamic modules provide flexibility, but they operate within the context of the initialized static core.

1.  **Initialize Static Core**: Create the `LifecycleGuard` with all necessary *static* modules (like `Logger`). This initiates the application's core services. **Dynamic module registration or loading cannot occur before this step is complete.**
2.  **Define and Register Dynamic Modules**: At any point *after* static initialization, you can define a dynamic module and register it using `RegisterDynamicModule()`. Registration will fail gracefully (returning `false`) if called prematurely (before `initialize()`) or if a dependency is not found.
3. **Load and Unload**: Use `LoadModule("MyModule")` and `UnloadModule("MyModule")` to control the module's lifecycle. `LoadModule` will panic if called before `initialize()` is complete.

The system uses reference counting. `LoadModule` increments a counter, and `UnloadModule` decrements it. The module's shutdown callback is only run when the counter reaches zero.

**Graceful Failure Handling**: A key feature of the dynamic module system is its robust error handling. Unlike the static initialization which is fatal on failure, any error during a dynamic module's loading process (e.g., an unmet dependency, or an exception in its startup callback) is handled gracefully. `LoadModule` will simply return `false`, and the error will be logged. This ensures that the failure of an optional component will not crash the main application, allowing the calling code to manage the failure as needed.

### Dynamic Module Example

```cpp
#include "utils/Lifecycle.hpp"
#include "utils/Logger.hpp"
#include <atomic>

// --- Define an optional, dynamic module ---
namespace analytics
{
std::atomic<bool> g_analytics_started = false;

void startup(const char*) {
    LOGGER_INFO("Analytics module started!");
    g_analytics_started = true;
}
void shutdown(const char*) {
    LOGGER_INFO("Analytics module shut down.");
    g_analytics_started = false;
}

ModuleDef GetLifecycleModule() {
    ModuleDef module("AnalyticsModule");
    // This dynamic module depends on the static Logger module
    module.add_dependency("pylabhub::utils::Logger");
    module.set_startup(startup);
    module.set_shutdown(shutdown, 1000);
    return module;
}
} // namespace analytics


int main(int argc, char* argv[]) {
    // 1. Create the LifecycleGuard to initialize static modules.
    //    No dynamic module operations are allowed before this.
    pylabhub::utils::LifecycleGuard app_lifecycle(
        pylabhub::utils::Logger::GetLifecycleModule()
    );

    LOGGER_INFO("Application started. Static modules are running.");
    
    // 2. Register a new dynamic module at runtime. This can only happen AFTER init.
    if (!RegisterDynamicModule(analytics::GetLifecycleModule())) {
        LOGGER_ERROR("Failed to register analytics module!");
        return 1;
    }
    LOGGER_INFO("Analytics module is registered but not loaded.");
    // assert(g_analytics_started == false);

    // 3. Load the dynamic module when needed. This can only happen AFTER init.
    LOGGER_INFO("Loading analytics module...");
    if (LoadModule("AnalyticsModule")) {
        LOGGER_INFO("Analytics module loaded successfully.");
        // assert(g_analytics_started == true);
    }

    // A second call to LoadModule just increments the reference count.
    LoadModule("AnalyticsModule");

    // 4. Unload the module. It won't shut down yet (ref count > 0).
    LOGGER_INFO("Unloading analytics module (first unload)...");
    UnloadModule("AnalyticsModule");
    // assert(g_analytics_started == true); // Still running

    // The second unload will cause it to shut down.
    LOGGER_INFO("Unloading analytics module (second unload)...");
    UnloadModule("AnalyticsModule");
    // assert(g_analytics_started == false); // Now it's shut down.

    return 0;
}


---

## 4. Core Utilities (C++ Namespace: `pylabhub::utils`)

### `Logger`

The `pylabhub::utils::Logger` class provides a high-performance, asynchronous, and thread-safe logging utility.

*   **Key Public Interfaces**:
    *   `Logger::instance()`: Accessor for the singleton logger instance.
    *   `set_level(Logger::Level lvl)`: Sets the minimum log level to record.
    *   `set_logfile(path, append)`: Switches the log output to a file.
    *   `set_rotating_logfile(base_filepath, max_file_size_bytes, max_backup_files, use_flock)`: Configures a rotating file log. The log file will automatically rotate when it exceeds `max_file_size_bytes`, keeping up to `max_backup_files`. `use_flock` enables inter-process locking on POSIX systems.

*   **Basic Usage**:
    ```cpp
    #include "utils/Logger.hpp"

    void my_function() {
        // Log messages using the fmt::format style
        LOGGER_INFO("Processing user {} with ID {}", "Alice", 123);
        LOGGER_WARN("A recoverable issue occurred.");

        // Set up a rotating log file: "app.log", max 10MB, keep 5 backups, use flock
        std::error_code ec;
        if (!pylabhub::utils::Logger::instance().set_rotating_logfile(
                "app.log", 10 * 1024 * 1024, 5, true, ec)) {
            LOGGER_ERROR("Failed to set rotating logfile: {}", ec.message());
        }

        // Ensure critical messages are written before continuing
        Logger::instance().flush();
    }
    ```
    *   `set_console()`: Switches the log output to the console (stderr).
    *   `flush()`: Blocks until all pending log messages have been written.
    *   `LOGGER_INFO(format, ...)`: Macro for logging an informational message (also `TRACE`, `DEBUG`, `WARN`, `ERROR`, `SYSTEM`).

*   **Basic Usage**:
    ```cpp
    #include "utils/Logger.hpp"

    void my_function() {
        // Log messages using the fmt::format style
        LOGGER_INFO("Processing user {} with ID {}", "Alice", 123);
        LOGGER_WARN("A recoverable issue occurred.");

        // Ensure critical messages are written before continuing
        Logger::instance().flush();
    }
    ```

*   **Lifecycle and Shutdown Behavior**:
    The Logger is a lifecycle-aware component managed by the `LifecycleManager`. Its shutdown process is designed to be robust and prevent data loss, even in chaotic scenarios.

    *   **Initialization**: The logger's background worker thread is started during the application's `initialize` phase. Using the logger before this is complete has two distinct behaviors:
        *   **Configuration functions** (e.g., `set_level`, `set_logfile`, `flush`) will trigger a fatal error and **abort** the application. This is a fail-fast mechanism to catch incorrect setup.
        *   **Logging macros** (e.g., `LOGGER_INFO`, `LOGGER_WARN`) will **silently drop** the message. This ensures that stray log calls from static destructors or other pre-main contexts do not cause a crash.

    *   **Shutdown Process**:
        1.  When the application's `finalize` phase begins, the logger transitions to a `ShuttingDown` state.
        2.  In this state, any new calls to logger functions (e.g., `LOGGER_INFO`, `set_level`) from other threads are immediately and silently ignored. This prevents deadlocks and makes the shutdown process robust against race conditions from threads that have not yet terminated.
        3.  The logger's background thread continues to run until it has processed all messages that were already in its queue before the shutdown was initiated.
        4.  After the queue is empty, the worker performs one final, automatic `flush()` on the current sink (e.g., file or console) to ensure all messages are written to the output.
        5.  The worker thread then cleanly exits.

    *   **Corner Cases**:
        *   **Logging During/After Shutdown**: If any part of the application attempts to log while the logger is shutting down or after it has shut down, the call is safely and silently ignored. The application will not crash.
        *   **`flush()` During/After Shutdown**: Calling `Logger::instance().flush()` while the logger is shutting down or has shut down is a safe no-op. It will return immediately without blocking or throwing an error.

### `JsonConfig`

The `pylabhub::utils::JsonConfig` class provides a robust, process-safe interface for managing JSON configuration files.

*   **Key Public Interfaces**:
    *   `JsonConfig(path, create, &ec)`: Constructor to initialize with a file path.
    *   `init(path, create, &ec)`: Initializes or re-initializes the object.
    *   `reload(&ec)`: Reloads the configuration from disk.
    *   `save(&ec)`: Saves the current in-memory configuration to disk.
    *   `with_json_read(callback, &ec)`: Provides safe, read-only access to the internal JSON object.
    *   `with_json_write(callback, &ec, timeout)`: Provides safe, write access. `timeout` is a `std::optional<std::chrono::milliseconds>` which defaults to `std::nullopt`, resulting in a blocking write. Provide a duration for a timed write.

*   **Basic Usage**:
    The `with_json_...` methods are the safest way to interact with the configuration, as they handle locking and unlocking automatically. Overloads are provided for convenience when an error code is not needed.

    ```cpp
    #include "utils/JsonConfig.hpp"
    #include <string>
    #include <nlohmann/json.hpp>
    #include <chrono>

    void access_config(pylabhub::utils::JsonConfig& cfg) {
        using namespace std::chrono_literals;

        // Read a value using the simple overload (no error code).
        std::string name;
        if (cfg.with_json_read([&](const nlohmann::json& j) {
            name = j.value("name", "default_name");
        })) {
            // Read was successful
        }

        // Write a value with a 100ms timeout.
        cfg.with_json_write([&](nlohmann::json& j) {
            j["attempt_count"] = j.value("attempt_count", 0) + 1;
        }, 100ms); // Uses the new overload without ec, with timeout

        // For cases where you need both detailed error information AND a timeout,
        // use the original, most explicit overload.
        std::error_code ec;
        cfg.with_json_write([&](nlohmann::json& j) {
            j["last_access_time"] = std::time(nullptr);
        }, &ec, 100ms); // Providing both ec and timeout
        if (ec) {
            // Handle the error...
        }
    }
    ```

### `FileLock`

The `pylabhub::utils::FileLock` class is a cross-platform, RAII-style utility for creating *advisory* inter-process and inter-thread locks.

*   **Key Public Interfaces**:
    *   `FileLock(path, type, mode)`: RAII constructor that acquires the lock. `mode` can be `Blocking` or `NonBlocking`.
    *   `FileLock(path, type, timeout)`: RAII constructor that acquires a lock, blocking up to a specified `timeout`.
    *   `valid()`: Checks if the lock was successfully acquired.
    *   `get_expected_lock_fullname_for(path, type)`: A static method to predict the lock file's name.
    *   `try_lock(path, type, ...)`: A static factory method that returns an `std::optional<FileLock>`.

*   **Basic Usage (Constructor-based)**:
    ```cpp
    #include "utils/FileLock.hpp"
    #include <filesystem>

    void safe_file_write(const std::filesystem::path& resource_path) {
        // Lock will be acquired in the constructor.
        pylabhub::utils::FileLock lock(
            resource_path,
            pylabhub::utils::ResourceType::File,
            pylabhub::utils::LockMode::Blocking
        );

        if (!lock.valid()) {
            // Failed to acquire lock
            return;
        }

        // ... perform safe file operations here ...

    } // Lock is automatically released here when 'lock' goes out of scope.
    ```

*   **Modern Usage (`try_lock` Pattern)**:
    The static `try_lock` methods provide a more expressive, modern C++ interface.

    ```cpp
    #include "utils/FileLock.hpp"
    #include <filesystem>
    #include <chrono>

    void safer_file_write(const std::filesystem::path& resource_path) {
        using namespace std::chrono_literals;

        // Attempt to acquire the lock with a 100ms timeout.
        if (auto lock = pylabhub::utils::FileLock::try_lock(resource_path, pylabhub::utils::ResourceType::File, 100ms)) {
            // Success! The lock is valid and its scope is confined to this block.
            // ... perform safe file operations here ...

        } else {
            // Handle lock failure (timed out or another error).
        }

    } // If acquired, the lock is automatically released here.
    ```

