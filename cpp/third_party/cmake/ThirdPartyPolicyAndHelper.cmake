cmake_minimum_required(VERSION 3.18)
# --- Include Guard ---
# This ensures that the contents of this file are processed only once, even if
# it is included multiple times from different scripts. This prevents redundant
# processing and repeated message output.
if(THIRD_PARTY_POLICY_AND_HELPER_INCLUDED)
  return()
endif()
set(THIRD_PARTY_POLICY_AND_HELPER_INCLUDED TRUE)
# ---------------------------------------------------------------------------
# ThirdPartyPolicyAndHelper.cmake
#
# This file serves two primary purposes:
# 1.  It defines the central, user-configurable CACHE variables that govern
#     the behavior of all third-party builds (e.g., enabling installation,
#     disabling tests).
# 2.  It provides a suite of robust helper functions (`snapshot_cache_var`,
#     `_is_target_installable`, etc.) that individual package scripts
#     (like `fmt.cmake`) must use to ensure consistent and isolated builds.
#
# This script is included by the top-level `third_party/CMakeLists.txt` and
# its helpers are included by each package-specific `.cmake` file.
# ---------------------------------------------------------------------------

# --- General Build & Install Policies ---
option(THIRD_PARTY_ALLOW_UPSTREAM_PCH "Allow upstream projects to enable precompiled headers" OFF)
option(THIRD_PARTY_FORCE_ALLOW_PCH "Force upstream PCH even on MSVC+Ninja (may produce errors)" OFF)
option(THIRD_PARTY_DISABLE_TESTS "Globally disable tests for all third-party libraries." ON)

# --- Per-Library Build Policies ---

# These CACHE variables allow fine-grained control over specific libraries.
# They can be set from the command line (e.g., -DTHIRD_PARTY_FMT_FORCE_VARIANT=shared).

# Force build variant for {fmt}. Values: "static", "shared", "none".
set(THIRD_PARTY_FMT_FORCE_VARIANT "static" CACHE STRING "Force fmt build variant: none|shared|static")

# Force build variant for libzmq. Values: "static", "shared", "none".
set(THIRD_PARTY_ZMQ_FORCE_VARIANT "static" CACHE STRING "Force libzmq build variant: none|shared|static")

# --- libzmq Specific Tunables ---
option(PYLABHUB_ZMQ_WITH_OPENPGM "Enable OpenPGM transport in libzmq" OFF)
option(PYLABHUB_ZMQ_WITH_NORM "Enable NORM transport in libzmq" OFF)
option(PYLABHUB_ZMQ_WITH_VMCI "Enable VMCI transport in libzmq" OFF)

# Policy for nlohmann/json.
option(USE_VENDOR_NLOHMANN_ONLY "Force the build to use only the vendored nlohmann/json headers." ON)

# ----------------------------------------------------------------------------
# XOPToolkit / XOPSupport build options
# ----------------------------------------------------------------------------
# Prefer the project vendor copy; override by setting USE_SYSTEM_XOPSUPPORT to a path.
# Example to force system XOPSupport:
#   cmake -D USE_SYSTEM_XOPSUPPORT="/opt/XOPSupport" ...

# Single override path: if non-empty, FindXOPSupport will use this path and bypass vendor.
set(USE_SYSTEM_XOPSUPPORT "" CACHE PATH "Optional: path to system-installed XOPSupport (overrides vendor tree when set).")

# ----------------------------------------------------------------------------
# Helper macros for snapshotting and restoring cache variables.
#
# Design:
# When using `add_subdirectory()` to include a third-party project, that
# project can modify CMake variables (both normal and cached). These
# modifications can "leak" out and affect the parent project or other
# third-party builds, leading to unpredictable behavior.
#
# These macros provide a robust mechanism to create an isolated scope for a
# sub-project. They save the state of specified variables before including the
# sub-project and restore them afterward.
#
# Usage:
#   # 1. Snapshot variables that the sub-project might change.
#   snapshot_cache_var(BUILD_SHARED_LIBS)
#
#   # 2. Set options for the sub-project.
#   set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
#
#   # 3. Include the sub-project.
#   add_subdirectory(third_party/subproject)
#
#   # 4. Restore the variables to their original state.
#   restore_cache_var(BUILD_SHARED_LIBS BOOL)
#
# This ensures the sub-project's build settings do not affect the main build.
#

