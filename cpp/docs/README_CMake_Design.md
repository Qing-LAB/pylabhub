# pyLabHub C++ Build System: Architecture and Developer's Guide

This document provides a definitive overview of the CMake build system for the pyLabHub C++ project. It outlines the core design principles and includes a practical guide for developers to perform common tasks.

## 1. Core Design Principles

Our architecture is built on modern CMake practices, emphasizing **clarity, robustness, and maintainability**. The key pillars of the design are detailed below.

### 1.1. Unified Staging Architecture

The cornerstone of the design is the **unified staging directory**. All build artifacts—executables, libraries, headers, bundles, etc.—are copied into this single location within the build directory. This creates a self-contained, runnable version of the project that mirrors the final installation layout, making local development and testing simple and reliable.

*   **Staging Directory Naming**: The staging directory name consistently includes the build configuration (e.g., `build/stage-debug`, `build/stage-release`). This allows artifacts from different build types to coexist without conflicts. The root is defined by the `PYLABHUB_STAGING_DIR` variable.

*   **Installation via Staging**: The final `install` step is a direct copy of the fully-populated staging directory. This provides a clean separation between development builds and distributable packages. To ensure correctness, the installation process is protected by a pre-install check that verifies the staging process has completed successfully.

*   **Orchestrated Staging Targets**: The staging process is controlled by a hierarchy of custom targets. The master `stage_all` target depends on aggregator targets like `stage_core_artifacts` and `stage_third_party_deps`. A foundational target, `create_staging_dirs`, ensures the directory structure is created before any files are copied, preventing race conditions in parallel builds.

### 1.2. Two Staging Patterns

To handle the different complexities of internal and third-party dependencies, the build system employs two distinct but complementary staging patterns.

#### Pattern A: Direct Staging (for Internal Executables)

This modern, property-based approach is used for the project's native executables.

1.  **How it Works**: The helper function `pylabhub_stage_executable` is called for the target. This function directly sets the `RUNTIME_OUTPUT_DIRECTORY` property on the target.
2.  **Result**: CMake builds the artifact directly into the desired subdirectory within the staging area (e.g., `stage-debug/bin`). No separate copy step is needed.
3.  **Dependency**: To ensure correct build ordering, the target itself (e.g., `pylabhub-hubshell`) is registered to the `CORE_STAGE_TARGETS` global property, making it a dependency of the `stage_core_artifacts` aggregator target.

#### Pattern B: Registration-Based Staging (for All Libraries)

This pattern provides a unified, robust API for staging artifacts from both complex internal libraries and all third-party dependencies, especially those whose outputs may not be known at configure time (e.g., built with `ExternalProject_Add`).

1.  **Registration**: A component's build script calls `pylabhub_register_library_for_staging` or `pylabhub_register_headers_for_staging`. This records the staging request in a global property.
2.  **Processing**: At the end of the `third_party/CMakeLists.txt` configuration, a loop iterates through all registrations.
3.  **Execution**: For each registration, a helper function (e.g., `pylabhub_attach_library_staging_commands` or `pylabhub_attach_headers_staging_commands`) attaches a `POST_BUILD` custom command to the `stage_third_party_deps` target. This command performs the actual file copying at build time. This applies equally to both library and header staging.
4.  **Result**: This decouples the declaration of "what to stage" from the execution of "how to stage," and correctly handles dependencies on non-native CMake targets. This is the required pattern for all libraries, internal or third-party.

### 1.3. Modular & Stable Target Interfaces

*   **Internal Libraries**: The project's main internal library is `pylabhub::utils`, a shared library for high-level utilities.
*   **Alias Targets**: Consumers **must** link against namespaced `ALIAS` targets (e.g., `pylabhub::utils`, `pylabhub::third_party::fmt`) rather than raw target names. This provides a stable public API for all dependencies and allows the underlying implementation targets to be modified without breaking consumer code.
*   **Third-Party Isolation**: Third-party dependencies are configured in isolated scopes using wrapper scripts in `third_party/cmake/`. This prevents their build options from "leaking" and affecting other parts of the project, thanks to the `snapshot_cache_var`/`restore_cache_var` helpers.

### 1.4. Prerequisite Build System

Some third-party libraries, like `libsodium`, are not built with CMake. To handle this, we use a prerequisite build mechanism:
1.  **ExternalProject_Add**: `libsodium` is built using `ExternalProject_Add`, which can run external build commands like `make`.
2.  **Prerequisite Install Directory**: The library is installed into a sandboxed directory within the build tree (`build/prereqs`).
3.  **Master Prerequisite Target**: A master custom target, `build_prerequisites`, depends on all such external projects.
4.  **Imported Target**: A stable `IMPORTED` target (`pylabhub::third_party::sodium`) is created that points to the artifacts in the prerequisite install directory.
5.  **Dependency Chaining**: Any other library (like `libzmq`) that depends on `libsodium` links against this `IMPORTED` target and adds a dependency on `build_prerequisites` to ensure the correct build order.

## 2. System Diagrams

### Internal Project Dependencies

