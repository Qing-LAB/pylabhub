# C++ Utilities (`pylabub::utils`) Documentation

This document provides design and usage notes for the core C++ utilities found in the `pylabub::utils` namespace.

---

## `JsonConfig`

The `JsonConfig` class provides a robust, thread-safe, and process-safe interface for managing JSON configuration files.

### Design Philosophy

- **Concurrency:** The class is designed for heavy concurrent use, both from multiple threads within a single process and from multiple cooperating processes.
- **Robustness:** On-disk file writes are atomic, ensuring that the configuration file is never left in a partially-written or corrupt state, even if the application crashes.
- **Safety:** The public API is `noexcept`, catching internal errors and translating them into simple boolean return values to prevent exceptions from propagating into consumer code.
- **ABI Stability:** It uses the Pimpl idiom (`std::unique_ptr<Impl>`) to hide all private data members, ensuring that future changes to the implementation do not break binary compatibility for shared library consumers.

### Concurrency Model

`JsonConfig` uses a sophisticated two-level locking strategy to balance performance and safety.

#### Level 1: Structural Lock (`initMutex`)
A coarse-grained `std::mutex` is used to serialize operations that affect the object's structure, lifecycle, or its relationship with the filesystem. This includes:
- `init()`, `reload()`, `replace()`, `save()`
- Move constructors and move assignment operators.

This lock ensures that the core state of the object (like its file path or the `Impl` pointer itself) cannot be changed while another thread is in the middle of a critical operation.

#### Level 2: Data Lock (`rwMutex`)
A fine-grained `std::shared_mutex` is used exclusively to protect access to the in-memory `nlohmann::json` data object.
- **Read Access** (`get`, `has`, etc.) uses a `std::shared_lock`, allowing for high-throughput concurrent reads from multiple threads.
- **Write Access** (`set`, `erase`, etc.) uses a `std::unique_lock`, ensuring that only one thread can modify the data at a time.

### Critical Risk Analysis: Concurrent Move Operations

The high-performance locking model (using `rwMutex` for simple accessors) introduces a critical rule for users of the class:

**WARNING: You MUST externally synchronize move operations.**

If one thread attempts to move a `JsonConfig` object while another thread is calling any method on it, a **use-after-free** memory error will occur, leading to a crash.

**Scenario:**
1.  **Thread A** calls `config.get("key")` and passes the initial checks.
2.  **Thread B** executes `JsonConfig new_config = std::move(config);`. This action transfers the internal implementation pointer from `config` to `new_config` and deallocates the memory `config` was managing.
3.  **Thread A** resumes execution and attempts to access its now-dangling internal pointer, causing a crash.

This is a deliberate design trade-off that prioritizes performance. The responsibility is on the consumer to ensure that an object is not being used by one thread while being moved by another. For long-lived, shared objects (like a global configuration singleton), consider making the object `const` or avoiding move operations after initialization.
