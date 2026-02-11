# `pylabhub-utils` Library

Common, robust utilities for C++ applications in scientific and laboratory environments. Modules are thread-safe, process-safe, and integrated into a unified lifecycle management system.

## Table of Contents

- [Umbrella Headers](#umbrella-headers)
- [Goals & Design Principles](#goals--design-principles)
- [Quick Start](#quick-start)
- [Recipes by Use Case](#recipes-by-use-case)
- [Module Overview](#module-overview)
- [Technical Specifications (HEPs)](#technical-specifications-heps)
- [Troubleshooting](#troubleshooting)
- [API Reference](#api-reference)

---

## Umbrella Headers

**Use umbrella headers** so you don't have to manage include order or module dependencies. Each layer includes the previous one in the correct order.

| Layer | Header | Provides |
|-------|--------|----------|
| **0** | `plh_platform.hpp` | Platform detection (`PYLABHUB_PLATFORM_WIN64`, etc.), Windows headers, version API (`get_version_*`) |
| **1** | `plh_base.hpp` | `format_tools`, `debug_info`, `atomic_guard`, `recursion_guard`, `scope_guard`, `module_def` |
| **2** | `plh_service.hpp` | `lifecycle`, `file_lock`, `logger` |
| **3** | `plh_datahub.hpp` | `json_config`, `message_hub`, `data_block` |

**Choose the smallest umbrella that covers your needs:**

| Use case | Include |
|----------|---------|
| Platform macros only | `#include "plh_platform.hpp"` |
| Formatting, guards, `ModuleDef` | `#include "plh_base.hpp"` |
| Lifecycle, FileLock, Logger | `#include "plh_service.hpp"` |
| Config, MessageHub, DataBlock | `#include "plh_datahub.hpp"` |

**Benefits:** No include-order issues, single include per layer, clear dependency hierarchy. Including a higher layer pulls in everything below it; use the smallest layer that has what you need.

---

## Goals & Design Principles

| Goal | Description |
|------|-------------|
| **Decouple components** | Lifecycle management and standardized interfaces |
| **High-performance IPC** | Data Exchange Hub for GiB/s data pipelines |
| **Reliability** | Defensive design, crash recovery |
| **ABI stability** | Pimpl idiom for shared library evolution |

| Principle | Description |
|-----------|-------------|
| **Lifecycle Management** | All major utilities are lifecycle modules; registered at startup |
| **ABI Stability** | Pimpl idiom and explicit symbol visibility |
| **Safety** | Thread and process safety using native OS primitives |
| **Modern C++** | C++20 |

**⚠️ Thread Safety Summary:**

| Module | Notes |
|--------|-------|
| **FileLock** | Process-safe and thread-safe |
| **Logger** | Fully thread-safe; lock-free queue |
| **JsonConfig** | Thread-safe reads; exclusive writes via FileLock |
| **Lifecycle** | Module registration NOT thread-safe (before `initialize()`); dynamic load/unload IS thread-safe |
| **RecursionGuard** | Thread-local only; does NOT prevent cross-thread recursion |

---

## Quick Start

Minimal application with Logger (using umbrella header):

```cpp
#include "plh_service.hpp"

int main() {
    pylabhub::utils::LifecycleGuard app(
        pylabhub::utils::Logger::GetLifecycleModule()
    );

    LOGGER_INFO("Application started.");
    // ... your logic ...
    return 0;  // FinalizeApp() called automatically
}
```

---

## Recipes by Use Case

### Recipe 1: Logger + FileLock (Config File Protection)

**Scenario:** Protect a config file from concurrent access while logging.

```cpp
#include "plh_service.hpp"

int main() {
    pylabhub::utils::LifecycleGuard app(
        pylabhub::utils::MakeModDefList(
            pylabhub::utils::FileLock::GetLifecycleModule(),
            pylabhub::utils::Logger::GetLifecycleModule()
        )
    );

    auto config_path = std::filesystem::path("/etc/myapp/config.json");
    if (auto lock = pylabhub::utils::FileLock::try_lock(
            config_path, pylabhub::utils::ResourceType::File,
            pylabhub::utils::LockMode::NonBlocking)) {
        LOGGER_INFO("Lock acquired, updating config...");
        // ... read/write config ...
    } else {
        LOGGER_WARN("Config locked by another process");
    }
    return 0;
}
```

---

### Recipe 2: Rotating Log File

**Scenario:** Log to a file that rotates when it reaches a size limit.

```cpp
#include "plh_service.hpp"

int main() {
    pylabhub::utils::LifecycleGuard app(
        pylabhub::utils::Logger::GetLifecycleModule()
    );

    std::error_code ec;
    auto log_path = std::filesystem::temp_directory_path() / "myapp.log";
    if (pylabhub::utils::Logger::instance().set_rotating_logfile(
            log_path, 1024 * 1024, 5, ec)) {
        LOGGER_SYSTEM("Logging to rotating file: {}", log_path.string());
    } else {
        LOGGER_ERROR("Failed: {}", ec.message());
        return 1;
    }

    for (int i = 0; i < 1000; ++i)
        LOGGER_DEBUG("Message #{}", i);

    pylabhub::utils::Logger::instance().flush();
    return 0;
}
```

---

### Recipe 3: Critical Error Before Abort (Synchronous Logging)

**Scenario:** Ensure an error message is written before the process terminates.

```cpp
#include "plh_service.hpp"

void handle_fatal(const std::exception& e) {
    // LOGGER_*_SYNC bypasses the queue; writes immediately
    LOGGER_ERROR_SYNC("Fatal: {} - aborting", e.what());
    std::abort();
}

int main() {
    pylabhub::utils::LifecycleGuard app(
        pylabhub::utils::Logger::GetLifecycleModule()
    );

    try {
        risky_operation();
    } catch (const std::exception& e) {
        handle_fatal(e);
    }
    return 0;
}
```

---

### Recipe 4: JsonConfig with Multi-Process Safety

**Scenario:** Read and write a shared JSON config file safely across processes.

```cpp
#include "plh_datahub.hpp"

int main() {
    pylabhub::utils::LifecycleGuard app(
        pylabhub::utils::MakeModDefList(
            pylabhub::utils::FileLock::GetLifecycleModule(),
            pylabhub::utils::Logger::GetLifecycleModule()
        )
    );

    pylabhub::utils::JsonConfig config;
    std::error_code ec;
    if (!config.init("/path/to/config.json", true, &ec)) {
        LOGGER_ERROR("Config init failed: {}", ec.message());
        return 1;
    }

    // Read with fresh data from disk
    auto name = config.transaction(AccessFlags::ReloadFirst).read(
        [](const auto& j) { return j.value("name", "default"); });

    // Write atomically
    config.transaction(AccessFlags::CommitAfter).write([](auto& j) {
        j["last_run"] = std::time(nullptr);
        return CommitDecision::Commit;
    });

    return 0;
}
```

---

### Recipe 5: Dynamic Plugin (Load/Unload at Runtime)

**Scenario:** Load an optional plugin after the app has started.

```cpp
#include "plh_service.hpp"

namespace MyPlugin {
    void startup(const char*) { LOGGER_INFO("Plugin started"); }
    void shutdown(const char*) { LOGGER_INFO("Plugin shut down"); }

    pylabhub::utils::ModuleDef GetModule() {
        pylabhub::utils::ModuleDef def("MyPlugin");
        def.add_dependency("Logger");
        def.set_startup(startup);
        def.set_shutdown(shutdown, 1000);
        return def;
    }
}

int main() {
    pylabhub::utils::LifecycleGuard app(
        pylabhub::utils::Logger::GetLifecycleModule()
    );

    if (pylabhub::utils::RegisterDynamicModule(MyPlugin::GetModule())) {
        if (pylabhub::utils::LoadModule("MyPlugin")) {
            LOGGER_INFO("Plugin active");
            // ... use plugin ...
            pylabhub::utils::UnloadModule("MyPlugin");
        }
    }
    return 0;
}
```

---

### Recipe 6: FileLock with Timeout

**Scenario:** Wait up to N seconds for a lock, then fail gracefully.

```cpp
#include "plh_service.hpp"

void update_resource(const std::filesystem::path& path) {
    pylabhub::utils::FileLock lock(path,
        pylabhub::utils::ResourceType::File,
        std::chrono::milliseconds(5000));

    if (lock.valid()) {
        LOGGER_INFO("Lock acquired for {}", path.string());
        // ... exclusive access ...
    } else {
        auto ec = lock.error_code();
        if (ec == std::errc::timed_out)
            LOGGER_ERROR("Lock timeout: {}", path.string());
        else
            LOGGER_ERROR("Lock error: {}", ec.message());
    }
}
```

---

### Recipe 7: Multi-Process Config Coordination

**Scenario:** Multiple processes coordinate via a shared JSON config (distributed lock).

```cpp
#include "plh_datahub.hpp"

bool try_acquire_lock(pylabhub::utils::JsonConfig& config, const std::string& lock_name, int pid) {
    using namespace pylabhub::utils;
    return config.transaction(JsonConfig::AccessFlags::FullSync).write([&](auto& j) {
        auto& locks = j["locks"];
        if (locks.contains(lock_name) && locks[lock_name]["owner"] != pid)
            return JsonConfig::CommitDecision::SkipCommit;
        locks[lock_name]["owner"] = pid;
        locks[lock_name]["ts"] = std::time(nullptr);
        return JsonConfig::CommitDecision::Commit;
    });
}
```

---

### Recipe 8: Data Exchange Hub (Producer-Consumer)

**Scenario:** High-throughput data streaming between processes. See [HEP core-0002](./hep/hep-core-0002-data-exchange-hub-framework.md) for full details.

```cpp
// Producer
auto producer = create_datablock_producer(hub, "sensor_data",
    DataBlockPolicy::RingBuffer, config);
char* buf = producer->begin_write(100);
if (buf) { memcpy(buf, data, size); producer->end_write(size, &flex); }

// Consumer
auto consumer = find_datablock_consumer(hub, "sensor_data", secret);
const char* buf = consumer->begin_consume(500);
if (buf) { process(buf); consumer->end_consume(); }
```

---

## Module Overview

| Module | Purpose | HEP |
|--------|---------|-----|
| **Lifecycle** | Dependency-aware startup/shutdown; static & dynamic modules | [core-0001](./hep/hep-core-0001-hybrid-lifecycle-model.md) |
| **Logger** | Async, thread-safe logging; sinks (console, file, syslog, eventlog) | [core-0004](./hep/hep-core-0004-async-logger.md) |
| **FileLock** | Cross-platform RAII file/directory locking | [core-0003](./hep/hep-core-0003-cross-platform-filelock.md) |
| **JsonConfig** | Thread/process-safe JSON config with atomic writes | — |
| **DataBlock** | Shared-memory IPC for high-throughput streaming | [core-0002](./hep/hep-core-0002-data-exchange-hub-framework.md) |
| **ScopeGuard** | RAII cleanup on scope exit | — |
| **RecursionGuard** | Detect re-entrant calls (thread-local) | — |
| **AtomicGuard** | Fast spinlock for critical sections | — |

---

## Technical Specifications (HEPs)

Hub Enhancement Proposals (HEPs) provide detailed technical design, API reference, sequence diagrams, and implementation notes:

| HEP | Title |
|-----|-------|
| [core-0001](./hep/hep-core-0001-hybrid-lifecycle-model.md) | Hybrid (Static & Dynamic) Module Lifecycle |
| [core-0002](./hep/hep-core-0002-data-exchange-hub-framework.md) | Data Exchange Hub Framework |
| [core-0003](./hep/hep-core-0003-cross-platform-filelock.md) | Cross-Platform RAII File Locking |
| [core-0004](./hep/hep-core-0004-async-logger.md) | High-Performance Asynchronous Logger |

---

## Logger: Async vs Sync

| Variant | Macro | Use When |
|---------|-------|----------|
| **Async** (default) | `LOGGER_INFO(...)` | Normal logging; ~100ns, non-blocking |
| **Sync** | `LOGGER_INFO_SYNC(...)` | Critical errors, before abort; blocks until written |
| **Runtime format** | `LOGGER_INFO_RT(...)` | Dynamic format string |

Use `LOGGER_*_SYNC` sparingly—it acquires the sink mutex and performs I/O on the calling thread.

---

## Troubleshooting

### Logger

| Problem | Solution |
|---------|----------|
| "Logger method called before initialization" | Include `Logger::GetLifecycleModule()` in `LifecycleGuard` |
| Messages dropped | Increase `set_max_queue_size()`; check `get_dropped_message_count()` |
| Hangs on shutdown | Check custom sink for blocking I/O; reduce shutdown timeout |

### FileLock

| Problem | Solution |
|---------|----------|
| Lock always times out | Another process holds it; check `get_expected_lock_fullname_for()` |
| Different processes, different locks | Use absolute paths; symlinks can cause different canonical paths |
| Stale `.lock` files after crash | `.lock` files are harmless; if cleanup is desired, use an external script when nothing is running |

### JsonConfig

| Problem | Solution |
|---------|----------|
| Changes not visible to other processes | Use `CommitAfter` or `FullSync`; call `overwrite()` |
| "resource_deadlock_would_occur" | Complete first transaction before starting another |

### Lifecycle

| Problem | Solution |
|---------|----------|
| "Circular dependency detected" | Break cycle; extract shared module |
| Dynamic module won't unload | Unload dependents first; or mark as persistent |

---

## Known Issues and Limitations

1. **DataBlock on Windows:** Mutex support incomplete; do not use in production.
2. **FileLock on NFS:** May be unreliable; use local filesystem for critical locks.
3. **JsonConfig on NFS:** Atomic rename may not hold on some NFS configs.
4. **Logger queue overflow:** Messages dropped when full; tiered strategy (soft/hard limit).
5. **RecursionGuard:** Thread-local; does not prevent cross-thread recursion.

---

## API Reference

**Umbrella headers** (recommended):

| Layer | Header |
|-------|--------|
| Platform | `plh_platform.hpp` |
| Base | `plh_base.hpp` |
| Service | `plh_service.hpp` |
| DataHub | `plh_datahub.hpp` |

**Individual module headers:**

| Module | Header |
|--------|--------|
| Lifecycle | `utils/lifecycle.hpp` |
| Logger | `utils/logger.hpp` |
| FileLock | `utils/file_lock.hpp` |
| JsonConfig | `utils/json_config.hpp` |
| DataBlock | `utils/data_block.hpp` |
| ScopeGuard | `utils/scope_guard.hpp` |
| RecursionGuard | `utils/recursion_guard.hpp` |
| AtomicGuard | `utils/atomic_guard.hpp` |

Each header contains inline documentation. For design rationale, sequence diagrams, and detailed API, see the [HEPs](./hep/).

---

## See Also

- [README_Versioning.md](README_Versioning.md) — Package version scheme and C++ `get_version_*` API
