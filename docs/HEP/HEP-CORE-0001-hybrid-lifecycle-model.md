# HEP-CORE-0001: A Hybrid (Static & Dynamic) Module Lifecycle

| Property         | Value                                          |
| ---------------- | ---------------------------------------------- |
| **HEP**          | `HEP-CORE-0001`                                |
| **Title**        | A Hybrid (Static & Dynamic) Module Lifecycle   |
| **Author**       | Quan Qing, AI assistant                        |
| **Status**       | Implemented                                    |
| **Category**     | Core                                           |
| **Created**      | 2026-01-05                                     |
| **Updated**      | 2026-02-12                                     |
| **C++-Standard** | C++20                                           |

---

## Implementation status

All described APIs are implemented in `src/include/utils/lifecycle.hpp`, `src/include/utils/module_def.hpp`, and three implementation files:
- `src/utils/service/lifecycle.cpp` — public API, ModuleDef, initialize/finalize, log sink
- `src/utils/service/lifecycle_topology.cpp` — buildStaticGraph, topologicalSort, printStatusAndAbort
- `src/utils/service/lifecycle_dynamic.cpp` — all dynamic module operations (load/unload/wait/computeClosure/dynShutdownThread)

They share the private header `src/utils/service/lifecycle_impl.hpp` (not installed).
Static and dynamic modules, `LifecycleGuard` (single/multiple/vector constructors), `MakeModDefList`, and all convenience functions are in use. For current plan and priorities elsewhere, see `docs/TODO_MASTER.md` and `docs/todo/`.

### Source file reference

| File | Layer | Description |
|------|-------|-------------|
| `src/include/utils/lifecycle.hpp` | L2 (public) | `LifecycleManager`, `LifecycleGuard`, `MakeModDefList`, convenience wrappers |
| `src/include/utils/module_def.hpp` | L2 (public) | `ModuleDef` builder, `LifecycleCallback` typedef, constants |
| `src/utils/service/lifecycle_impl.hpp` | internal | `LifecycleManagerImpl`, `InternalGraphNode`, `DynamicModuleStatus` enum |
| `src/utils/service/lifecycle.cpp` | impl | `ModuleDef` API, `register_module`, `initialize`/`finalize`, log sink |
| `src/utils/service/lifecycle_topology.cpp` | impl | `buildStaticGraph()`, Kahn's `topologicalSort()`, cycle detection |
| `src/utils/service/lifecycle_dynamic.cpp` | impl | `loadModule`/`unloadModule`, ref counting, `dynShutdownThreadMain()` |
| `tests/test_layer1_base/test_module_def.cpp` | test | `ModuleDef` builder validation (name, deps, move semantics) |
| `tests/test_layer2_service/test_lifecycle.cpp` | test | Static module init/finalize (multi-process workers) |
| `tests/test_layer2_service/test_lifecycle_dynamic.cpp` | test | Dynamic load/unload, ref counting, async shutdown |

---

## Abstract

This HEP specifies the **LifecycleManager**: a dependency-aware startup/shutdown system that supports **static** modules (registered before init, started once) and **dynamic** modules (registered after init, load/unload at runtime). It provides a single dependency graph, topological ordering, and optional reference counting for dynamic modules.

---

## Motivation

| Use Case | Description |
|----------|-------------|
| **Core services** | Logger, FileLock, etc. — started once with the application |
| **Optional / heavy features** | Load on demand; unload when ref_count drops to zero |
| **Plugin systems** | Third-party extensions registered and loaded after static core is up |
| **Mode-specific code** | Different tool sets for "Editing" vs "Playback" without restart |

---

## Design philosophy

### Goals

| Goal | Description |
|------|-------------|
| **Unified dependency graph** | Static and dynamic modules in one graph; topological sort determines start order |
| **Graceful failure** | Dynamic load failures return `false`; invalid names throw or return false |
| **Strict ordering** | Dynamic modules registered only *after* static init; static cannot depend on dynamic |
| **Reference counting** | Dynamic modules unload only when ref_count drops to zero (unless persistent) |
| **ABI stability** | Pimpl for `LifecycleManager` and `ModuleDef`; public API remains stable |

### Constraints

