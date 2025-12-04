# pyLabHub C++ Build System: Final Architecture Summary

This document provides a definitive overview of the CMake build system designed for the pyLabHub C++ project. It outlines the core design principles, the overall structure, and the conventions developers should follow.

## 1. Core Design Principles

Our architecture is built on a foundation of modern CMake practices, emphasizing **clarity, robustness, and maintainability**.

*   **Unified Staging**: The cornerstone of the design is the unified staging directory (`${PYLABHUB_STAGING_DIR}`). All build artifacts—executables, libraries, headers, and bundles—are copied into this single location within the build tree. This creates a self-contained, runnable version of the project that mirrors the final installation layout, making local development and testing simple and reliable.
*   **Isolation & Abstraction**: Each third-party dependency is configured in a "sandbox" using a `snapshot_cache_var`/`restore_cache_var` mechanism to prevent its build settings from "leaking" and affecting other parts of the build. Consumers of these dependencies link against stable, namespaced `ALIAS` targets (e.g., `pylabhub::third_party::fmt`), completely abstracting away the underlying build details.
*   **Top-Down Control & Modularity**: The build system is broken down into logical, single-responsibility modules (`cmake/`, `third_party/`, `src/`, `tests/`). High-level build policies are defined as user-facing `CACHE` options (e.g., `BUILD_TESTS`, `THIRD_PARTY_INSTALL`), which are then interpreted by the appropriate modules. This provides a clear, centralized API for developers to tune the build.
*   **Clear Separation of Concerns**: We have established a clear hierarchy of components:
    *   **Core Components** (`pylabhub-corelib`, `pylabhub-shell`, `IgorXOP`): These are integral to the project and are **always** staged.
    *   **Third-Party Libraries**: These are external dependencies whose staging is controlled by the `THIRD_PARTY_INSTALL` option.
    *   **Optional Components** (`tests`): The building and staging of these components are controlled by their own dedicated options (`BUILD_TESTS`).
    *   **Installation**: The final `install` target is optional and controlled by `PYLABHUB_CREATE_INSTALL_TARGET`, providing a clean separation between building for development and creating a distributable package.

## 2. Build System Structure

The project is organized into a clear hierarchy, with each component playing a specific role:

*   **Top-Level (`/cpp/`)**
    *   `CMakeLists.txt`: The main orchestrator. It sets up the project, defines the staging infrastructure (`PYLABHUB_STAGING_DIR`, `stage_all` target), includes helper modules, and adds the subdirectories for third-party, source, and test builds. It also contains the final, optional `install()` rule.
    *   `cmake/`: Contains helper modules that define the global build environment:
        *   `PlatformAndCompiler.cmake`: Centralizes platform detection and global compiler flags.
        *   `ToplevelOptions.cmake`: Defines all primary user-facing options (`BUILD_TESTS`, `PYLABHUB_CREATE_INSTALL_TARGET`, etc.).
        *   `StageHelpers.cmake`: Provides the `pylabhub_stage_*` helper functions used by component scripts.

*   **Third-Party Dependencies (`/cpp/third_party/`)**
    *   This is a self-contained framework for managing all external dependencies.
    *   `CMakeLists.txt`: The entry point for the third-party system. It includes the core policy helper and then includes each individual library wrapper script. It defines the `stage_third_party_deps` target and makes the global `stage_all` target depend on it.
    *   `cmake/`: Contains the logic for the framework, including `ThirdPartyPolicyAndHelper.cmake` (which defines tunable options and helper functions) and individual `<lib>.cmake` wrapper scripts for each dependency.

*   **Source Code (`/cpp/src/`)**
    *   Contains the project's own source code and build scripts.
    *   `CMakeLists.txt` files within `src` (and its subdirectories like `IgorXOP`) define the project's own targets. They are pure consumers of the dependency targets provided by the `third_party` framework, linking against the stable `pylabhub::third_party::*` aliases. They also contain the `POST_BUILD` commands to stage their artifacts unconditionally.

*   **Test Suite (`/cpp/tests/`)**
    *   Contains the project's test suite. The `CMakeLists.txt` here defines the test executables, links them against `pylabhub::corelib`, and conditionally stages them based on the `PYLABHUB_CREATE_INSTALL_TARGET` option.

## 3. Naming Conventions

A consistent naming scheme is used throughout the project to ensure clarity:

*   **Targets**:
    *   **Third-Party Aliases**: `pylabhub::third_party::<name>` (e.g., `pylabhub::third_party::zmq`). This is the stable, namespaced alias for all internal project code to use.
    *   **Internal Targets**: `pylabhub::<name>` (e.g., `pylabhub::corelib`, `pylabhub::shell`).
*   **Variables & Options**:
    *   **Global Policies**: `PYLABHUB_*` or `BUILD_*` (e.g., `PYLABHUB_CREATE_INSTALL_TARGET`, `BUILD_TESTS`).
    *   **Third-Party Policies**: `THIRD_PARTY_*` (e.g., `THIRD_PARTY_INSTALL`).
    *   **Internal/Temporary**: Prefixed with an underscore (`_`) to denote them as local to the current scope.

## 4. Expected Behaviors & Workflow

The build system is now predictable and easy to use for various development scenarios, as documented in the top-level `CMakeLists.txt`.

*   **Development**: A simple `cmake -S . -B build && cmake --build build` will configure and build the entire project, placing all core artifacts in the `build/stage/` directory for immediate use and testing.
*   **Testing**: Running `ctest` from the build directory will execute all tests defined via `add_test()`.
*   **Packaging/Installation**: To create a full, distributable package, a developer enables all relevant options (`-DTHIRD_PARTY_INSTALL=ON`, `-DPYLABHUB_CREATE_INSTALL_TARGET=ON`) and runs `cmake --install`.
