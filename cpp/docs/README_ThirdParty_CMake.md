# Third-Party CMake Integration Guide

This document is a design and style guide for integrating third-party dependencies via the wrapper scripts in `third_party/cmake/`. It describes the required structure, best practices, and the two distinct patterns for managing libraries.

Each wrapper script is responsible for building its dependency and exposing a clean, namespaced CMake target for the main project to link against (e.g., `pylabhub::third_party::fmt`).

---

## Core Concept: Two Integration Patterns

There are two primary categories for third-party libraries, each with a corresponding integration pattern and staging strategy. The developer must first decide which category a new library falls into.

### Pattern 1: CMake Subprojects (Type A)

- **Who it's for**: Libraries that have a modern, well-behaved CMake build system (e.g., `fmt`, `libzmq`).
- **How it works**: Integrated via `add_subdirectory()`. The wrapper script uses `snapshot_cache_var` and `restore_cache_var` to isolate the subproject's build configuration.
- **Staging**: The wrapper script is **responsible for staging** the library's artifacts by calling the `pylabhub_register_library_for_staging` and `pylabhub_register_headers_for_staging` helper functions.

### Pattern 2: External Prerequisites (Type B)

- **Who it's for**: Libraries that use non-CMake build systems (`make`, `msbuild`, etc.) or are otherwise complex to build (e.g., `luajit`, `libsodium`).
- **How it works**: Integrated via the `pylabhub_add_external_prerequisite` helper function, which wraps `ExternalProject_Add`. This builds the library into an intermediate `${PREREQ_INSTALL_DIR}` directory (`build/prereqs`).
- **Staging**: Staging is **handled automatically by a global, bulk process**. The wrapper script **must not** call any staging helper functions. The entire `prereqs` directory (including `lib`, `include`, and `share`) is copied to the final staging area.

---

## Naming & Exported Symbols Conventions

Each third-party helper script must (where possible) do the following **inside `third_party/cmake/<pkg>.cmake`**:

### 1. Discover the Canonical Concrete Target
- Detect the *real* concrete target(s) produced by the package (e.g., the static or shared library target). This is the **canonical target**.
  Example candidate sets:
  - `fmt` might produce a concrete target `fmt` and an alias `fmt::fmt`. The canonical target is `fmt`.
  - `libzmq` might produce `libzmq-static` or `libzmq`. The canonical target is whichever is built and installable.
- Use this logic:
  1. Maintain a **candidate list** in order of preference (most likely real/installable first).
  2. Iterate through the candidates. The first one that exists and is **not** an `INTERFACE` library is likely the concrete target.
     ```cmake
     if(TARGET "${_cand}" AND NOT TARGET "${_cand}" STREQUAL "INTERFACE")
     ```
  3. Pick the first concrete/installable target found as the package *canonical target*.
 
### 2. Provide a Stable Wrapper Target and Namespaced Alias
- Create a stable `INTERFACE` wrapper target with a deterministic, project-specific name (e.g., `pylabhub_fmt`).
- Create a *namespaced alias* for consumers using the `pylabhub::third_party::` prefix (e.g., `pylabhub::third_party::fmt`). This is the target that downstream consumers should link against.
  ```cmake
  if (NOT TARGET pylabhub_fmt)
    add_library(pylabhub_fmt INTERFACE)
  endif()
  add_library(pylabhub::third_party::fmt ALIAS pylabhub_fmt)
  ```
- If a canonical concrete target was found, make the wrapper forward to it:
  ```cmake
  target_link_libraries(pylabhub_fmt INTERFACE <canonical-target>)
  ```
  Otherwise, populate the wrapper's `INTERFACE_INCLUDE_DIRECTORIES` with vendored include paths.

- Always set `INTERFACE_INCLUDE_DIRECTORIES` on the wrapper so top level and consumers can detect header locations (via `get_target_property(... INTERFACE_INCLUDE_DIRECTORIES)`).

### 3. Stage Artifacts for Installation
- The top-level project defines a global staging directory (`PYLABHUB_STAGING_DIR`).
- The staging rules are now strict based on the integration pattern:
  - **CMake Subprojects**: The wrapper script **must** call `pylabhub_register_headers_for_staging` and `pylabhub_register_library_for_staging`. These registrations are processed to generate `POST_BUILD` copy commands.
  - **External Prerequisites**: The wrapper script **must not** call any registration functions. Staging is handled by a global process that bulk-copies the `prereqs` directory.

---

## Top-level Usage (How Staging & Installation Work)

The top-level `CMakeLists.txt` will:

