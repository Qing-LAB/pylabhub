# Technical Design: LifecycleManager Hybrid (Static & Dynamic) Model

## 1. Introduction and Goal

The `LifecycleManager` is a foundational component responsible for the ordered startup and shutdown of application modules. This document details its architecture, which supports two distinct module types: **Static** and **Dynamic**.

The goal is to provide a robust framework for managing both critical, application-wide services (static modules) and optional, on-demand features or plugins (dynamic modules) within a single, unified system.

---

## 2. The Static Module Lifecycle (Existing System)

The core of the `LifecycleManager` is its system for managing static modules, which are critical for the application's basic functionality.

### 2.1. Core Principles
- **Fatal on Failure**: Static modules represent the application's core. Any failure during their dependency resolution or startup is considered a catastrophic error, and the application will immediately and loudly abort.
- **Register Before Init**: All static modules must be registered *before* the main application lifecycle begins.
- **Single Init/Shutdown**: Static modules are initialized once when the application starts and shut down once when it exits.

### 2.2. Implementation Details
- **`InternalGraphNode`**: The central `LifecycleManagerImpl` contains a dependency graph (`m_module_graph`) composed of `InternalGraphNode` objects. Each node contains the module's definition, callbacks, and dependency-tracking fields (`dependents`).
- **`buildStaticGraph()`**: Called once during `initialize()`, this function populates `m_module_graph` from a temporary list of pre-registered static modules.
- **`topologicalSort()`**: This function uses Kahn's algorithm on the graph of static modules to produce a deterministic startup order.

---

## 3. The Need for a Dynamic Lifecycle

The rigid, "all-or-nothing" static model is unsuitable for optional components. A dynamic lifecycle is needed for:
- **Optional, Resource-Heavy Features**: E.g., a data analysis toolkit that should only consume memory when in use.
- **Plugin Systems**: Allowing users to enable or disable extensions without an application restart.
- **Mode-Specific Functionality**: Activating different sets of tools for different application modes (e.g., "Editing" vs. "Playback").

---

## 4. The Hybrid Lifecycle Model (Proposed Framework)

The proposed solution extends the `LifecycleManager` with a fully integrated system for managing dynamic modules. This design rigorously maintains the existing architectural constraints to ensure ABI stability and predictable behavior.

### 4.1. Architectural Constraints (Singleton & ABI Stability)
The entire design and implementation of the dynamic module system adheres strictly to the existing architectural principles of the `LifecycleManager`. These principles are fundamental for code review and future maintenance:

-   **Pimpl Idiom / ABI Stability**: All new state and functionality for dynamic modules are encapsulated within the private `LifecycleManagerImpl` class and its internal structs (e.g., `InternalGraphNode`). The public `LifecycleManager` header remains ABI-stable, ensuring binary compatibility for existing consumers.
-   **Singleton Pattern**: The `LifecycleManager` remains a singleton. All new public methods for dynamic module management are exposed through the existing `LifecycleManager::instance()` accessor, maintaining a single, global point of control for the entire application lifecycle.

### 4.2. Core Principles
- **Unified Dependency Graph**: All modules, static and dynamic, reside in a **single, unified dependency graph**. A flag on each node (`is_dynamic`) distinguishes its type. This reuses the existing graph infrastructure, ensuring all dependency logic is centralized and consistent.
- **Graceful Failure**: Unlike static module initialization (which is fatal on failure), any error during a dynamic module's loading process (e.g., an unmet dependency, or an exception in its startup callback) is **not fatal**. The corresponding `LoadModule()` call will return `false`, and an error will be logged, allowing the application to handle the failure gracefully.
- **Strict Ordering**: Dynamic modules can only be registered or loaded *after* the static application core has been fully initialized. This ensures a stable foundation for all dynamic components.
- **Reference Counting**: To manage shared dependencies between dynamic modules, a reference counting system is used. A module is only fully unloaded when its reference count drops to zero.
- **Dependency Rules**:
    - Dynamic modules can depend on static modules.
    - Static modules **cannot** depend on dynamic modules. This is enforced at initialization.

