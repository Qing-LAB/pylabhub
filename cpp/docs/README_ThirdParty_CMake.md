# THIRD_PARTY_CMAKESCRIPT.md

A design and style guide for `third_party/cmake/*.cmake` wrapper scripts and their integration into the top-level `CMakeLists.txt`.

This document describes the recommended structure and required behaviors for CMake wrapper scripts under `third_party/cmake/`.

Each script manages a specific third-party library. Because each script has the most accurate knowledge of its package, it is responsible for two primary tasks:

- Exposing clean and consistent CMake targets for the main project to link against.
- Staging all necessary artifacts (headers, libraries) into the unified project staging directory (e.g., `${CMAKE_BINARY_DIR}/stage`) provided by the main project. This ensures that each package's specific layout and installation needs are handled correctly.

---

## Goals (high level)

1.  **Canonical Targets**: Each third-party script builds its project (or finds a system-installed version) and exposes a canonical CMake target for the main project to use.
3.  **Encapsulated Staging**: Each wrapper script is responsible for staging its artifacts by using the provided `pylabhub_stage_*` helper functions. This encapsulates package-specific logic while ensuring a consistent staging process.
4.  **Fail-Fast**: Collisions, such as duplicate header paths or library names within the staging directory, are detected early.

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
- The top-level project defines a global staging directory (`PYLABHUB_STAGING_DIR`) and two custom targets: `stage_third_party_deps` (for third-party only) and `stage_all` (for the entire project).
- Each wrapper script must use the `pylabhub_register_headers_for_staging` and `pylabhub_register_library_for_staging` helper functions. These functions register the artifacts to a global property list. A centralized loop in `third_party/CMakeLists.txt` then processes this list and attaches the necessary `add_custom_command` calls to the `stage_third_party_deps` target.
  - **Headers**: `pylabhub_stage_headers(DIRECTORIES <path-to-headers> SUBDIR <pkg-name>)`
  - **Libraries**: `pylabhub_stage_libraries(TARGETS <concrete-target>)`
- This approach ensures that the logic for finding and copying artifacts is consistent, while knowledge of *what* to copy remains encapsulated within the script that knows the most about the package.

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

## Header-only vs Binary libraries — how to treat them

- **Header-only libraries**
  - No binary artifacts to stage.
  - The wrapper script must call `pylabhub_stage_headers` to copy the header directory into `${PYLABHUB_STAGING_DIR}/include/`.

- **Libraries producing binaries**
  - Must expose a **concrete target** (static/shared) discovered by the wrapper script.
  - The wrapper script must call `pylabhub_stage_libraries` to copy the library artifact (`$<TARGET_FILE:concrete_target>`) into `${PYLABHUB_STAGING_DIR}/lib/`.
  - The script must also call `pylabhub_stage_headers` to copy the associated header directory into `${PYLABHUB_STAGING_DIR}/include/`.

- **If only an alias or INTERFACE wrapper exists for a library**:
  - The wrapper script is responsible for resolving the alias to its underlying concrete target before calling `pylabhub_stage_libraries`.

---

## Provided Helper Functions in `ThirdPartyPolicyAndHelper.cmake`

The following helper functions are provided as part of the framework for third-party integration. Each package script should use them to ensure consistency and proper behavior.

1. `_resolve_alias_to_concrete(TARGET_NAME OUTVAR)`  
   - If `TARGET_NAME` is an alias, use `get_target_property(ALIASED_TARGET ${TARGET_NAME} ALIASED_TARGET)` and return that; else return original.

2. `_expose_wrapper(WRAPPER_NAME NAMESPACE_ALIAS)`  
   - Creates an `INTERFACE` library named `WRAPPER_NAME` and an `ALIAS` library named `NAMESPACE_ALIAS` that points to it. This provides the standard two-layer abstraction for all dependencies.

3. `snapshot_cache_var(VAR_NAME)`
   - Saves the current state (both its normal and cached value) of `VAR_NAME`. This should be called before `add_subdirectory` for any variable that the sub-project might modify (e.g., `BUILD_SHARED_LIBS`).

4. `restore_cache_var(VAR_NAME CACHE_TYPE)`
   - Restores `VAR_NAME` to its pre-snapshot state. This must be called after `add_subdirectory` to prevent the sub-project's settings from leaking and affecting other parts of the build. The `CACHE_TYPE` (e.g., `BOOL`, `STRING`, `PATH`) is required to correctly manage the cache entry.

Using these provided functions helps keep package scripts concise, consistent, and correctly integrated with the project's build policies.

---

## Example — `third_party/cmake/fmt.cmake` (recommended implementation sketch)