- **Module registration is NOT thread-safe** — must occur before `initialize()`.
- **Dynamic load/unload IS thread-safe** — protected by internal mutex.
- **RecursionGuard** prevents re-entrant calls from startup/shutdown callbacks (deadlock prevention).
- **Static modules cannot depend on dynamic modules**; dynamic may depend on static.

---

## Architecture overview

### Component diagram

```mermaid
flowchart TB
    subgraph App["Application"]
        direction TB
        LG[LifecycleGuard]
        LM[LifecycleManager]
        subgraph Static["Static Modules"]
            Logger[Logger]
            FileLock[FileLock]
        end
        subgraph Dynamic["Dynamic Modules"]
            PluginA[Plugin A]
            PluginB[Plugin B]
        end
    end

    LG -->|registers & owns| LM
    LM -->|topological start| Static
    LM -->|load/unload| Dynamic
    PluginA -.->|depends on| Logger
    PluginB -.->|depends on| PluginA
```

### Class and API relationships

```mermaid
classDiagram
    class LifecycleManager {
        +instance()
        +register_module(ModuleDef)
        +register_dynamic_module(ModuleDef) bool
        +initialize(source_location)
        +finalize(source_location)
        +load_module(name, loc) bool
        +unload_module(name, loc) bool
        +is_initialized() bool
        +is_finalized() bool
    }
    class ModuleDef {
        +ModuleDef(const char* name)
        +add_dependency(const char*)
        +set_startup(callback)
        +set_startup(callback, data, len)
        +set_shutdown(callback, timeout_ms)
        +set_shutdown(callback, timeout_ms, data, len)
        +set_as_persistent(bool)
    }
    class LifecycleGuard {
        +LifecycleGuard()
        +LifecycleGuard(ModuleDef)
        +LifecycleGuard(vector~ModuleDef~)
        ~LifecycleGuard()
    }

    LifecycleGuard --> LifecycleManager : RegisterModule / InitializeApp
    LifecycleManager --> ModuleDef : consumes
    LifecycleGuard --> ModuleDef : passes
```

### LifecycleGuard owner semantics

```mermaid
flowchart LR
    A[First LifecycleGuard] -->|compare_exchange_strong| B{Owner?}
    B -->|Yes| C[Register modules\nInitializeApp\nm_is_owner = true]
    B -->|No| D[Warn, ignore modules\nm_is_owner = false]
    E[Subsequent LifecycleGuard] --> B
    C --> F[Destructor: FinalizeApp if owner]
    D --> G[Destructor: no-op]
```

Only the **first** `LifecycleGuard` to be constructed becomes the owner (registers modules and calls `InitializeApp`). Its destructor calls `FinalizeApp`. Any later guard is a no-op for registration and finalization.

### Concrete module dependency graph (pylabhub)

The project registers these static modules via `GetLifecycleModule()`. The topological sort
determines startup order; shutdown runs in reverse.

```mermaid
flowchart BT
    Logger["Logger"]
    FileLock["FileLock"]
    Crypto["Crypto"]
    SchemaStore["SchemaStore"]

    FileLock -.->|depends on| Logger
    SchemaStore -.->|depends on| Logger

    subgraph Binaries["Binary entry points"]
        direction LR
        HubShell["hubshell.cpp"]
        ProdMain["producer_main.cpp"]
        ConsMain["consumer_main.cpp"]
        ProcMain["processor_main.cpp"]
    end

    HubShell -->|LifecycleGuard| Logger
    HubShell -->|LifecycleGuard| FileLock
    HubShell -->|LifecycleGuard| Crypto
    ProdMain -->|LifecycleGuard| Logger
    ProdMain -->|LifecycleGuard| Crypto
    ConsMain -->|LifecycleGuard| Logger
    ConsMain -->|LifecycleGuard| Crypto
    ProcMain -->|LifecycleGuard| Logger
    ProcMain -->|LifecycleGuard| Crypto
```

Each binary constructs a `LifecycleGuard` with its required `ModuleDef` set.
The topological sort ensures Logger starts before FileLock and SchemaStore (which depend on it).
Crypto has no dependencies and can start in any position.

### StartupLogFileSink — early log sink switching