### 4.3. Detailed Component and State Modifications
- **`InternalGraphNode` Enhancement**: The `InternalGraphNode` struct is enhanced with fields for dynamic state:
  ```cpp
  struct InternalGraphNode {
      // ... existing static fields ...
      bool is_dynamic = false;
      DynamicModuleStatus dynamic_status = DynamicModuleStatus::UNLOADED;
      int ref_count = 0;
  };
  ```
- **`DynamicModuleStatus` Enum**: A new enum tracks the runtime state of dynamic modules: `UNLOADED`, `LOADING`, `LOADED`, `FAILED`.
- **`m_graph_mutation_mutex`**: A new mutex is added to protect the `m_module_graph` against data races from concurrent dynamic module operations.

### 4.4. Detailed Code Flows
- **Static Registration (`register_module`)**:
    - **Timing**: Must be called *before* `initialize()`.
    - **Logic**: Adds the module definition to a temporary list. Will `PLH_PANIC` if called after initialization.
- **Dynamic Registration (`register_dynamic_module`)**:
    - **Timing**: Must be called *after* `initialize()` and *before* `finalize()`.
    - **Logic**:
        1. Checks that `initialize()` has completed (gracefully returns `false` if not).
        2. Locks `m_graph_mutation_mutex`.
        3. Validates that the module name is unique and all its dependencies exist in the graph.
        4. Adds a new node to `m_module_graph` and connects it to its dependencies.
- **Dynamic Loading (`load_module`)**:
    - **Timing**: Must be called *after* `initialize()` has completed.
    - **Logic**: `PLH_PANIC`s if called before initialization. Otherwise, it uses a `RecursionGuard`, locks the graph, and calls a recursive helper that:
        1. Checks the module's `dynamic_status` (handles `LOADED`, `LOADING`, `FAILED` states).
        2. Sets status to `LOADING`.
        3. Validates that all static dependencies are `Started`.
        4. Recursively calls `loadModuleInternal` for all dynamic dependencies.
        5. Runs the module's `startup()` callback.
        6. On success, sets status to `LOADED`, sets `ref_count` to 1, and increments the `ref_count` of its dynamic dependencies.
        7. On any failure, sets status to `FAILED` and returns `false`.
- **Dynamic Unloading (`unload_module`)**:
    - **Logic**: Uses a `RecursionGuard`, locks the graph, and calls a recursive helper that decrements the module's `ref_count`. If the count reaches zero, it runs the `shutdown()` callback and then recurses to unload its dependencies.
- **Finalization (`finalize`)**:
    - **Logic**: First, it finds all loaded dynamic modules, topologically sorts them, and runs their `shutdown()` callbacks in the correct reverse dependency order. Then, it proceeds with the existing shutdown logic for static modules.

---

## 5. Risk Analysis and Mitigations

- **Leaked Modules**: A client forgetting to call `unload_module()` will cause a resource leak for the application's session.
    - **Mitigation**: This is a matter of API discipline. The automatic cleanup during `finalize` provides a last-resort safety net.
- **Circular Dependencies**: A dependency loop between dynamic modules would cause infinite recursion.
    - **Mitigation**: The `load_module` logic explicitly checks for a `LOADING` status to detect and break cycles.
- **Re-entrant Calls**: A module's callback must not call `load_module` or `unload_module`, as this would cause a deadlock.
    - **Mitigation**: The public `load_module` and `unload_module` functions use `RecursionGuard` to detect and block such calls.
- **Thread Safety**: Concurrent calls to `register_dynamic_module`, `load_module`, and `unload_module` could corrupt the graph.
    - **Mitigation**: All runtime graph operations are protected by a single `m_graph_mutation_mutex`, ensuring serial access.