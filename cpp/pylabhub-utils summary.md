Project Architecture Summary
Our goal is to build a robust, high-performance, and modular C++ project. The architecture is based on the following key principles:

1. Unified pylabhub-utils Shared Library

What: All common utilities (Logger, JsonConfig, FileLock, AtomicGuard) are consolidated into a single, dedicated shared library named pylabhub-utils.
Why: This is the cornerstone of the architecture. It ensures that any component linking against it gets the same implementation, which is critical for singleton patterns. It also promotes modularity and reduces compilation times for consumers.
2. Asynchronous, High-Performance Logging

What: The Logger uses an asynchronous, worker-thread model. Log calls from application threads are non-blocking; they simply push formatted messages to a queue.
Why: This decouples application performance from I/O latency (disk/network), ensuring the logger does not become a bottleneck in a high-throughput environment.
3. Guaranteed Process-Wide Singletons

What: The Logger's internal implementation (Impl) is a true process-wide singleton, managed by std::call_once.
Why: By placing this singleton implementation inside the pylabhub-utils shared library, the operating system's dynamic linker guarantees there is only one instance of the logger's worker thread and queue for the entire application process. This prevents race conditions and garbled output when multiple plugins or modules use the logger simultaneously.
4. Consistent Structure and Unified Staging

What:
All utility headers are located in include/utils/.
All utility source files are located in src/utils/.
All build artifacts—executables (including tests) and required shared libraries—are copied to a single stage/bin directory.
Why: This creates a clean, predictable project structure. The unified staging directory ensures that the application and its tests are run in an environment that exactly mirrors the final deployment layout, which is crucial for reliability and catching runtime dependency issues.
In short, the core principle is:

All utilities reside in the pylabhub-utils shared library. All other components (executables, tests, plugins) are consumers that link against pylabhub::utils to use them.

This establishes a clean, modern, and maintainable foundation for the project.