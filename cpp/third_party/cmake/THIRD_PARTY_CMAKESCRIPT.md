# THIRD_PARTY_CMAKESCRIPT.md

A design and style guide for `third_party/cmake/*.cmake` helper scripts and how the top-level CMakeLists will consume them.

This document describes conventions, required behavior, and recommended APIs for each third-party helper script under `third_party/cmake/`. Each of these third-party helper scripts will handle specific third-party modules, and must expose clean, consistent targets and metadata so the top level can (a) access the header files and link against resulting libraries when available, (b) stage their headers/libs into the project `build/install/` tree, and (c) include/install them correctly for downstream users.

---

## Goals (high level)

1. Each third-party script builds the third-party project (or wires to the system one), and exposes canonical targets usable by the top level.
2. The top level uses a small, accurate set of lists (populated by the third-party scripts) to decide what to copy into the local staging tree during post-build.
3. Header-only libraries and libraries that produce binary artifacts are handled clearly and consistently — stage header files into `install/include` and binary libraries into `install/lib`.
4. Detect collisions early and fail fast (e.g., duplicate header relative paths across staged include roots, duplicate lib basenames).

---

## Naming & exported symbols conventions

Each third-party helper script must (where possible) do the following **inside `third_party/cmake/<pkg>.cmake`**:

### Canonical concrete target discovery
- Detect the *real* concrete target(s) produced by the package (static/shared target names). Call these *canonical targets*.  
  Example candidate sets:
  - `fmt` might produce `fmt` (concrete) and `fmt::fmt` (alias). Prefer the concrete target (`fmt`) for copying `$<TARGET_FILE:...>`.
  - `libzmq` might produce `libzmq-static` or `libzmq`. Choose whichever is real and installable.

- Use this logic:
  1. Maintain a **candidate list** in order of preference (most likely real/installable first).
  2. For each candidate:
     - If `TARGET <candidate>` is present, test whether it is installable (see helper `_is_target_installable()` below) and/or is of type `STATIC_LIBRARY` or `SHARED_LIBRARY`.
     - If the candidate is an ALIAS, resolve its `ALIASED_TARGET` and inspect that.
  3. Pick the first concrete/installable target found as the package *canonical target*.

### Provide a stable wrapper target and namespaced alias
- Create a small, stable wrapper target (usually an `INTERFACE` target) with a deterministic un-namespaced name, e.g., `pylabhub_fmt` or `pylabhub_zmq`.
- Create a *namespaced alias* to expose to the top level, e.g.:
  ```cmake
  if(NOT TARGET pylabhub_fmt)
    add_library(pylabhub_fmt INTERFACE)
  endif()
  add_library(pylabhub::fmt ALIAS pylabhub_fmt)
  ```
- If a canonical concrete target was found, make the wrapper forward to it:
  ```cmake
  target_link_libraries(pylabhub_fmt INTERFACE <canonical-target>)
  ```
  Otherwise, populate the wrapper's `INTERFACE_INCLUDE_DIRECTORIES` with vendored include paths.

- Always set `INTERFACE_INCLUDE_DIRECTORIES` on the wrapper so top level and consumers can detect header locations (via `get_target_property(... INTERFACE_INCLUDE_DIRECTORIES)`).

### Provide installability when appropriate
- If the third-party project includes `install()` rules and you want to support `cmake --install`, either:
  - Rely on the third-party's own `install(TARGETS ...)` and `install(DIRECTORY ...)` calls, **or**
  - If you package with a wrapper, the wrapper should be created such that `install(TARGETS pylabhub_fmt EXPORT ...)` is meaningful — but prefer leaving install rules to the subproject to keep semantics consistent.
- Document whether the package is header-only or produces installable libs.

---

## Lists / Variables to be populated for top-level staging

Top-level CMake will rely on a small set of project-wide variables that third-party scripts must set/append to. These are authoritative for staging behavior.

**Variables (names & semantics):**

- `THIRD_PARTY_POST_BUILD_HEADERS` — semicolon list of absolute directories containing headers to stage (each directory is expected to be copied into `<build>/install/include/` and flattened at that top level). Example entries:
  - `/path/to/third_party/fmt/include`
  - `$<BUILD_INTERFACE:...>` resolved to absolute path during configure