1.  Define a global staging directory variable: `set(PYLABHUB_STAGING_DIR ${CMAKE_BINARY_DIR}/stage)`.
2.  Define a global "hook" target: `add_custom_target(stage_all)`.
3.  Include `third_party/CMakeLists.txt`, which defines its own `stage_third_party_deps` target and makes `stage_all` depend on it.
4.  Each `<pkg>.cmake` script calls `pylabhub_stage_*` helpers to attach its staging commands to `stage_third_party_deps`.
5.  The top-level project also attaches staging commands for its own targets (`pylabhub-corelib`, `pylabhub-shell`, etc.) to run `POST_BUILD`.
6.  The final `install()` step is a single command that copies the entire, fully populated `${PYLABHUB_STAGING_DIR}` to the installation prefix: `install(DIRECTORY "${PYLABHUB_STAGING_DIR}/" DESTINATION ".")`.
7.  This entire staging and installation mechanism is controlled by the `THIRD_PARTY_INSTALL` option. If `OFF`, no staging or installation rules are generated.

This approach keeps the installation logic atomic and declarative. The responsibility for populating the staging directory is correctly distributed to the components that know about their own artifacts.

---

## Provided Helper Functions in `cmake/ThirdPartyPolicyAndHelper.cmake`

The following helper functions are provided as part of the framework for third-party integration. Each package script should use them to ensure consistency and proper behavior.

1. `pylabhub_add_external_prerequisite(...)`
   - The primary function for building and integrating prerequisites that use **non-CMake** build systems (or require special `ExternalProject_Add` handling).
   - It wraps `ExternalProject_Add` and automates the creation of a stable `IMPORTED` target and the post-build detection/normalization step.
   - The caller provides platform-specific `CONFIGURE_COMMAND`, `BUILD_COMMAND`, and `INSTALL_COMMAND` lists.
   - See the example below for detailed usage.

2. `_resolve_alias_to_concrete(TARGET_NAME OUTVAR)`  
   - If `TARGET_NAME` is an alias, use `get_target_property(ALIASED_TARGET ${TARGET_NAME} ALIASED_TARGET)` and return that; else return original.

3. `snapshot_cache_var(VAR_NAME)`
   - Saves the current state (both its normal and cached value) of `VAR_NAME`. This should be called before `add_subdirectory` for any variable that the sub-project might modify (e.g., `BUILD_SHARED_LIBS`).

4. `restore_cache_var(VAR_NAME CACHE_TYPE)`
   - Restores `VAR_NAME` to its pre-snapshot state. This must be called after `add_subdirectory` to prevent the sub-project's settings from leaking and affecting other parts of the build. The `CACHE_TYPE` (e.g., `BOOL`, `STRING`, `PATH`) is required to correctly manage the cache entry.

Using these provided functions helps keep package scripts concise, consistent, and correctly integrated with the project's build policies.

---

## Example — `third_party/cmake/fmt.cmake` (CMake subproject)

This implementation is for a library that uses CMake and can be added via `add_subdirectory`.

```cmake
# inside third_party/cmake/fmt.cmake
include(ThirdPartyPolicyAndHelper) # provides core helpers
include(StageHelpers)              # provides staging helpers

# Use snapshot/restore to isolate the subproject's build settings
snapshot_cache_var(BUILD_SHARED_LIBS)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build fmt as a static lib" FORCE)

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/fmt EXCLUDE_FROM_ALL)

restore_cache_var(BUILD_SHARED_LIBS BOOL)

# Find the real library target created by the subproject
_resolve_alias_to_concrete("fmt::fmt" _canonical_target)

# Create our stable wrapper and alias
add_library(pylabhub_fmt INTERFACE)
add_library(pylabhub::third_party::fmt ALIAS pylabhub_fmt)
target_link_libraries(pylabhub_fmt INTERFACE "${_canonical_target}")

# Register the library and its headers for staging
if(THIRD_PARTY_INSTALL)
  pylabhub_register_library_for_staging(TARGET ${_canonical_target})
  pylabhub_register_headers_for_staging(
    DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/fmt/include"
    SUBDIR "fmt" # Stage into include/fmt
  )
endif()
```

---

## Example — `third_party/cmake/libexternal.cmake` (External Build)

This is the standard pattern for libraries like `libsodium` and `luajit` that require `ExternalProject_Add`.

**The goal is to define platform-specific build commands and pass them to the generic `pylabhub_add_external_prerequisite` function.**

