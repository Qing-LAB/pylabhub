| Property       | Value                                        |
| -------------- | -------------------------------------------- |
| **HEP**        | `core-0001`                                  |
| **Title**      | A Hybrid (Static & Dynamic) Module Lifecycle |
| **Author**     | Quan Qing <quan.qing@asu.edu>                |
| **Status**     | Draft                                        |
| **Category**   | Core                                         |
| **Created**    | 2026-01-05                                   |
| **C++-Standard** | C++20                                        |

## Abstract

This Hub Enhancement Proposal (HEP) details a technical design for the `LifecycleManager` that supports two distinct module types: **Static** and **Dynamic**. The goal is to provide a robust framework for managing both critical, application-wide services (static modules) and optional, on-demand features or plugins (dynamic modules) within a single, unified dependency management system.

## Motivation

The existing `LifecycleManager` provides a robust "all-or-nothing" system for static modules, which is ideal for the application's core functional units. However, this rigid model is unsuitable for several key use cases:
-   **Optional, Resource-Heavy Features**: E.g., a data analysis toolkit that should only consume memory when explicitly activated by the user.
-   **Plugin Systems**: Allowing third parties to develop extensions or for users to enable/disable features without an application restart.
-   **Mode-Specific Functionality**: Activating different sets of tools for different application modes (e.g., "Editing" vs. "Playback").

A dynamic lifecycle is required to address these needs, allowing modules to be loaded and unloaded at runtime gracefully.

## Rationale and Design

The proposed solution extends the `LifecycleManager` with a fully integrated system for managing dynamic modules. This design rigorously maintains the existing architectural constraints to ensure ABI stability and predictable behavior.

### Architectural Constraints

The design and implementation of the dynamic module system adhere strictly to the existing architectural principles of the `LifecycleManager`:

-   **Pimpl Idiom / ABI Stability**: All new state and functionality for dynamic modules are encapsulated within the private `LifecycleManagerImpl` class. The public `LifecycleManager` header remains ABI-stable, ensuring binary compatibility for existing consumers.
-   **Singleton Pattern**: The `LifecycleManager` remains a singleton, providing a single, global point of control for the entire application lifecycle via the `LifecycleManager::instance()` accessor.

### Core Principles of the Hybrid Model

-   **Unified Dependency Graph**: All modules, static and dynamic, reside in a **single, unified dependency graph**. A flag on each node (`is_dynamic`) distinguishes its type. This reuses the existing graph infrastructure, ensuring all dependency logic is centralized and consistent.
-   **Graceful Failure**: Unlike static module initialization (which is fatal on failure), any error during a dynamic module's loading process (e.g., an unmet dependency or an exception in its startup callback) is **not fatal**. The corresponding `load_module()` call will return `false`, and an error will be logged, allowing the application to handle the failure gracefully.
-   **Strict Ordering**: Dynamic modules can only be registered or loaded *after* the static application core has been fully initialized. This ensures a stable foundation for all dynamic components.
-   **Reference Counting**: To manage shared dependencies between dynamic modules, a reference counting system is used. A module is only fully unloaded when its reference count drops to zero.
-   **Dependency Rules**:
    -   Dynamic modules can depend on static modules.
    -   Static modules **cannot** depend on dynamic modules. This is enforced at initialization.

## Specification

### `InternalGraphNode` Enhancement
The `InternalGraphNode` struct is enhanced with fields to track dynamic state:

```cpp
struct InternalGraphNode {
    // ... existing static fields ...
    bool is_dynamic = false;
    DynamicModuleStatus dynamic_status = DynamicModuleStatus::UNLOADED;
    int ref_count = 0;
};
```

### `DynamicModuleStatus` Enum
A new enum tracks the runtime state of dynamic modules:

```cpp
enum class DynamicModuleStatus {
    UNLOADED,
    LOADING,
    LOADED,
    FAILED
};
```

### Thread Safety
A new mutex, `m_graph_mutation_mutex`, is added to the `LifecycleManagerImpl` to protect the `m_module_graph` against data races from concurrent dynamic module operations.

### API and Behavior
-   **Static Registration (`register_module`)**:
    -   **Timing**: Must be called *before* `initialize()`.
    -   **Logic**: Adds the module definition to a temporary list. Will `PLH_PANIC` if called after initialization.
-   **Dynamic Registration (`register_dynamic_module`)**:
    -   **Timing**: Must be called *after* `initialize()` and *before* `finalize()`.
    -   **Logic**: Locks the graph mutation mutex, validates uniqueness and dependencies, adds a new node to `m_module_graph`, and connects it to its dependencies. Returns `true` on success or `false` on failure.
-   **Dynamic Loading (`load_module`)**:
    -   **Timing**: Must be called *after* `initialize()` has completed.
    -   **Logic**: `PLH_PANIC`s if called before initialization. Otherwise, it uses a `RecursionGuard`, locks the graph, and recursively loads the module and its dependencies. It validates that all static dependencies are started, runs the module's `startup()` callback, and on success, sets the module's status to `LOADED` and increments reference counts for all its dynamic dependencies. On any failure, it sets the status to `FAILED` and returns `false`.
-   **Dynamic Unloading (`unload_module`)**:
    -   **Logic**: Uses a `RecursionGuard` and locks the graph. This operation only succeeds if the module's `ref_count` is zero, meaning no other currently loaded dynamic modules depend on it. If successful, it runs the module's `shutdown()` callback. It then recursively unloads any of its dynamic dependencies that now have a zero `ref_count` and are no longer required by any other loaded modules. Finally, the module and any recursively unloaded dependencies are completely de-registered from the `LifecycleManager` (removed from the dependency graph). To re-load a module after it has been unloaded, it must first be re-registered via `register_dynamic_module()`. The function returns `false` if the module cannot be unloaded because its `ref_count` is greater than zero.
-   **Finalization (`finalize`)**:
    -   **Logic**: First, it finds all loaded dynamic modules, topologically sorts them, and runs their `shutdown()` callbacks in the correct reverse dependency order. It then proceeds with the existing shutdown logic for static modules.

## Risk Analysis and Mitigations

-   **Risk**: A client forgetting to call `unload_module()` will cause a resource leak for the application's session.
    -   **Mitigation**: This is a matter of API discipline. The automatic cleanup during `finalize()` provides a last-resort safety net.
-   **Risk**: A dependency loop between dynamic modules would cause infinite recursion.
    -   **Mitigation**: The `load_module` logic explicitly checks for a `LOADING` status to detect and break cycles.
-   **Risk**: A module's callback must not call `load_module` or `unload_module`.
    -   **Mitigation**: The public API methods use `RecursionGuard` to detect and block such re-entrant calls, which would otherwise deadlock.
-   **Risk**: Concurrent calls to `register_dynamic_module`, `load_module`, and `unload_module` could corrupt the graph.
    -   **Mitigation**: All runtime graph operations are protected by `m_graph_mutation_mutex`, ensuring serial access.

## Copyright

This document is placed in the public domain or under the CC0-1.0-Universal license, whichever is more permissive.