# --- snapshot_cache_var(VAR_NAME) ---
#
# Behavior:
#   Saves the current state of a variable named <VAR_NAME>. It captures:
#   1. Whether a normal (non-cached) variable of that name exists and its value.
#   2. Whether a cached variable of that name exists and its value.
#   This state is stored in internal `_save_*` variables for later use by
#   `restore_cache_var`.
macro(snapshot_cache_var _var)
  # --- Capture the state of the normal (non-cached) variable ---
  if(DEFINED ${_var})
    set(_save_${_var}_was_defined TRUE)
    set(_save_${_var}_value "${${_var}}")
  else()
    set(_save_${_var}_was_defined FALSE)
  endif()

  # --- Capture the state of the CACHE variable ---
  # Requires CMake 3.18+ for `DEFINED CACHE{...}`
  if(DEFINED CACHE{${_var}})
    set(_save_${_var}_was_in_cache TRUE)
    # The cached value is available as a normal variable expansion.
    set(_save_${_var}_cache_value "${${_var}}")
  else()
    set(_save_${_var}_was_in_cache FALSE)
  endif()
endmacro()

# --- restore_cache_var(VAR_NAME CACHE_TYPE) ---
#
# Behavior:
#   Restores a variable named <VAR_NAME> to the state captured by
#   `snapshot_cache_var`. It correctly handles all cases:
#   - If the variable was originally cached, its cached value is restored.
#   - If it was not cached, any cache entry created by the sub-project is removed.
#   - If it was a normal variable, its value is restored.
#   - If it did not exist at all, it is unset.
#
# Arguments:
#   _var: The name of the variable to restore.
#   _cache_type: The expected type (e.g., BOOL, STRING, PATH) if the variable
#                needs to be re-added to the cache. This is required by the
#                `set(... CACHE ...)` command.
macro(restore_cache_var _var _cache_type)
  # --- Restore the CACHE variable state ---
  if(_save_${_var}_was_in_cache) # This is safe as it's always TRUE or FALSE
    # The variable was originally in the cache. Restore it with its saved value.
    # We use FORCE to ensure we overwrite any changes made by the sub-project.
    if(DEFINED _save_${_var}_cache_value) # Explicitly check if a value was saved
      set(${_var} "${_save_${_var}_cache_value}" CACHE ${_cache_type} "" FORCE)
    else()
      # This case is unlikely (cached but undefined value), but for robustness,
      # we restore it as an empty cached variable.
      set(${_var} "" CACHE ${_cache_type} "" FORCE) # Re-create with empty value
    endif()
  else()
    # The variable was NOT originally in the cache.
    # If the sub-project added it to the cache, we must remove it to prevent leakage.
    if(DEFINED CACHE{${_var}}) # Strict check
      unset(${_var} CACHE)
    endif()
  endif()

  # --- Restore the normal (non-cached) variable state ---
  if(_save_${_var}_was_defined) # This is safe as it's always TRUE or FALSE
    # The variable existed as a normal variable. Restore its value.
    if(DEFINED _save_${_var}_value) # Explicitly check if a value was saved
      set(${_var} "${_save_${_var}_value}")
    else()
      # It existed but was empty. Unset it to be safe.
      unset(${_var})
    endif()
  else()
    # The variable did not exist as a normal variable. Ensure it is unset now.
    unset(${_var})
  endif()

  # --- Clean up the temporary snapshot variables to keep the CMake scope clean ---
  unset(_save_${_var}_was_defined)
  unset(_save_${_var}_value)
  unset(_save_${_var}_was_in_cache)
  unset(_save_${_var}_cache_value)
endmacro()