```cmake
# inside third_party/cmake/libexternal.cmake
include(ThirdPartyPolicyAndHelper)

# 1. Define paths
set(_source_dir "${CMAKE_CURRENT_SOURCE_DIR}/libexternal")
set(_build_dir "${CMAKE_BINARY_DIR}/third_party/libexternal-build")
set(_install_dir "${PREREQ_INSTALL_DIR}")

# 2. Define platform-specific build commands
if(MSVC)
    # MSVC uses msbuild
    find_program(_MSBUILD_EXE msbuild REQUIRED)
    set(_build_command ${_MSBUILD_EXE} libexternal.sln /p:Configuration=Release)
    set(_install_command "") # Let post-build detection handle copying
    set(_byproducts "${_install_dir}/lib/libexternal.lib")
else()
    # POSIX systems use Makefiles
    find_program(_MAKE_PROG make REQUIRED)
    set(_configure_command ${_source_dir}/configure --prefix=${_install_dir})
    set(_build_command ${_MAKE_PROG})
    set(_install_command ${_MAKE_PROG} install)
    set(_byproducts "${_install_dir}/lib/libexternal.a")
endif()

# 3. Call the generic helper function
pylabhub_add_external_prerequisite(
  NAME              libexternal
  SOURCE_DIR        "${_source_dir}"
  BINARY_DIR        "${_build_dir}"
  INSTALL_DIR       "${_install_dir}"

  # Pass the platform-specific commands
  CONFIGURE_COMMAND ${_configure_command}
  BUILD_COMMAND     ${_build_command}
  INSTALL_COMMAND   ${_install_command}
  BUILD_BYPRODUCTS  ${_byproducts}

  # Pass patterns for the post-build detection script
  LIB_PATTERNS      "libexternal.lib;libexternal.a"
  HEADER_SOURCE_PATTERNS "include"
)
```
The helper function handles all the `ExternalProject_Add` boilerplate, the post-build detection step, and the creation of the `pylabhub::third_party::libexternal` imported target.

---

## Error handling & diagnostics

- All helper scripts must print informative `message(STATUS "...")` lines that make it clear:
  - Which canonical target was found for this package.
  - Which include roots were registered for staging.
  - Whether a package was determined header-only or binary.
- If a helper script cannot find headers or any artifact it expects, it should:
  - For optional packages: print `STATUS` and skip.
  - For packages required by the project: use `message(FATAL_ERROR "...")` with actionable advice.

- Top level will:
  - Print the resolved header roots and library targets before finalizing configure.
  - On collisions or a missing staging tree during `cmake --install`, fail with a clear, actionable message.

---

## Checklist for implementers of `third_party/cmake/<pkg>.cmake`

### Is your library a well-behaved CMake project?

#### **YES -> Use the CMake Subproject Pattern**
- [ ] In your `third_party/cmake/<pkg>.cmake` wrapper:
- [ ] Use `snapshot_cache_var`/`restore_cache_var` to isolate the build environment.
- [ ] Call `add_subdirectory()` to integrate the library.
- [ ] Create a `pylabhub::third_party::<pkg>` alias target.
- [ ] **Call `pylabhub_register_library_for_staging()` for the compiled library target.**
- [ ] **Call `pylabhub_register_headers_for_staging()` for the header files.**

#### **NO -> Use the External Prerequisite Pattern**
- [ ] In your `third_party/cmake/<pkg>.cmake` wrapper:
- [ ] Define platform-specific command lists for `CONFIGURE`, `BUILD`, and `INSTALL`.
- [ ] Call `pylabhub_add_external_prerequisite`, passing the commands and detection patterns.
- [ ] In `third_party/CMakeLists.txt`:
- [ ] Add the `include(<pkg>.cmake)` call.
- [ ] Manually create the `IMPORTED` target and the final `pylabhub::third_party::<pkg>` alias that points to the artifact in the `prereqs` directory.
- [ ] **Do not** add any calls to `pylabhub_register_*` functions for this library. Staging is automatic.

---

## FAQ / Notes

**Q: Why have each script add its own custom commands?**  
A: This encapsulates the logic. The `fmt.cmake` script knows exactly where `fmt`'s headers and libraries are. The top-level script shouldn't have to. By letting each script manage its own staging, we make the system more modular and easier to maintain.

**Q: What about non-standard layouts (headers not in a single `include` directory)?**  
A: Register each directory root that contains headers. The top-level collision check will still work, as it compares relative paths within each root. If headers with identical relative paths exist in different roots, the configure step will fail as intended.

**Q: Should the staging flatten include contents?**  
A: No, it should not flatten. The contents of each include root are copied into `${PYLABHUB_STAGING_DIR}/include/` while preserving their subdirectory structure. For example, consumers should use `#include <fmt/format.h>`, not `#include <format.h>`.

**Q: What if two third parties install the same header relative path?**  
A: This will cause a fatal error at configure time. This is intentional to prevent surprising header shadowing and forces the developer to resolve the dependency conflict.

**Q: How are Precompiled Headers (PCH) handled? Is it tunable?**  
A: Yes, PCH usage is tunable via the global `THIRD_PARTY_ALLOW_UPSTREAM_PCH` option (default `OFF`).
- When `OFF`, each wrapper script is responsible for setting the appropriate variables to disable PCH for its subproject.
- When `ON`, the wrapper scripts do not interfere, allowing the subproject to use PCH if its build system is configured to do so (e.g., via `target_precompile_headers`).
We do **not** define a central PCH directory. Modern CMake automatically manages PCH artifacts in a safe, target-specific manner, and centralizing them would be fragile and unnecessary.

---

## Closing notes

- The **wrapper target + namespaced alias** pattern provides a stable, future-proof API for consumers, even if upstream target names change.
- Centralizing logic in `ThirdPartyPolicyAndHelper.cmake` keeps package scripts short and consistent.
- The staging process is now imperative, with each script adding commands to a shared target. This gives maximum flexibility to each package script.
