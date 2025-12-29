### High-Level Summary

Overall, the CMake system is excellent. It is modern, robust, and well-designed, adhering to many best practices. The core design principles—unified staging, dependency isolation, and clear abstraction through wrapper targets—are implemented effectively. The code is well-commented, and the isolation of third-party builds using the `snapshot_cache_var` / `restore_cache_var` macros is a standout feature that prevents common build issues.

My review identifies a few areas with minor redundancy, inconsistencies, and one potential bug related to sub-project installation. The suggestions aim to further enhance the clarity, correctness, and maintainability of the system.

---

### 1. Critical Issues & Potential Bugs

#### **Issue: Upstream Installation Leakage in `fmt.cmake` and `libzmq.cmake`**

The `add_subdirectory()` command is used with `EXCLUDE_FROM_ALL`, which correctly prevents the third-party targets from being built as part of the default `all` target. However, this does **not** prevent their `install()` rules from being executed when a user runs `cmake --install`.

*   **File:** `third_party/cmake/fmt.cmake`, `third_party/cmake/libzmq.cmake`
*   **Problem:** The `fmt` and `libzmq` sub-projects contain their own `install()` commands. When `cmake --install` is run on our project, CMake will execute those `install()` rules, potentially polluting the final install location (`CMAKE_INSTALL_PREFIX`) with artifacts that bypass our unified staging system. The goal of the staging system is to have full control over the installation layout.
*   **Suggestion:** To fix this, you must prevent the sub-project from registering its own installation rules. The most common way to do this is to set `CMAKE_INSTALL_PREFIX` to a temporary directory *within* the isolated scope of the sub-project build.

**Example Fix (in `third_party/cmake/fmt.cmake`):**

```cmake
# --- 1. Snapshot cache variables ---
snapshot_cache_var(CMAKE_INSTALL_PREFIX) # Add this
# ... other snapshots

# --- 2. Configure the fmt sub-project build ---
set(CMAKE_INSTALL_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/_install" CACHE PATH "Isolate upstream install" FORCE) # Add this
# ... other configuration

# --- 3. Add the fmt subproject ---
add_subdirectory(fmt EXCLUDE_FROM_ALL)

# ... (rest of the file)

# --- 8. Restore cache variables ---
restore_cache_var(CMAKE_INSTALL_PREFIX PATH) # Add this
# ... other restores
```

---

### 2. Obsolete Code & Redundancy

#### **Obsolete Option: `PYLABHUB_BUILD_SHARED`**

*   **File:** `CMakeLists.txt`, `cmake/ToplevelOptions.cmake`
*   **Problem:** The top-level `CMakeLists.txt` documents the option `-DPYLABHUB_BUILD_SHARED=ON/OFF` to "Build corelib as shared/static". However, the final configuration summary at the end of the script hardcodes the message: `Utility Library: pylabhub-utils (SHARED)`. This suggests that the option is either no longer wired up to control the build type of `pylabhub-utils`, or the summary message is incorrect. This can lead to user confusion.
*   **Suggestion:** Review the `src/utils/CMakeLists.txt` file. If the option is still functional, update the summary message to reflect the actual build type. If the option is obsolete, remove it from `ToplevelOptions.cmake` and the documentation.

#### **Redundant Cache Variables in Wrappers**

*   **Files:** `third_party/cmake/fmt.cmake`, `third_party/cmake/libzmq.cmake`
*   **Problem:** When forcing a static or shared build, multiple cache variables are set (e.g., `BUILD_SHARED_LIBS`, `BUILD_SHARED`, `BUILD_STATIC`). The comments correctly identify some of these as potentially "redundant".
*   **Suggestion:** This is a minor issue, as setting extra variables is harmless. However, for cleanliness, you could investigate which variable each sub-project *actually* respects and remove the others. For example, most modern projects only respect `BUILD_SHARED_LIBS`.

#### **Non-existent Cache Variable: `FMT_INSTALL`**

*   **File:** `third_party/cmake/fmt.cmake`
*   **Problem:** The script calls `snapshot_cache_var(FMT_INSTALL)`. The `fmt` project does not use an `FMT_INSTALL` option. This is likely a leftover from a previous version or another project.
*   **Suggestion:** Remove the `snapshot_cache_var(FMT_INSTALL)` and `restore_cache_var(FMT_INSTALL BOOL)` lines. This has no functional impact but cleans up the script.

---

### 3. Suggestions for Improvement

#### **Improve Clarity of Staging Option**

*   **File:** `third_party/cmake/ThirdPartyPolicyAndHelper.cmake`
*   **Problem:** The option `THIRD_PARTY_INSTALL` is slightly misleading. It doesn't control `installation` in the `cmake --install` sense; rather, it controls whether third-party artifacts are copied to the *staging* directory.
*   **Suggestion:** For better clarity, consider renaming the option to `PYLABHUB_STAGE_THIRD_PARTY` or similar. This would make its purpose (`ON` = stage, `OFF` = don't stage) perfectly clear.

#### **Inconsistent `include()` path for `googletest.cmake`**

*   **File:** `third_party/CMakeLists.txt`
*   **Problem:** The line `list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")` correctly adds the helper directory to the module path. However, `googletest` is included via the explicit relative path `include(cmake/googletest.cmake)`. Other modules like `fmt` are included simply as `include(fmt)`.
*   **Suggestion:** Make it consistent. Change `include(cmake/googletest.cmake)` to `include(googletest)`.

#### **Simplify Directory Creation**

*   **File:** `CMakeLists.txt`
*   **Problem:** The `create_staging_dirs` target lists a separate `COMMAND` for each directory it creates. This is verbose and requires manual updating if a new staged directory is needed.
*   **Suggestion:** Use a `foreach` loop to make this more concise and easier to maintain.

```cmake
# Before
add_custom_target(create_staging_dirs
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PYLABHUB_STAGING_DIR}/bin"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PYLABHUB_STAGING_DIR}/lib"
    # ... and so on
)

# After
set(staging_subdirs bin lib include IgorXOP docs tools config tests)
add_custom_target(create_staging_dirs COMMENT "Initializing staging directories")
foreach(subdir IN LISTS staging_subdirs)
    add_custom_command(TARGET create_staging_dirs
        COMMAND ${CMAKE_COMMAND} -E make_directory "${PYLABHUB_STAGING_DIR}/${subdir}"
    )
endforeach()
```

#### **Simplify `find_path` Logic in `XOPToolKit.cmake`**

*   **File:** `third_party/cmake/XOPToolKit.cmake`
*   **Problem:** The script first uses `find_path` and then has separate fallback logic to check for `XOP.h` in the root of the search path.
*   **Suggestion:** The `find_path` command can check the root path itself if you add the main path to `HINTS` or `PATHS` and remove `PATH_SUFFIXES`. A simpler way is to just add the root path to the list of `PATH_SUFFIXES` with an empty string or `.`.

```cmake
# In function _check_root_for_xop
find_path(_try_include
  NAMES "XOP.h"
  HINTS "${root}"
  PATH_SUFFIXES "include" "XOP Support" "" # Adding "" searches the root path itself
  NO_DEFAULT_PATH
)
```
This would eliminate the need for the subsequent `if(NOT _try_include)` block.