This diagram illustrates how the main application and internal libraries depend on each other and on third-party libraries. The nodes represent **CMake alias targets**.

```mermaid
graph TD
    subgraph "Executable Target"
        A[pylabhub::hubshell]
    end
    
    subgraph "Internal Shared Library"
        B(pylabhub::utils)
    end
    
    subgraph "Third-Party Libraries"
        C(pylabhub::third_party::fmt)
        D(pylabhub::third_party::cppzmq)
        E(pylabhub::third_party::nlohmann_json)
    end
    
    A --> B;
    B --> C;
    B --> D;
    B --> E;

    style A fill:#D5F5E3
    style B fill:#E6F3FF,stroke:#66a3ff,stroke-width:2px
    style C fill:#FFF5E6,stroke:#FFC300,stroke-width:2px
    style D fill:#FFF5E6,stroke:#FFC300,stroke-width:2px
    style E fill:#FFF5E6,stroke:#FFC300,stroke-width:2px
```

### Staging Target Dependencies

The `stage_all` target orchestrates several smaller, modular staging targets. This diagram clarifies how the two different staging patterns feed into the aggregator targets.

```mermaid
graph TD
    subgraph "Global Master Target"
        stage_all
    end
    
    subgraph "Aggregator Targets"
        stage_core_artifacts
        stage_third_party_deps
        stage_tests
    end
    
    subgraph "Infrastructure Target"
      create_staging_dirs
    end

    %% Dependencies between aggregators
    stage_all --> stage_core_artifacts & stage_tests & stage_pylabhubxop

    stage_core_artifacts --> stage_third_party_deps

    %% All aggregators depend on the base directory structure
    stage_core_artifacts --> create_staging_dirs
    stage_third_party_deps --> create_staging_dirs
    stage_tests --> create_staging_dirs
    stage_pylabhubxop --> create_staging_dirs

    subgraph "Pattern A: Direct Staging"
      A["pylabhub-hubshell (target)"]
    end
    
    subgraph "Pattern B: Registration-Based Staging"
        subgraph "Internal Libs"
            B[stage_pylabhub_utils]
            D[stage_pylabhubxop]
        end
        subgraph "Third-Party Libs"
            C["... (via POST_BUILD commands on stage_third_party_deps)"]
        end
    end

    %% Wiring patterns to aggregators
    stage_core_artifacts -- depends on --- A & B
    stage_third_party_deps -- "is populated by" --- C
    B -- depends on --- B_lib["pylabhub-utils (target)"]
    D -- depends on --- D_lib["pylabhubxop (target)"]
```

## 3. Developer's Cookbook: Common Tasks

This section provides practical recipes for common development tasks.

### Recipe 1: How to Add a New Add-On Executable

This recipe uses the **Direct Staging** pattern, suitable for simple, native executables.

1.  **Create the source file and `CMakeLists.txt` in the `add-ons` directory.**
2.  **Edit `add-ons/my-tool/CMakeLists.txt`:**
    ```cmake
    # add-ons/my-tool/CMakeLists.txt
    add_executable(my-tool main.cpp)
    add_executable(pylabhub::my-tool ALIAS my-tool)
    target_link_libraries(my-tool PRIVATE pylabhub::utils)

    # --- Staging (Pattern A: Direct) ---
    pylabhub_stage_executable(TARGET my-tool DESTINATION bin)
    set_property(GLOBAL APPEND PROPERTY CORE_STAGE_TARGETS my-tool)
    ```
3.  **Include the new subdirectory in `add-ons/CMakeLists.txt`:**
    ```cmake
    add_subdirectory(my-tool)
    ```

### Recipe 2: How to Add a New Internal Library

This recipe shows the general structure for an internal library, which must use the **Registration-Based Staging** pattern. This example is for a shared library.

1.  **Create directory and files in `src/`.** Ensure source headers (e.g., `my-lib.h`) are placed in `src/include/my-lib/` if they are intended for global staging by the `stage_project_source_headers` target.
2.  **Edit `src/my-lib/CMakeLists.txt`:**
    ```cmake
    add_library(my-lib SHARED my-lib.cpp)
    add_library(pylabhub::my-lib ALIAS my-lib)
    
    # Use CMake's feature for handling DLL exports/imports (important for shared libs)
    include(GenerateExportHeader)
    generate_export_header(my-lib BASE_NAME "my_lib" EXPORT_MACRO_NAME "MY_LIB_EXPORT")
    
    # Make the generated export header discoverable for build
    target_include_directories(my-lib PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>)

    # --- Staging (Pattern B: Registration) ---
    # 1. Create a local custom target that aggregates all staging commands for this library.
    add_custom_target(stage_my-lib COMMENT "Staging my-lib artifacts")
    add_dependencies(stage_my-lib my-lib) # Ensure the library itself is built first

    # 2. Add commands to stage the generated export header.
    # Note: Primary source headers in src/include are staged globally by the root CMakeLists.txt.
    # This section is for generated headers or other files not covered by global staging.
    add_custom_command(TARGET stage_my-lib POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different # Use copy_if_different for robustness
                "${CMAKE_CURRENT_BINARY_DIR}/my_lib_export.h"
                "${PYLABHUB_STAGING_DIR}/include/"
    )
    
    # 3. Add commands to stage the library artifacts (.dll, .lib, .so, .a).
    pylabhub_get_library_staging_commands(
      TARGET my-lib
      # Use 'bin' for Windows runtime DLLs; 'lib' for POSIX shared libraries
      DESTINATION bin 
      OUT_COMMANDS stage_lib_commands
    )
    add_custom_command(TARGET stage_my-lib POST_BUILD COMMAND ${stage_lib_commands})

    # 4. Register the local staging target with the global core artifacts aggregator.
    set_property(GLOBAL APPEND PROPERTY CORE_STAGE_TARGETS stage_my-lib)
    ```