`Logger::GetStartupLogFileSinkModule()` returns a `ModuleDef` named `"StartupLogFileSink"`
that depends on `"pylabhub::utils::Logger"` and switches the log sink to a file immediately
after Logger initialises — before any other module emits log messages.

Modules that produce startup log output (e.g. ZMQContext, DataExchangeHub) can declare
`add_dependency("StartupLogFileSink")` so the topological sort places them after the sink
switch, ensuring all their output goes to the file rather than the console.

**Factory method** (in `Logger`):

```cpp
// Plain append-mode log file:
static ModuleDef GetStartupLogFileSinkModule(
    const std::string &log_file_path,
    std::optional<RotatingLogConfig> rotating = std::nullopt);

// Rotating log file:
mods.push_back(Logger::GetStartupLogFileSinkModule(
    "/var/log/hub.log", Logger::RotatingLogConfig{10*1024*1024, 5}));
```

**RotatingLogConfig** fields: `max_file_size_bytes` (default 10 MiB),
`max_backup_files` (default 5 — matches `config::LoggingConfig`'s user-facing
default so JSON-driven and direct C++ users see the same retention),
`timestamped_names` (default `false`, numeric `base.1/base.2/...` rotation),
`use_flock` (default `true`).

The `-1` sentinel in JSON config (`"logging.backups": -1`) maps to
`LoggingConfig::kKeepAllBackups` (`SIZE_MAX`), meaning "never delete rotated
files". JSON value `0` is rejected by the parser. Direct C++ users may still
pass `0` to the sink, which is interpreted defensively as "no backup".

**Usage in role binaries** (`role_main_helpers.hpp`):

```cpp
auto zmq_mod = pylabhub::hub::GetZMQContextModule();
auto hub_mod = pylabhub::hub::GetLifecycleModule();
if (!log_file.empty())
{
    zmq_mod.add_dependency("StartupLogFileSink");
    hub_mod.add_dependency("StartupLogFileSink");
}
auto mods = MakeModDefList(Logger::GetLifecycleModule(), ..., std::move(zmq_mod), std::move(hub_mod));
if (!log_file.empty())
    mods.push_back(Logger::GetStartupLogFileSinkModule(log_file));
```

**Usage in hubshell** (rotating):

```cpp
mods.push_back(Logger::GetStartupLogFileSinkModule(
    hub_log_path, Logger::RotatingLogConfig{}));
```

---

## Public API reference

### LifecycleManager

| Method | Signature | Description |
|--------|-----------|-------------|
| `instance` | `static LifecycleManager& instance()` | Singleton accessor |
| `register_module` | `void register_module(ModuleDef&&)` | Register static module; must be called *before* `initialize()` |
| `register_dynamic_module` | `bool register_dynamic_module(ModuleDef&&)` | Register dynamic module; valid *after* `initialize()`, *before* `finalize()`. Returns false on failure (e.g. duplicate name, missing dependency). |
| `initialize` | `void initialize(std::source_location loc)` | Start all static modules in dependency order. Idempotent. |
| `finalize` | `void finalize(std::source_location loc)` | Shut down all modules in reverse order. Idempotent. |
| `load_module` | `bool load_module(const char* name, std::source_location loc)` | Load dynamic module and dependencies. Thread-safe, idempotent. Returns false if invalid name or load failed. Must not be called from startup/shutdown callback. |
| `unload_module` | `bool unload_module(const char* name, std::source_location loc)` | Unload dynamic module if ref_count is zero; runs shutdown, may cascade. Returns false if still in use. Must not be called from startup/shutdown callback. |
| `is_initialized` | `bool is_initialized()` | True if `initialize()` has been called |
| `is_finalized` | `bool is_finalized()` | True if `finalize()` has been called |

### ModuleDef