```cmake
# inside third_party/cmake/fmt.cmake
include(ThirdPartyPolicyAndHelper) # provides core helpers
include(StageHelpers)              # provides staging helpers

# candidate list (preference order)
set(_fmt_candidates fmt fmt::fmt)

set(_fmt_canonical "")
foreach(_cand IN LISTS _fmt_candidates)
  if(TARGET "${_cand}" AND NOT TARGET "${_cand}" STREQUAL "INTERFACE")
    _resolve_alias_to_concrete("${_cand}" _real)
    set(_fmt_canonical "${_real}")
    break()
  endif()
endforeach()

# create stable wrapper + alias
_expose_wrapper(pylabhub_fmt pylabhub::third_party::fmt)

if(_fmt_canonical)
  # Found a binary library, link the wrapper to it and register it for staging.
  target_link_libraries(pylabhub_fmt INTERFACE "${_fmt_canonical}")
  pylabhub_register_library_for_staging(TARGET ${_fmt_canonical})
else()
  # Header-only fallback: expose include directory directly.
  target_include_directories(pylabhub_fmt INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/fmt/include>
  )
endif()

# Always register the header directory for staging.
pylabhub_register_headers_for_staging(
  DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/fmt/include"
  SUBDIR "fmt" # Stage into include/fmt
)
```

`libzmq.cmake` would follow the same pattern, preferring to resolve a concrete `libzmq-static` target and registering that for `THIRD_PARTY_POST_BUILD_LIBS`, while also registering `/third_party/libzmq/include` into `THIRD_PARTY_POST_BUILD_HEADERS`.

---

## Example — `ExternalProject_Add` for non-CMake libraries (robust pattern)

This is the most robust pattern, used for libraries like `libzmq`, `libsodium`, and `luajit` that are built as prerequisites. It uses `ExternalProject_Add` with a post-build detection script to create a stable, predictable library target that is resilient to changes in platform or library version.

**Scenario**: Add `libexternal`, a non-CMake library, to the project.

1.  **Add Submodule**: Add `libexternal` source to `third_party/`.

2.  **Create Prerequisite Build Script**: Create `third_party/cmake/libexternal.cmake`. This script will contain the complete logic for building and discovering the library.

    ```cmake
    # third_party/cmake/libexternal.cmake
    include(ExternalProject)
    include(ThirdPartyPolicyAndHelper)

    # Define source and temporary install paths
    set(LIBEXTERNAL_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/libexternal")
    set(LIBEXTERNAL_INSTALL_DIR "${PREREQ_INSTALL_DIR}")

    # --- Define a stable path for the final library artifact ---
    set(LIBEXTERNAL_STABLE_LIB_PATH "${LIBEXTERNAL_INSTALL_DIR}/lib/libexternal-stable")
    set(LIBEXTERNAL_STAMP_FILE "${LIBEXTERNAL_INSTALL_DIR}/libexternal-stamp.txt")

    # --- Define platform-specific build commands ---
    # This project does not support out-of-source builds, so we must copy the source
    # to a temporary build directory to keep our own source tree pristine.
    set(LIBEXTERNAL_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/third_party/libexternal-build")
    set(LIBEXTERNAL_CONFIGURE_COMMAND ${CMAKE_COMMAND} -E copy_directory
      "${LIBEXTERNAL_SOURCE_DIR}/"
      "${LIBEXTERNAL_BINARY_DIR}"
    )

    if(MSVC)
      set(LIBEXTERNAL_BUILD_COMMAND      "msbuild.exe libexternal.sln /p:Configuration=Release")
      set(LIBEXTERNAL_BUILD_WORKING_DIR  "${LIBEXTERNAL_BINARY_DIR}/msvc")
      set(LIBEXTERNAL_INSTALL_COMMAND    "") # Let detection script copy artifacts
    else()
      set(LIBEXTERNAL_BUILD_COMMAND      "${CMAKE_MAKE_PROGRAM}")
      set(LIBEXTERNAL_BUILD_WORKING_DIR  "${LIBEXTERNAL_BINARY_DIR}")
      set(LIBEXTERNAL_INSTALL_COMMAND    "${CMAKE_MAKE_PROGRAM}" install PREFIX="${LIBEXTERNAL_INSTALL_DIR}")
    endif()

    # --- Create the post-build detection script ---
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/detect_libexternal.cmake" [[
      message(STATUS "detect_libexternal.cmake: Discovering artifacts...")
      set(INSTALL_DIR "${LIBEXTERNAL_INSTALL_DIR}")
      set(BINARY_DIR "${LIBEXTERNAL_BINARY_DIR}") # Use the binary dir for searching
      set(STABLE_LIB_PATH_NO_EXT "${LIBEXTERNAL_STABLE_LIB_PATH}")
      set(STAMP_FILE "${LIBEXTERNAL_STAMP_FILE}")

      # Search for the compiled library in likely output locations
      file(GLOB LIBS
        "${INSTALL_DIR}/lib/libexternal*.a"
        "${BINARY_DIR}/msvc/Release/libexternal*.lib" # Example for MSVC
      )
      if(NOT LIBS)
        message(FATAL_ERROR "detect_libexternal.cmake: Could not find library artifact.")
      endif()

      # Copy the found library to the stable path
      list(GET LIBS 0 REAL_LIB_PATH)
      get_filename_component(REAL_LIB_EXT "${REAL_LIB_PATH}" EXT)
      set(STABLE_LIB_FULL_PATH "${STABLE_LIB_PATH_NO_EXT}${REAL_LIB_EXT}")
      file(COPY "${REAL_LIB_PATH}" DESTINATION "${INSTALL_DIR}/lib/")
      get_filename_component(REAL_LIB_NAME "${REAL_LIB_PATH}" NAME)
      file(RENAME "${INSTALL_DIR}/lib/${REAL_LIB_NAME}" "${STABLE_LIB_FULL_PATH}")

      # Copy headers
      file(COPY_DIRECTORY "${BINARY_DIR}/include/" DESTINATION "${INSTALL_DIR}/include/libexternal/")
      
      # Create stamp file to signal completion
      file(WRITE "${STAMP_FILE}" "OK")
    ]])

    # --- Define the ExternalProject_Add target ---
    ExternalProject_Add(libexternal_external
      SOURCE_DIR        "${LIBEXTERNAL_SOURCE_DIR}"
      BINARY_DIR        "${LIBEXTERNAL_BINARY_DIR}"
      INSTALL_DIR       "${LIBEXTERNAL_INSTALL_DIR}"
      
      CONFIGURE_COMMAND ${LIBEXTERNAL_CONFIGURE_COMMAND}
      BUILD_COMMAND     ${LIBEXTERNAL_BUILD_COMMAND}
      WORKING_DIRECTORY ${LIBEXTERNAL_BUILD_WORKING_DIR}
      INSTALL_COMMAND   ${LIBEXTERNAL_INSTALL_COMMAND}
      
      # This command runs *after* install to perform detection and create the stable artifact
      COMMAND           ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/detect_libexternal.cmake"
      
      # The stamp file is the final, predictable output of this entire process
      BUILD_BYPRODUCTS  "${LIBEXTERNAL_STAMP_FILE}"
    )
    ```