# --- _resolve_alias_to_concrete(TARGET_NAME OUT_VAR) ---
#
# Design:
#   Recursively resolves an `ALIAS` target to its underlying concrete target.
#   This is useful because third-party projects often provide namespaced
#   aliases (e.g., `fmt::fmt`) that point to the real target (e.g., `fmt`).
#   To get the physical file path for staging, we need the name of the real target.
#
# Behavior:
#   - If <TARGET_NAME> is an alias, it follows the `ALIASED_TARGET` property
#     until it finds a target that is not an alias.
#   - If <TARGET_NAME> is not an alias, it returns the original name.
#   - The final, concrete target name is stored in <OUT_VAR>.
#
# Usage:
#   _resolve_alias_to_concrete("fmt::fmt" real_fmt_target)
#   # real_fmt_target will now contain "fmt"
#
function(_resolve_alias_to_concrete target_name out_var)
  set(resolved_target "${target_name}")
  set(original_target "${target_name}")
  while(TARGET "${resolved_target}")
    get_target_property(aliased_target "${resolved_target}" ALIASED_TARGET)
    # The `if(aliased_target)` check is the robust, idiomatic way to do this.
    # If the ALIASED_TARGET property does not exist, get_target_property sets the
    # variable to a value ending in "-NOTFOUND", which CMake's if() command evaluates as FALSE.
    if(aliased_target)
      set(resolved_target "${aliased_target}")
    else()
      break() # Found the concrete target, exit the loop.
    endif()
  endwhile()
  if(NOT "${original_target}" STREQUAL "${resolved_target}")
    message(STATUS "[pylabhub-third-party] Resolved alias '${original_target}' to concrete target '${resolved_target}'.")
  endif()
  set(${out_var} "${resolved_target}" PARENT_SCOPE)
endfunction()

# --- _expose_wrapper(WRAPPER_NAME NAMESPACE_ALIAS) ---
#
# Design:
#   Creates the standard two-layer abstraction for a third-party library:
#   1. A non-namespaced `INTERFACE` wrapper target (e.g., `pylabhub_fmt`).
#   2. A namespaced `ALIAS` target for consumers (e.g., `pylabhub::third_party::fmt`).
#   This provides a stable, consistent naming convention for all dependencies,
#   decoupling consumers from the implementation details of the third-party build.
#
# Behavior:
#   - Creates the `INTERFACE` target `pylabhub_<pkg>` if it doesn't exist.
#   - Creates the `ALIAS` target `pylabhub::third_party::<pkg>` pointing to the
#     wrapper if it doesn't exist.
#
# Usage:
#   _expose_wrapper(pylabhub_fmt pylabhub::third_party::fmt)
#
function(_expose_wrapper wrapper_name namespace_alias)
  if(NOT TARGET ${wrapper_name})
    add_library(${wrapper_name} INTERFACE)
    message(STATUS "[pylabhub-third-party] Created INTERFACE wrapper target: ${wrapper_name}")
  endif()
  if(NOT TARGET ${namespace_alias})
    add_library(${namespace_alias} ALIAS ${wrapper_name})
    message(STATUS "[pylabhub-third-party] Created ALIAS target: ${namespace_alias} -> ${wrapper_name}")
  endif()
endfunction()

# --- Final Policy Summary ---
# Print a summary of the configured policies for easy debugging.
message(STATUS "=========================================================")
message(STATUS "[pylabhub-third-party] Global Policies:")
message(STATUS "  - Create Install Target:${PYLABHUB_CREATE_INSTALL_TARGET}")
message(STATUS "  - Stage 3rd-Party:      ${THIRD_PARTY_INSTALL}")
message(STATUS "  - Disable tests:        ${THIRD_PARTY_DISABLE_TESTS}")
message(STATUS "  - Allow upstream PCH:   ${THIRD_PARTY_ALLOW_UPSTREAM_PCH}")
message(STATUS "[pylabhub-third-party] Per-Library Policies:")
message(STATUS "  - fmt variant:          ${THIRD_PARTY_FMT_FORCE_VARIANT}")
message(STATUS "  - libzmq variant:       ${THIRD_PARTY_ZMQ_FORCE_VARIANT}")
message(STATUS "  - libzmq with OpenPGM:  ${PYLABHUB_ZMQ_WITH_OPENPGM}")
message(STATUS "  - libzmq with NORM:     ${PYLABHUB_ZMQ_WITH_NORM}")
message(STATUS "  - libzmq with VMCI:     ${PYLABHUB_ZMQ_WITH_VMCI}")
message(STATUS "=========================================================")
message(STATUS "")

# ----------------------------------------------------------------------------
# End of ThirddParty Policy
# ----------------------------------------------------------------------------