| Method | Signature | Description |
|--------|-----------|-------------|
| Constructor | `explicit ModuleDef(const char* name)` | Unique module name. C-string null-terminated, length ≤ `MAX_MODULE_NAME_LEN` (256). Throws `std::invalid_argument` / `std::length_error` if invalid. |
| `add_dependency` | `void add_dependency(const char* dependency_name)` | Add dependency by name. Null or oversized throws. |
| `set_startup` | `void set_startup(LifecycleCallback startup_func)` | Startup callback (no argument; `nullptr` passed). |
| `set_startup` | `void set_startup(LifecycleCallback startup_func, const char* data, size_t len)` | Startup callback with string argument. `len` must not exceed `MAX_CALLBACK_PARAM_STRLEN` (1024). |
| `set_shutdown` | `void set_shutdown(LifecycleCallback shutdown_func, unsigned int timeout_ms)` | Shutdown callback and timeout in milliseconds. |
| `set_shutdown` | `void set_shutdown(..., const char* data, size_t len)` | Shutdown callback with string argument. |
| `set_as_persistent` | `void set_as_persistent(bool persistent = true)` | Dynamic module: if persistent, not unloaded when ref_count reaches zero; only on finalize. |

**Constants**

| Constant | Value | Description |
|----------|-------|-------------|
| `ModuleDef::MAX_MODULE_NAME_LEN` | 256 | Max length (excluding null) for module and dependency names |
| `ModuleDef::MAX_CALLBACK_PARAM_STRLEN` | 1024 | Max length for callback string arguments |

**Callback type:** `typedef void (*LifecycleCallback)(const char* arg);` — C-style for ABI stability; `arg` may be `nullptr` when no data provided.

### LifecycleGuard

| Constructor | Description |
|-------------|-------------|
| `LifecycleGuard(std::source_location loc = ...)` | No modules; if first guard, calls `InitializeApp()` only |
| `explicit LifecycleGuard(ModuleDef&& module, std::source_location loc = ...)` | Single module; if owner, registers it and calls `InitializeApp()` |
| `explicit LifecycleGuard(std::vector<ModuleDef>&& modules, std::source_location loc = ...)` | Multiple modules; if owner, registers all and calls `InitializeApp()` |

Destructor: if this guard is the owner (`m_is_owner`), calls `FinalizeApp(loc)`. Non-copyable, non-movable.

### Convenience functions

| Function | Wrapper for | Notes |
|----------|-------------|--------|
| `RegisterModule(ModuleDef&&)` | `LifecycleManager::instance().register_module()` | Preferred for static registration |
| `InitializeApp(loc)` | `LifecycleManager::instance().initialize(loc)` | Default `loc = std::source_location::current()` |
| `FinalizeApp(loc)` | `LifecycleManager::instance().finalize(loc)` | |
| `IsAppInitialized()` | `LifecycleManager::instance().is_initialized()` | |
| `IsAppFinalized()` | `LifecycleManager::instance().is_finalized()` | |
| `RegisterDynamicModule(ModuleDef&&)` | `LifecycleManager::instance().register_dynamic_module()` | Returns bool |
| `LoadModule(name, loc)` | `LifecycleManager::instance().load_module()` | Returns bool |
| `UnloadModule(name, loc)` | `LifecycleManager::instance().unload_module()` | Returns bool |

### MakeModDefList

| Function | Description |
|----------|-------------|
| `MakeModDefList(Mods&&... mods)` | Builds `std::vector<ModuleDef>` from one or more `ModuleDef` rvalues. Use with `LifecycleGuard(std::move(vec))` or `LifecycleGuard(MakeModDefList(Logger::GetLifecycleModule(), FileLock::GetLifecycleModule()))`. |

---

## Internal implementation details

(Not part of the stable API; may change.)

### Graph node (conceptual)

```cpp
struct InternalGraphNode {
    std::string name;
    std::function<void()> startup;
    InternalModuleShutdownDef shutdown;
    std::vector<std::string> dependencies;
    std::vector<InternalGraphNode*> dependents;
    std::atomic<ModuleStatus> status;
    bool is_dynamic = false;
    bool is_persistent = false;
    std::atomic<DynamicModuleStatus> dynamic_status;
    std::atomic<int> ref_count;
};

enum class DynamicModuleStatus {
    UNLOADED,
    LOADING,          // Startup in progress (cycle-detection sentinel)
    LOADED,
    FAILED,           // Startup threw or returned error
    UNLOADING,        // Marked for shutdown; dyn-shutdown thread will process
    SHUTDOWN_TIMEOUT, // Shutdown callback timed out; module may be in undefined state
    FAILED_SHUTDOWN   // Shutdown callback threw; module may be in undefined state
};
```

