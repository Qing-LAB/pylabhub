| Property       | Value                                        |
| -------------- | -------------------------------------------- |
| **HEP**        | `core-0004`                                  |
| **Title**      | High-Performance Asynchronous Logger         |
| **Author**     | Gemini AI                                    |
| **Status**     | Draft                                        |
| **Category**   | Core                                         |
| **Created**    | 2026-01-30                                   |
| **C++-Standard** | C++20                                        |

## Abstract

This Hub Enhancement Proposal (HEP) describes the design of `pylabhub::utils::Logger`, a high-performance, asynchronous, thread-safe logging framework. Its core architecture is based on a decoupled command queue and a dedicated worker thread, ensuring that logging operations have minimal performance impact on application-critical threads. The logger is extensible through a sink-based architecture and integrates with the `LifecycleManager` for graceful startup and shutdown.

## Motivation

Logging is a fundamental requirement for debugging, monitoring, and auditing applications. However, naive logging implementations that perform I/O operations (e.g., writing to a file or the console) directly on the calling thread can introduce significant latency, harming application performance and responsiveness. This is especially problematic in multi-threaded, high-throughput environments.

The `Logger` module is designed to solve this by providing:
-   **Low-Latency Logging Calls**: Application threads should be able to log a message and continue their work with near-zero delay.
-   **Thread Safety**: Multiple threads must be able to log concurrently without data races or garbled output.
-   **Extensibility**: The logging backend should be easily configurable to direct output to various destinations (console, file, syslog, etc.).
-   **Robustness**: The logger should handle high message volumes gracefully and ensure that logs are not lost during a clean shutdown.
-   **Modern C++ Practices**: The API should be type-safe, easy to use, and leverage modern C++ features like compile-time format string validation.

## Rationale and Design

The logger's design prioritizes performance and thread safety by decoupling the logging call site from the actual I/O work.

### Core Principles

-   **Asynchronous Command-Queue Pattern**: The central design pattern is a producer-consumer model.
    -   **Producers**: Application threads calling logging macros (`LOGGER_INFO`, etc.). These are lightweight calls that format a log message and emplace a `LogMessage` command object onto a shared, thread-safe queue.
    -   **Consumer**: A single, dedicated background worker thread is the sole consumer of the queue. This thread is responsible for all potentially blocking I/O operations.

-   **Dedicated Worker Thread**: By offloading all I/O to a single worker, application threads are shielded from latency. This design also naturally serializes access to shared resources (like file handles), eliminating the need for fine-grained locking within sinks.

-   **Sink Abstraction**: A `Sink` is a polymorphic base class defining a simple interface (`write`, `flush`, `description`). Concrete implementations (`ConsoleSink`, `FileSink`, `RotatingFileSink`, `SyslogSink`, `EventLogSink`) encapsulate the details of writing to a specific destination. This makes the logger highly extensible.

-   **Bounded Queue**: The command queue has a configurable maximum size. If the producers generate logs faster than the consumer can write them, the queue will eventually fill up. To prevent unbounded memory growth, new messages are dropped when the queue is full. The logger logs a recovery message once the queue has capacity again, indicating how many messages were dropped. This is a deliberate trade-off in favor of application stability and performance over guaranteed log delivery in extreme scenarios.

-   **Compile-Time Format String Validation**: The primary logging macros (`LOGGER_INFO`, etc.) leverage the `{fmt}` library's `format_string` feature. This validates the format string and arguments at compile time, catching errors that would otherwise be runtime exceptions. This significantly improves the robustness of the logging code.

-   **Lifecycle Integration**: The `Logger` is a `LifecycleManager` module.
    -   **Startup**: The worker thread is started only when `LifecycleManager::initialize()` is called. Calling logger configuration methods before this results in a fatal error, while log messages are silently dropped.
    -   **Shutdown**: During `LifecycleManager::finalize()`, the logger's shutdown sequence is triggered. This involves queuing a shutdown command and waiting for the worker thread to process all pending messages, ensuring a graceful flush of all logs before the application exits.

-   **ABI Stability (Pimpl Idiom)**: The public `Logger` class uses the Pimpl idiom to hide all internal state (queue, mutexes, worker thread, etc.) within `Logger::Impl`. This guarantees a stable ABI, which is critical for a shared library.

### API Specification

#### `Logger` Class
The main interface is a singleton accessible via `Logger::instance()`.

```cpp
class PYLABHUB_UTILS_EXPORT Logger {
public:
    enum class Level { L_TRACE, L_DEBUG, L_INFO, L_WARNING, L_ERROR, L_SYSTEM };

    static Logger &instance();
    static ModuleDef GetLifecycleModule();
    static bool lifecycle_initialized() noexcept;

    // --- Sink Configuration (Asynchronous Commands) ---
    [[nodiscard]] bool set_console();
    [[nodiscard]] bool set_logfile(const std::string &utf8_path, bool use_flock);
    [[nodiscard]] bool set_rotating_logfile(const std::filesystem::path &base_filepath,
                                            size_t max_file_size_bytes, size_t max_backup_files,
                                            bool use_flock, std::error_code &ec) noexcept;
    [[nodiscard]] bool set_syslog(const char *ident = nullptr, int option = 0, int facility = 0);
    [[nodiscard]] bool set_eventlog(const wchar_t *source_name);

    // --- Control (Blocking) ---
    void shutdown();
    void flush();

    // --- Runtime Configuration ---
    void set_level(Level lvl);
    Level level() const;
    void set_max_queue_size(size_t max_size);
    // ... other configuration methods ...

    // --- Logging Macros (Primary API) ---
    // #define LOGGER_INFO(fmt, ...)
    // #define LOGGER_INFO_SYNC(fmt, ...)
    // #define LOGGER_INFO_RT(fmt, ...)
};
```
A full set of macros (`LOGGER_TRACE`, `LOGGER_DEBUG`, `LOGGER_INFO`, `LOGGER_WARN`, `LOGGER_ERROR`, `LOGGER_SYSTEM`) is provided, along with `_SYNC` and `_RT` (runtime format) variants.

#### `Sink` Interface
```cpp
class Sink {
public:
    virtual ~Sink() = default;
    virtual void write(const LogMessage &msg, WRITE_MODE mode) = 0;
    virtual void flush() = 0;
    virtual std::string description() const = 0;
};
```

## Risk Analysis and Mitigations

-   **Risk**: Loss of log messages if the application crashes.
    -   **Mitigation**: This is an inherent trade-off of an asynchronous logger. The `flush()` method can be called at critical points to mitigate this. For the most critical errors, `LOGGER_..._SYNC()` macros can be used to guarantee the message is written before proceeding, at the cost of blocking the thread.
-   **Risk**: Unbounded memory usage if logs are produced much faster than they are consumed.
    -   **Mitigation**: The command queue is bounded. When full, new messages are dropped. The queue size is configurable (`set_max_queue_size`), and a "messages dropped" notification is logged upon recovery.
-   **Risk**: Deadlock if a user-provided error callback tries to call a blocking logger function.
    -   **Mitigation**: The `set_write_error_callback` uses a dedicated `CallbackDispatcher` thread, so the user callback is executed outside the logger's internal locks, preventing deadlocks.
-   **Risk**: Performance bottleneck on the single worker thread.
    -   **Mitigation**: For most applications, a single consumer thread writing to modern storage is sufficient. The primary bottleneck is I/O, not the worker itself. If this were to become an issue, the design could be extended to support multiple workers with more complex queueing, but this adds significant complexity.

## Copyright

This document is placed in the public domain or under the CC0-1.0-Universal license, whichever is more permissive.
