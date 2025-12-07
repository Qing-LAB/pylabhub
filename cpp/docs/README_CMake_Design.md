# pyLabHub C++ Build System: Final Architecture Summary

This document provides a definitive overview of the CMake build system designed for the pyLabHub C++ project. It outlines the core design principles, the overall structure, and the conventions developers should follow.

## 1. Core Design Principles

Our architecture is built on a foundation of modern CMake practices, emphasizing **clarity, robustness, and maintainability**. The key principles are:

*   **Separation of Build and Stage**: The system makes a clear distinction between *building* artifacts and *staging* them.
    *   **Build**: A standard build (`cmake --build .`) compiles sources and produces artifacts (libraries, executables) in the CMake build tree (`build/src/`, etc.). It does **not** copy them to the staging area. Assembly of complex artifacts (like the macOS `.xop` bundle) may happen as a `POST_BUILD` step, but the result remains within the build tree, ready for staging.
    *   **Stage**: A dedicated target, `stage_all`, is responsible for populating the staging directory. Running `cmake --build . --target stage_all` executes all necessary copy operations, creating a complete, runnable layout of the project.

*   **Unified Staging**: The cornerstone of the design is the unified staging directory (`${PYLABHUB_STAGING_DIR}`). After a successful build, running the `stage_all` target copies all artifacts—executables, libraries, headers, and bundles—into this single location. This creates a self-contained, runnable version of the project that mirrors the final installation layout, making local development and testing simple and reliable.

*   **Isolation & Abstraction**: Each third-party dependency is configured in a "sandbox" using a `snapshot_cache_var`/`restore_cache_var` mechanism to prevent its build settings from "leaking" and affecting other parts of the build. Consumers of these dependencies link against stable, namespaced `ALIAS` targets (e.g., `pylabhub::third_party::fmt`), completely abstracting away the underlying build details.

*   **Top-Down Control & Modularity**: The build system is broken down into logical, single-responsibility modules (`cmake/`, `third_party/`, `src/`, `tests/`). High-level build policies are defined as user-facing `CACHE` options (e.g., `BUILD_TESTS`, `THIRD_PARTY_INSTALL`), which are then interpreted by the appropriate modules. This provides a clear, centralized API for developers to tune the build.

*   **Clear Separation of Concerns**: We have established a clear hierarchy of components:
    *   **Core Components** (`pylabhub::utils`, `pylabhub::core`, `pylabhub::app`): These are integral to the project and are **always** built. Their artifacts are staged by the `stage_core_artifacts` target.
    *   **Third-Party Libraries**: These are external dependencies whose staging is controlled by the `THIRD_PARTY_INSTALL` option and handled by the `stage_third_party_deps` target.
    *   **Optional Components** (`tests`, `IgorXOP`): The building and staging of these components are controlled by their own dedicated options (`BUILD_TESTS` and `BUILD_XOP`, respectively).
*   **Installation**: The final `install` target is optional and controlled by `PYLABHUB_CREATE_INSTALL_TARGET`. It performs a direct copy of the staged directory, providing a clean separation between building for development and creating a distributable package.

## 2. Build System Structure & Component Layout

The project is organized into a clear hierarchy, with each component playing a specific role:

*   **Top-Level (`/cpp/`)**
    *   `CMakeLists.txt`: The main orchestrator. It sets up the project, defines the staging infrastructure (`PYLABHUB_STAGING_DIR`, `stage_all` target), includes helper modules, and adds the subdirectories for third-party, source, and test builds. It also contains the final, optional `install()` rule.
    *   `cmake/`: Contains helper modules that define the global build environment:
        *   `PlatformAndCompiler.cmake`: Centralizes platform detection and global compiler flags.
        *   `ToplevelOptions.cmake`: Defines all primary user-facing options (`BUILD_TESTS`, `PYLABHUB_CREATE_INSTALL_TARGET`, etc.).
        *   `StageHelpers.cmake`: Provides a consistent API for staging artifacts. Key functions include `pylabhub_stage_executable`, `pylabhub_stage_libraries`, `pylabhub_stage_headers`, and the lower-level `pylabhub_get_library_staging_commands`.