Cycle detection during `load_module` uses `LOADING` to detect recursive load.

---

## Sequence of operations

### Static module lifecycle

```mermaid
sequenceDiagram
    participant Main
    participant Guard as LifecycleGuard
    participant LM as LifecycleManager
    participant Mod as Module

    Main->>Guard: LifecycleGuard(Logger::GetLifecycleModule(), ...)
    Guard->>LM: RegisterModule(Logger)
    Guard->>LM: RegisterModule(FileLock)
    Guard->>LM: InitializeApp()
    LM->>LM: buildStaticGraph(), topologicalSort()
    LM->>Mod: startup() for each module
    Mod-->>LM: done
    LM-->>Guard: initialized
    Guard-->>Main: modules ready

    Note over Main: Application runs...

    Main->>Guard: destructor (scope exit)
    Guard->>LM: FinalizeApp()
    LM->>Mod: shutdown() reverse order
    Mod-->>LM: done
```

### Dynamic module load / unload

```mermaid
sequenceDiagram
    participant App
    participant LM as LifecycleManager
    participant PluginA
    participant PluginB

    Note over App: After InitializeApp()

    App->>LM: RegisterDynamicModule(PluginA)
    LM->>LM: Validate deps, add to graph
    App->>LM: LoadModule("PluginA")
    LM->>PluginA: startup()
    PluginA-->>LM: done

    App->>LM: RegisterDynamicModule(PluginB)
    App->>LM: LoadModule("PluginB")
    Note over LM: PluginB depends on PluginA
    LM->>LM: recalculateReferenceCounts()
    LM->>PluginB: startup()
    PluginB-->>LM: done

    App->>LM: UnloadModule("PluginB")
    LM->>PluginB: shutdown()
    LM->>LM: ref_count check, cascade unload
    App->>LM: UnloadModule("PluginA")
    LM->>PluginA: shutdown()
```

---

## Examples

### Static modules (LifecycleGuard + MakeModDefList)

```cpp
#include "plh_service.hpp"

int main() {
    pylabhub::utils::LifecycleGuard app_lifecycle(
        pylabhub::utils::MakeModDefList(
            pylabhub::utils::Logger::GetLifecycleModule(),
            pylabhub::utils::FileLock::GetLifecycleModule()
        )
    );

    LOGGER_INFO("Application started.");
    // ... application logic ...
    return 0;  // FinalizeApp() called automatically
}
```

### Dynamic module

```cpp
namespace MyPlugin {
    void startup(const char*) { LOGGER_INFO("MyPlugin started"); }
    void shutdown(const char*) { LOGGER_INFO("MyPlugin shut down"); }

    pylabhub::utils::ModuleDef GetLifecycleModule() {
        pylabhub::utils::ModuleDef def("MyPlugin");
        def.add_dependency("Logger");
        def.set_startup(startup);
        def.set_shutdown(shutdown, 1000);
        return def;
    }
}

int main() {
    pylabhub::utils::LifecycleGuard app_lifecycle(
        pylabhub::utils::Logger::GetLifecycleModule()
    );

    if (pylabhub::utils::RegisterDynamicModule(MyPlugin::GetLifecycleModule())) {
        if (pylabhub::utils::LoadModule("MyPlugin")) {
            // Use plugin...
            pylabhub::utils::UnloadModule("MyPlugin");
        }
    }
    return 0;
}
```

---

## Risk analysis and mitigations

| Risk | Mitigation |
|------|------------|
| Client forgets `unload_module()` | Resource leak for session; `finalize()` provides last-resort cleanup |
| Dependency loop between dynamic modules | `load_module` checks `LOADING` status to detect cycles |
| Callback calls `load_module`/`unload_module` | RecursionGuard detects and blocks (prevents deadlock) |
| Concurrent graph mutations | Internal mutex serializes all dynamic operations |
| Static object destructors after FinalizeApp | Documented: avoid destructors of static objects depending on lifecycle services |

---

## Copyright

This document is placed in the public domain or under the CC0-1.0-Universal license, whichever is more permissive.