- `THIRD_PARTY_POST_BUILD_LIBS` — semicolon list of *concrete* CMake target names (not aliases or interface-only targets) that produce binary library artifacts and that should be copied (via `$<TARGET_FILE:...>`) into `<build>/install/lib/`. Example entries:
  - `fmt` (concrete static library target)
  - `libzmq-static`

- `THIRD_PARTY_HEADER_ONLY_PKGS` — optional list of package keys that are header-only. Helpful to make logic explicit.

- `THIRD_PARTY_INSTALLABLE_HEADERS` — (optional) mapping or list of pairs indicating if a package performed its own `install(DIRECTORY ...)` and where. Useful if the third party already installs into a skippable location.

**Rules for the scripts:**

- Each `third_party/cmake/<pkg>.cmake` that discovers a vendor include path must append the absolute include root to `THIRD_PARTY_POST_BUILD_HEADERS`. Use `file(REAL_PATH ...)` when possible to ensure unique canonical paths.
- Each script that discovers an actual, concrete library target should append that concrete target name (as a literal CMake target, e.g. `libzmq-static`) to `THIRD_PARTY_POST_BUILD_LIBS`.
- For header-only packages, the canonical target might be an `INTERFACE` only; do **not** add the INTERFACE target to `THIRD_PARTY_POST_BUILD_LIBS`. Instead add the include directory to `THIRD_PARTY_POST_BUILD_HEADERS` and optionally add to `THIRD_PARTY_HEADER_ONLY_PKGS`.

**Important:** scripts should avoid appending duplicate paths/targets (use `list(FIND ...)` to detect duplicates before append).

---

## Top-level usage (how staging will work)

The top-level `CMakeLists.txt` will:

1. Include `third_party/CMakeLists.txt` which in turn includes `third_party/cmake/<pkg>.cmake` for each package.
2. After all third-party scripts are processed, read the final values of:
   - `THIRD_PARTY_POST_BUILD_HEADERS`
   - `THIRD_PARTY_POST_BUILD_LIBS`
3. Use those lists **only** to:
   - Schedule `add_custom_command(TARGET stage_install POST_BUILD ... copy_directory ...)` for each header directory (flattening into `<build>/install/include`, preserving subdirectories).
   - Schedule `add_custom_command(TARGET stage_install POST_BUILD ... copy $<TARGET_FILE:concrete_target> ...)` for each concrete lib target.
4. Run a configure-time collision check on header lists:
   - For each include root, enumerate relative paths (recursively) and fail with `message(FATAL_ERROR)` if the same relative path appears in two different include roots (prevents accidental header override).
5. Run a build-time duplicate basename check on the staged `lib/` directory (a small CMake script run via `cmake -P` after copies) to ensure two lib files do not share the same basename (e.g. `libfmt.a` from two providers).

This approach guarantees the top-level only needs accurate lists and not to know about complicated target alias resolution logic — that stays inside each third-party helper script.

---

## Header-only vs Binary libraries — how to treat them

- **Header-only libraries**
  - No binary artifacts to stage.
  - Must expose `INTERFACE_INCLUDE_DIRECTORIES` via the wrapper target (or set `THIRD_PARTY_POST_BUILD_HEADERS` to include the include root).
  - Add package to `THIRD_PARTY_HEADER_ONLY_PKGS` (optional but recommended).
  - Top-level will copy the header directory into `build/install/include/`.

- **Libraries producing binaries**
  - Must expose a **concrete installable target** (static/shared) discovered by the helper script.
  - The helper script must append the concrete target name to `THIRD_PARTY_POST_BUILD_LIBS`.
  - The helper script should also provide include directories (usually via forwarding includes on the wrapper target).
  - Top-level will schedule `copy $<TARGET_FILE:concrete_target>` into `build/install/lib/`.

- **If only an alias or INTERFACE wrapper exists for a library**:
  - The helper script is responsible for resolving that alias to the concrete target and appending the **concrete** target to `THIRD_PARTY_POST_BUILD_LIBS`. Do not append interface/alias targets to the libs list as they do not produce files.

---

## Suggested helper functions to include in `third_party/cmake/ThirdPartyPolicyAndHelper.cmake`