3.  **Integrate and Stage in `third_party/CMakeLists.txt`**: Now, create the clean `IMPORTED` target that the rest of the project will use.

    ```cmake
    # third_party/CMakeLists.txt

    # ... other libraries ...

    # 1. Include the script that defines the libexternal_external target.
    include(libexternal)
    
    # 2. Add the external target to the master prerequisite build.
    add_dependencies(build_prerequisites libexternal_external)

    # --- 3. Define the clean IMPORTED target for consumers ---
    # This block is simple and platform-agnostic.
    set(LIBEXTERNAL_STABLE_LIB_PATH "${PREREQ_INSTALL_DIR}/lib/libexternal-stable")
    set(LIBEXTERNAL_INCLUDE_DIR "${PREREQ_INSTALL_DIR}/include/libexternal")

    add_library(pylabhub::third_party::libexternal UNKNOWN IMPORTED GLOBAL)
    set_target_properties(pylabhub::third_party::libexternal PROPERTIES
      IMPORTED_LOCATION "${LIBEXTERNAL_STABLE_LIB_PATH}"
      INTERFACE_INCLUDE_DIRECTORIES "${LIBEXTERNAL_INCLUDE_DIR}"
    )
    add_dependencies(pylabhub::third_party::libexternal build_prerequisites)

    # --- 4. Register the artifacts for final staging ---
    if(THIRD_PARTY_INSTALL)
      pylabhub_register_library_for_staging(TARGET pylabhub::third_party::libexternal)
      pylabhub_register_headers_for_staging(
        DIRECTORIES "${LIBEXTERNAL_INCLUDE_DIR}"
        SUBDIR "" # Copy contents of include/libexternal/ to stage/include/
        EXTERNAL_PROJECT_DEPENDENCY libexternal_external
      )
    endif()
    ```

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

- [ ] Provide a candidate list of plausible concrete targets (in likely order).
- [ ] Resolve aliases to their aliased concrete target(s) and prefer concrete/installable targets.
- [ ] Create a `pylabhub_<pkg>` wrapper target and `pylabhub::third_party::<pkg>` namespaced alias.
- [ ] Ensure `INTERFACE_INCLUDE_DIRECTORIES` is set on the wrapper target (use `$<BUILD_INTERFACE:...>` and `$<INSTALL_INTERFACE:...>`).
- [ ] If a concrete library target is found, call `pylabhub_stage_libraries` to stage it to `${PYLABHUB_STAGING_DIR}/lib`.
- [ ] If a header directory exists, call `pylabhub_stage_headers` to stage it to `${PYLABHUB_STAGING_DIR}/include`.
- [ ] Emit clear `message(STATUS ...)` lines describing actions taken & fallbacks used.
- [ ] Use helper functions from `ThirdPartyPolicyAndHelper.cmake` to keep scripts concise and consistent.

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