*   **Third-Party Dependencies (`/cpp/third_party/`)**
    *   This is a self-contained framework for managing all external dependencies.
    *   `CMakeLists.txt`: The entry point for the third-party system. It includes the core policy helper and then includes each individual library wrapper script. It defines the `stage_third_party_deps` target and makes the global `stage_all` target depend on it.
    *   `cmake/`: Contains the logic for the framework, including `ThirdPartyPolicyAndHelper.cmake` (which defines tunable options and helper functions) and individual `<lib>.cmake` wrapper scripts for each dependency.

*   **Source Code (`/cpp/src/`)**
    *   Contains the project's own source code and build scripts.
    *   The source is organized into several components defined in `CMakeLists.txt` files within `src` and its subdirectories:
        *   A shared utility library (`pylabhub::utils`) is built from the `src/utils` directory.
        *   A static core library (`pylabhub::core`) and a main executable (`pylabhub::app`) are built from the remaining sources.
    *   **Source File Management**: The project uses a hybrid approach. Core components like `pylabhub::utils` explicitly list their source files for maximum build robustness. Other components, like `pylabhub::core`, may use `file(GLOB_RECURSE)` for convenience. While pragmatic, this requires developers to manually re-run CMake when adding or removing source files.
    *   **Staging and Dependencies**: These targets are pure consumers of the dependency targets provided by the `third_party` framework, linking against the stable `pylabhub::third_party::*` aliases. They use the staging helper functions to schedule their artifacts to be copied to the staging directory. For example, `pylabhub_stage_executable` is used for `pylabhub::app`, and `pylabhub_get_library_staging_commands` is used for `pylabhub::core` and `pylabhub::utils`. These commands are attached to the `stage_core_artifacts` target, ensuring that staging only occurs when `stage_all` is explicitly built.

*   **Igor Pro XOP (`/cpp/src/IgorXOP/`)**
    *   This optional component, controlled by the `BUILD_XOP` option, builds a macOS bundle (`.xop`) for integration with Igor Pro.
    *   Its `CMakeLists.txt` handles the specific requirements of bundle creation, including setting bundle properties and packaging resources. The resulting `.xop` bundle is staged to the appropriate location by the `stage_all` target.

*   **Test Suite (`/cpp/tests/`)**
    *   Contains the project's test suite. The `CMakeLists.txt` here defines the test executables and links them against the project's internal libraries (e.g., `pylabhub::core`).
    *   If `BUILD_TESTS` is `ON`, a `stage_tests` target is created to copy the test executables to the staging directory's `bin` folder. This ensures they can find runtime dependencies (like the `pylabhub-utils` shared library) which are also staged there.

## 3. Naming Conventions

A consistent naming scheme is used throughout the project to ensure clarity:

*   **Targets**:
    *   **Third-Party Aliases**: `pylabhub::third_party::<name>` (e.g., `pylabhub::third_party::zmq`). This is the stable, namespaced alias for all internal project code to use.
    *   **Internal Targets**: `pylabhub::<name>` (e.g., `pylabhub::core`, `pylabhub::utils`).
*   **Variables & Options**: The system uses a clear prefixing scheme to denote the scope and purpose of variables.
    *   **Global Policies**: `PYLABHUB_*` or `BUILD_*` (e.g., `PYLABHUB_CREATE_INSTALL_TARGET`, `BUILD_TESTS`).
    *   **Third-Party Policies**: `THIRD_PARTY_*` (e.g., `THIRD_PARTY_INSTALL`).
    *   **Internal/Temporary**: Prefixed with an underscore (`_`) to denote them as local to the current scope.

## 4. Expected Behaviors & Workflow

The build system is predictable and easy to use for various development scenarios. The key distinction is between building and staging.

*   **Build**: A simple `cmake -S . -B build && cmake --build build` will configure and build all enabled project targets. Artifacts are created in the build tree but **not** copied to the staging area.
*   **Stage**: After a successful build, run `cmake --build build --target stage_all`. This will populate the `build/stage/` directory with all libraries, executables, headers, and resources, creating a complete, runnable package.
*   **Testing**: After staging, you can run executables directly from `build/stage/bin/`. To run the test suite, run `ctest --test-dir build`. CTest is configured to run the executables from their staged location.
*   **Packaging/Installation**: To create a full, distributable package, enable all relevant options (`-DTHIRD_PARTY_INSTALL=ON`, `-DPYLABHUB_CREATE_INSTALL_TARGET=ON`) and run `cmake --install build` after a successful staging step.