Provide small helper functions that each package script can call:

1. `_is_target_installable(TARGET_NAME OUTVAR)`  
   - Inspect `get_target_property(TYPE ${TARGET_NAME} TYPE)` and whether it's `STATIC_LIBRARY` or `SHARED_LIBRARY` or an OBJECT library with configured outputs. Return boolean in `OUTVAR`. Prefer check for `INSTALL` rules if available (harder) but TYPE is primary.

2. `_resolve_alias_to_concrete(TARGET_NAME OUTVAR)`  
   - If `TARGET_NAME` is an alias, use `get_target_property(ALIASED_TARGET ${TARGET_NAME} ALIASED_TARGET)` and return that; else return original.

3. `_append_unique_list(VAR VALUE)`  
   - Append `VALUE` to `VAR` only if not present.

4. `_register_post_build_header(PATH)`  
   - Do `file(REAL_PATH ...)` then append to `THIRD_PARTY_POST_BUILD_HEADERS` using `_append_unique_list`.

5. `_register_post_build_lib(TARGET)`  
   - Append concrete target name to `THIRD_PARTY_POST_BUILD_LIBS` using `_append_unique_list`.

6. `_expose_wrapper(TARGET_NAME WRAPPER_NAME NAMESPACE)`  
   - Create `add_library(<wrapper> INTERFACE)` if missing, create alias `add_library(<namespace>::<pkg> ALIAS <wrapper>)`, and `target_link_libraries(<wrapper> INTERFACE <concrete>...)` as needed.

Providing these functions reduces repeated logic in each `/third_party/cmake/<pkg>.cmake`.

---

## Example — `third_party/cmake/fmt.cmake` (recommended implementation sketch)

```cmake
# inside third_party/cmake/fmt.cmake
include(ThirdPartyPolicyAndHelper.cmake)  # provides helpers above

# candidate list (preference order)
set(_fmt_candidates fmt;fmt::fmt;fmt::fmt-header-only)

set(_fmt_canonical "")
foreach(_cand IN LISTS _fmt_candidates)
  if(TARGET "${_cand}")
    _resolve_alias_to_concrete("${_cand}" _real)
    _is_target_installable("${_real}" _inst)
    if(_inst)
      set(_fmt_canonical "${_real}")
      break()
    endif()
  endif()
endforeach()

# create stable wrapper + alias
if(NOT TARGET pylabhub_fmt)
  add_library(pylabhub_fmt INTERFACE)
endif()
add_library(pylabhub::fmt ALIAS pylabhub_fmt)

if(_fmt_canonical)
  target_link_libraries(pylabhub_fmt INTERFACE "${_fmt_canonical}")
  _register_post_build_lib("${_fmt_canonical}")
else()
  # header-only fallback: expose include dir
  _register_post_build_header("${CMAKE_CURRENT_SOURCE_DIR}/fmt/include")
  target_include_directories(pylabhub_fmt INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/fmt/include>
    $<INSTALL_INTERFACE:include>
  )
endif()

# even if canonical target exists, ensure we still register include dir for staging
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fmt/include")
  _register_post_build_header("${CMAKE_CURRENT_SOURCE_DIR}/fmt/include")
endif()
```

`libzmq.cmake` would follow the same pattern, preferring to resolve a concrete `libzmq-static` target and registering that for `THIRD_PARTY_POST_BUILD_LIBS`, while also registering `/third_party/libzmq/include` into `THIRD_PARTY_POST_BUILD_HEADERS`.

---

## Staging policy & collision checks (top-level responsibilities summary)

- Top-level staging will:
  - At configure time:
    - Read `THIRD_PARTY_POST_BUILD_HEADERS` (list of absolute dirs). Compute all relative paths inside each and fail (FATAL) if the same relative path occurs in more than one include root.
    - Report the final resolved include roots for debugging.
  - At build time (via `stage_install` target):
    - Creates `install/bin`, `install/lib`, `install/include`.
    - Copies project binaries and headers.
    - Adds `add_custom_command(TARGET stage_install POST_BUILD copy_directory <each include-root> <install/include>)`.
    - Adds `add_custom_command(TARGET stage_install POST_BUILD copy $<TARGET_FILE:<lib-target>> <install/lib>)` for each lib target.
    - After copies, run duplicate-basename check on `install/lib`.

