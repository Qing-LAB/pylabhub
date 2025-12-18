# Logger Design Revision Summary

This document outlines the completely redesigned architecture of the `pylabub::utils::Logger`. The new design addresses the deadlocking, race conditions, and performance issues of the previous implementation by adopting a robust, asynchronous, command-queue pattern.

## 1. Core Design Principles

The new architecture is founded on a few key principles to ensure performance, thread-safety, and extensibility.

### 1.1 Command-Queue Architecture
All public API calls—whether logging a message or changing configuration—are translated into **command objects**. These commands are placed onto a single, thread-safe queue. This makes all public-facing methods fast, non-blocking, and lightweight, as they only perform minimal work before returning control to the application.

### 1.2 Single Worker Thread
A dedicated background thread is the sole consumer of the command queue. This thread is responsible for **all I/O operations**, including writing to files, the console, or other sinks.

**This is the cornerstone of the new design's stability.** By ensuring only one thread ever interacts with file handles or other I/O resources, the entire class of deadlocks and race conditions related to resource contention is eliminated at an architectural level.

### 1.3 Sink Abstraction
A new `Sink` abstract base class defines a simple interface for log destinations. Concrete implementations (`ConsoleSink`, `FileSink`, `SyslogSink`, `EventLogSink`) encapsulate all logic specific to their destination. This makes the logger highly modular and easy to extend with new sinks (e.g., for network logging) in the future without modifying core logger code.

### 1.4 Minimal Locking & Asynchronous API
The only lock acquired by application threads is a brief, short-lived lock to push a command onto the queue. All expensive operations (file I/O, sink creation/destruction) are performed without locks on the worker thread.

Methods like `set_logfile` or `set_console` are now fully asynchronous. They return immediately after queueing a command for the worker to execute, preventing the application from stalling on I/O.

## 2. API Overview

The public API remains conceptually similar but is now fully asynchronous.

- `set_console()`, `set_logfile()`, `set_syslog()`, `set_eventlog()`: Non-blocking methods that queue a command for the worker to switch the active sink.
- `log_...()` / `LOGGER_...` macros: Non-blocking calls that queue a log message.
- `flush()`: A synchronous, blocking call that waits until the worker has processed all commands currently in the queue.
- `shutdown()`: A synchronous call that flushes the queue and then permanently stops the worker thread, ensuring a graceful exit.

## 3. Thread Safety & Deadlock Resolution

The previous design suffered from deadlocks due to complex interactions between multiple mutexes and blocking I/O calls (like `flock`). The new design resolves this fundamentally:

- **Serialization**: All operations are serialized into a single command queue, executed in order by one thread. There is no possibility of a `set_logfile` operation racing with a `write` operation, as they are handled sequentially.
- **No Contention**: Since only the worker thread opens/closes/writes to files, there is no inter-thread contention for I/O resources, and thus no need for complex locking around them. The `flock`-related deadlock is now impossible.

## 4. PImpl and ABI Safety

The new design maintains the PImpl (`std::unique_ptr<Impl>`) idiom to ensure a stable ABI, which is critical for the `pylabub::utils` dynamic library.

- The `Logger` class is a singleton, accessible via `Logger::instance()`.
- Its constructor is private, and it holds a `std::unique_ptr` to the `Impl`.
- The `Logger` destructor is correctly defined in the `.cpp` file, ensuring the `Impl` can be safely destroyed without its definition being exposed in the public header.

This maintains the strong ABI compatibility guarantees of the original design.
