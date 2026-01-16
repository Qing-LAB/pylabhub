# Tech Draft: Revision of Dynamic Module Lifecycle Management

## 1. Problem Summary

The current implementation of dynamic module lifecycle management in `Lifecycle.cpp` has been identified as the source of a deadlock, observed during CTest runs (specifically `LifecycleDynamicTest.PermanentModuleIsNotUnloaded`).

The root cause is the reference counting strategy, which relies on simple incremental/decremental updates during the recursive `loadModuleInternal` and `unloadModuleInternal` calls. This approach has proven tobe brittle and stateful, leading to inconsistencies in the reference counts, especially in complex dependency scenarios involving "permanent" modules. These inconsistencies are the likely cause of the deadlock.

The existing logic for permanent modules, which was intended to stop the unload chain, is insufficient and contributes to the incorrect state management.

## 2. Proposed Solution: Global Recalculation

To fix this robustly, we will abandon the incremental/decremental approach and refactor the system to use a global, stateless recalculation of reference counts after every `load` or `unload` operation. This ensures the system's state is always consistent and derived from the ground truth of which modules are currently loaded.

### 2.1. New Core Function: `recalculateReferenceCounts()`

A new private helper method will be the cornerstone of this new design.

**Algorithm:**

1.  Reset the `ref_count` of all dynamic modules in the graph to `0`.
2.  Iterate through each currently `LOADED` dynamic module (the `source_node`).
3.  For each `source_node`, iterate through its **direct** dependencies.
4.  For each direct dependency (`dep_node`):
    -   If `dep_node` is a dynamic module and is **not** permanent, increment its `ref_count`.
    -   If `dep_node` is permanent, its own `ref_count` is not affected.

This algorithm correctly calculates `ref_count` as the number of direct, loaded modules that depend on a given module, which is a simpler and more robust definition.

### 2.2. New Unload Logic

The unload process will be changed to a recursive "unload, recalculate, check" cycle.

**Algorithm for `unloadModuleInternal(M)`:**

1.  The function is called on a module `M`. It immediately shuts down `M` and marks its status as `UNLOADED`.
2.  It then iterates through a copy of the direct dependencies of the just-unloaded module `M`.
3.  For each dependency `D` in the list:
    a. It triggers the global `recalculateReferenceCounts()` function.
    b. After the counts are updated, it checks if `D.ref_count` is now `0`.
    c. If the count is `0` (and `D` is not permanent), it recursively calls `unloadModuleInternal(D)`.

This ensures modules are unloaded in the correct order as their dependents are removed.

## 3. Implementation Steps

The following steps will be performed in a fresh session to implement the new design:

1.  **Restore `Lifecycle.cpp`**: Ensure the file is in its original, clean state before applying changes.
2.  **Add `recalculateReferenceCounts()`**: Add the new private helper function to `LifecycleManagerImpl` with the logic described in section 2.1.
3.  **Refactor `loadModuleInternal()`**: Remove all `ref_count` manipulation from this function. Its sole responsibility will be to recursively bring a module and its dependencies to the `LOADED` state.
4.  **Refactor `loadModule()`**: The public function will call the simplified `loadModuleInternal()` and then immediately call `recalculateReferenceCounts()` to ensure the graph state is consistent.
5.  **Refactor `unloadModuleInternal()`**: Replace the body of this function with the new recursive "unload-recalculate-check" algorithm described in section 2.2.
6.  **Refactor `unloadModule()`**: The public function will call the new `unloadModuleInternal()` to start the process, and then make one final call to `recalculateReferenceCounts()` to ensure the final state is correct.
7.  **Fix `buildStaticGraph()`**: A minor bug was found where the `is_permanent` flag was not being correctly passed for static modules. This will also be fixed.
8.  **Build and Test**: Compile the entire project and run the CTest suite to verify that the deadlock is resolved and all lifecycle tests pass.