- Do **not** stage XOP bundles via this mechanism. XOP subproject handles its own post-build staging into `<build>/install/xop`.

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
  - On collisions or missing staging tree during `cmake --install`, fail with a clear, actionable message.

---

## Checklist for implementers of `third_party/cmake/<pkg>.cmake`

- [ ] Provide a candidate list of plausible concrete targets (in likely order).
- [ ] Resolve aliases to their aliased concrete target(s) and prefer concrete/installable targets.
- [ ] Create a `pylabhub_<pkg>` wrapper target and `pylabhub::pkg` namespaced alias.
- [ ] Ensure `INTERFACE_INCLUDE_DIRECTORIES` is set on the wrapper target (use `$<BUILD_INTERFACE:...>` and `$<INSTALL_INTERFACE:...>`).
- [ ] If concrete target found, call `_register_post_build_lib(<concrete-target>)`.
- [ ] If header dir exists, call `_register_post_build_header(<abs-path>)`.
- [ ] Emit clear `message(STATUS ...)` lines describing actions taken & fallbacks used.
- [ ] Avoid adding interface or alias targets directly to `THIRD_PARTY_POST_BUILD_LIBS`. Always register concrete target names.
- [ ] Use helper functions from `ThirdPartyPolicyAndHelper.cmake` to keep scripts concise and consistent.

---

## FAQ / Notes

**Q: Why keep header directory lists instead of relying on `INTERFACE_INCLUDE_DIRECTORIES` alone?**  
A: `INTERFACE_INCLUDE_DIRECTORIES` values can be generator expressions (e.g. `$<BUILD_INTERFACE:...>`). We still use them, but storing canonical, absolute include directories in `THIRD_PARTY_POST_BUILD_HEADERS` ensures staging is deterministic and independent of generator expressions at copy time. It also allows vendor scripts to provide fallback include directories.

**Q: What about nonstandard layouts (headers not under a single include root)?**  
A: Prefer to register the root(s) that third-party authors recommend for installation. If a package installs headers into multiple roots, each root can be registered separately. The top-level collision check considers relative paths — if multiple roots truly overlap required headers, the configure step will detect and fail.

**Q: Should the staging flatten include contents?**  
A: Yes: copy directory roots into `<build>/install/include/` preserving subfolders. Consumers should include with `#include <fmt/format.h>` etc. Our policy is to *not* further flatten subdirs — only the set of roots is merged under `install/include/`.

**Q: What if two third parties install the same header relative path?**  
A: That is a fatal error at configure time — the project must resolve provider overlap. This is intentional to avoid surprising header shadowing at runtime.

---

## Example top-level pseudocode for staging (summary)

1. After third-party scripts run, `THIRD_PARTY_POST_BUILD_HEADERS` and `THIRD_PARTY_POST_BUILD_LIBS` are populated (absolute include dirs and concrete target names respectively).
2. Configure-time:
   - Deduplicate `THIRD_PARTY_POST_BUILD_HEADERS`.
   - Collision check of relative paths across them → FATAL if collision.
3. Create `stage_install` that:
   - Creates `install/bin`, `install/lib`, `install/include`.
   - Copies project binaries and headers.
   - Adds `add_custom_command(TARGET stage_install POST_BUILD copy_directory <each include-root> <install/include>)`.
   - Adds `add_custom_command(TARGET stage_install POST_BUILD copy $<TARGET_FILE:<lib-target>> <install/lib>)`.
   - After copies, run duplicate-basename check on `install/lib`.

---

## Closing notes

- Keep the alias/wrapper pattern: wrapper target + namespaced alias gives the project a stable, future-proof handle when upstream target names vary across versions or packaging choices.
- Keep helper utilities in `third_party/cmake/ThirdPartyPolicyAndHelper.cmake` so each package file stays short and consistent.
- Make the staging lists (`THIRD_PARTY_POST_BUILD_HEADERS`, `THIRD_PARTY_POST_BUILD_LIBS`) the *single* source of truth for top-level staging. Do not attempt to guess targets at the top-level — let each package declare what it actually provides.