3.  **Include the subdirectory in `src/CMakeLists.txt`:** `add_subdirectory(my-lib)`

### Recipe 3: How to Add a New Third-Party Library

This is the most complex task and uses the full power of the **Registration-Based Staging** pattern. The goal is to create a self-contained wrapper script in `third_party/cmake/`.

**Scenario**: Add a new library `new-lib` built with CMake.

1.  **Add the Submodule**: Add `new-lib` as a git submodule in `third_party/`.

2.  **Create the Wrapper Script**: Create `third_party/cmake/new-lib.cmake`.

3.  **Edit the Wrapper Script `new-lib.cmake`**:
    ```cmake
    # third_party/cmake/new-lib.cmake
    include(ThirdPartyPolicyAndHelper)
    include(StageHelpers)

    message(STATUS "[pylabhub-third-party] Configuring new-lib...")

    # 1. Snapshot any cache variables the library might modify.
    snapshot_cache_var(BUILD_SHARED_LIBS)
    snapshot_cache_var(BUILD_TESTS) # Snapshot common upstream test variable
    
    # 2. Set options for the isolated build scope.
    set(BUILD_SHARED_LIBS ON CACHE BOOL "Build new-lib as a shared lib" FORCE)
    if(THIRD_PARTY_DISABLE_TESTS) # Use the global policy variable
      set(BUILD_TESTS OFF CACHE BOOL "Disable new-lib tests via global policy" FORCE)
    endif()

    # 3. Add the subdirectory (assuming new-lib is a CMake project).
    # If new-lib uses ExternalProject_Add, the logic will be different.
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/new-lib EXCLUDE_FROM_ALL)

    # 4. Find the canonical target created by the library.
    _resolve_alias_to_concrete("new-lib::new-lib" _canonical_target)

    # 5. Create our stable, namespaced alias.
    _expose_wrapper(pylabhub_new-lib pylabhub::third_party::new-lib)
    target_link_libraries(pylabhub_new-lib INTERFACE ${_canonical_target})

    # 6. Register artifacts for staging using the project's API.
    if(THIRD_PARTY_INSTALL)
      pylabhub_register_headers_for_staging(
        DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/new-lib/include"
        SUBDIR "new-lib" # Stage into include/new-lib
      )
      pylabhub_register_library_for_staging(TARGET ${_canonical_target})
    endif()
    
    # 7. Add to the install export set.
    install(TARGETS pylabhub_new-lib EXPORT pylabhubTargets)
    install(TARGETS ${_canonical_target} EXPORT pylabhubTargets
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )

    # 8. Restore the cache variables to prevent leakage.
    restore_cache_var(BUILD_SHARED_LIBS BOOL)
    restore_cache_var(BUILD_TESTS BOOL)
    ```
4.  **Include the Wrapper**: Add `include(new-lib)` to `third_party/CMakeLists.txt`.

### Recipe 4: How to Add a New Test Suite

*(This section from the original document is accurate and detailed. A key point for developers is how to handle runtime dependencies for tests on Windows.)*

#### Developer Note: Handling Test Dependencies on Windows

On Windows, an executable needs to be able to find its dependent DLLs at runtime. There are two primary strategies used in this project:

1.  **Copying DLLs (Used by `pylabhub-utils`)**: The `pylabhub-utils.dll` is explicitly copied into the `tests/` staging directory alongside the test executables. This is a simple and effective approach that ensures tests run "out of the box" without any environment configuration. The downside is minor artifact duplication. This is the project's preferred method for core libraries.

2.  **Modifying the PATH (Used by Add-On example)**: For add-on tests, an alternative is to use the `TEST_LAUNCHER` property in CMake. This allows you to prepend the `bin` directory (where release DLLs are staged) to the `PATH` environment variable just for the duration of the test run.

    ```cmake
    # Prepend the main 'bin' directory to the PATH for this test
    set_property(TARGET my-tool-tests PROPERTY
      TEST_LAUNCHER "${CMAKE_COMMAND}" -E env --modify "PATH=path_list_append:${STAGED_BIN_DIR}"
    )
    ```
This method avoids copying DLLs but requires more configuration per-test. Both approaches are valid depending on the situation.
