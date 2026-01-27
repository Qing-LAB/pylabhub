include(ThirdPartyPolicyAndHelper) # Ensure helpers are available.
# ---------------------------------------------------------------------------
# third_party/cmake/fmt.cmake
# Wrapper for fmt (third_party/fmt)
#
# Responsibilities:
#  - Configure the upstream fmt subproject in an isolated scope using the
#    snapshot/restore cache helpers.
#  - Discover the canonical concrete `fmt` library target.
#  - Provide a stable, namespaced `pylabhub::third_party::fmt` target for consumers.
#  - Stage the `fmt` headers and library artifacts into the project's
#    `PYLABHUB_STAGING_DIR` by attaching custom commands to the
# - PYLABHUB_STAGING_DIR: (PATH)
#    `stage_third_party_deps` target, as per the design document.
#  - Preserve your original wrapper flags (THIRD_PARTY_FMT_FORCE_VARIANT,
#    THIRD_PARTY_DISABLE_TESTS, THIRD_PARTY_ALLOW_UPSTREAM_PCH, etc.)
# ---------------------------------------------------------------------------
#
# This script is controlled by the following global options/variables,
# typically set in `third_party/cmake/ThirdPartyPolicyAndHelper.cmake` or
# at the top-level project:
#
# - THIRD_PARTY_FMT_FORCE_VARIANT: (string: "static" | "shared" | "none")
#   Forces the build to be static or shared. As a CACHE variable, this can be
#   set from the cmake command line.
#
# - USE_FMT_HEADER_ONLY: (BOOL: ON | OFF)
#   If defined and ON, forces a header-only configuration for fmt. This is
#   typically set as a normal variable before including the third-party directory.
#
# - THIRD_PARTY_DISABLE_TESTS: (CACHE BOOL: ON | OFF)
#   If ON, disables the build of fmt's internal tests.
#
# - THIRD_PARTY_ALLOW_UPSTREAM_PCH: (CACHE BOOL: ON | OFF)
#   If OFF (default), disables fmt's precompiled header options.
#
# - THIRD_PARTY_INSTALL: (CACHE BOOL: ON | OFF)
#   If ON, enables the post-build staging of fmt artifacts.
#
# Internal Build Control & Isolation:
# This script uses `snapshot_cache_var` and `restore_cache_var` to create an isolated
# build environment for the fmt subproject. It translates the high-level
# options above into the specific CACHE variables that the fmt build system
# understands (e.g., `BUILD_SHARED_LIBS`, `FMT_TEST`). Any pre-existing global
# values for these variables are saved before `add_subdirectory(fmt)` is called
# and restored immediately after, preventing the fmt build settings from
# "leaking" and affecting the main project or other dependencies.
#
#
# The following are provided by the top-level build environment and are not
# intended to be user-configurable options:
#
# - THIRD_PARTY_STAGING_DIR: (PATH)
#   The absolute path to the staging directory where artifacts will be copied.
#
# - stage_third_party_deps: (CMake Target)
#   The custom target "hook" to which staging commands are attached.
# ---------------------------------------------------------------------------

# Include the top-level staging helpers. The path is added by the root CMakeLists.txt.
include(StageHelpers)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fmt/CMakeLists.txt")
  message(STATUS "[pylabhub-third-party] Configuring fmt submodule...")

  # --- 1. Snapshot cache variables ---
  # Save the state of any variables the fmt build might modify.
  # ---------------------------
  snapshot_cache_var(BUILD_SHARED)
  snapshot_cache_var(BUILD_STATIC)
  snapshot_cache_var(BUILD_SHARED_LIBS)
  snapshot_cache_var(BUILD_TESTS)
  snapshot_cache_var(ENABLE_PRECOMPILED)

  snapshot_cache_var(FMT_TEST)
  snapshot_cache_var(FMT_INSTALL)       # Snapshot upstream's install option to ensure it's isolated.
  snapshot_cache_var(FMT_HEADER_ONLY)
  
  # --- 2. Configure the fmt sub-project build ---
  # Variant / build options (preserve original semantics)
  # ---------------------------
  if(NOT DEFINED THIRD_PARTY_FMT_FORCE_VARIANT)
    set(THIRD_PARTY_FMT_FORCE_VARIANT "none")
  endif()

  if(THIRD_PARTY_FMT_FORCE_VARIANT STREQUAL "static")
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Wrapper: prefer static libs for fmt (generic hint)" FORCE)
    set(BUILD_SHARED OFF CACHE BOOL "Wrapper: prefer static libs for fmt (redundant)" FORCE)
    set(BUILD_STATIC ON CACHE BOOL "Wrapper: prefer static libs for fmt (redundant)" FORCE)
    message(STATUS "[pylabhub-third-party] Forcing fmt variant = static")
  elseif(THIRD_PARTY_FMT_FORCE_VARIANT STREQUAL "shared")
    set(BUILD_SHARED_LIBS ON CACHE BOOL "Wrapper: prefer shared libs for fmt (generic hint)" FORCE)
    set(BUILD_SHARED ON CACHE BOOL "Wrapper: prefer shared libs for fmt (redundant)" FORCE)
    set(BUILD_STATIC OFF CACHE BOOL "Wrapper: prefer shared libs for fmt (redundant)" FORCE)
    message(STATUS "[pylabhub-third-party] Forcing fmt variant = shared")
  else()
    message(STATUS "[pylabhub-third-party] Not forcing fmt variant (using upstream default)")
  endif()

  # Honor top-level USE_FMT_HEADER_ONLY if present (map to upstream FMT_HEADER_ONLY).
  if(DEFINED USE_FMT_HEADER_ONLY AND USE_FMT_HEADER_ONLY)
    set(FMT_HEADER_ONLY ON CACHE BOOL "Wrapper: honor USE_FMT_HEADER_ONLY (scoped to fmt)" FORCE)
    message(STATUS "[pylabhub-third-party] Honoring USE_FMT_HEADER_ONLY => FMT_HEADER_ONLY=ON")
  endif()

  # Optionally disable only fmt's tests (do NOT set global BUILD_TESTS).
  if(THIRD_PARTY_DISABLE_TESTS)
    set(FMT_TEST OFF CACHE BOOL "Wrapper: disable fmt tests only" FORCE)
    message(STATUS "[pylabhub-third-party] Disabling fmt tests (FMT_TEST=OFF)")
  endif()

  # ---------------------------
  # Precompiled header handling (preserve original intent)
  # ---------------------------
  if(NOT DEFINED THIRD_PARTY_ALLOW_UPSTREAM_PCH)
    set(THIRD_PARTY_ALLOW_UPSTREAM_PCH OFF)
  endif()

  if(NOT THIRD_PARTY_ALLOW_UPSTREAM_PCH)
    set(ENABLE_PRECOMPILED OFF CACHE BOOL "Wrapper: disable fmt precompiled headers (scoped)" FORCE)
    # The fmt build does not use these variables, but we set them for robustness
    # in case future versions do.
    # set(FMT_USE_PRECOMPILED_HEADER OFF CACHE BOOL "Wrapper: disable fmt precompiled header (alt)" FORCE)
    # set(FMT_USE_PCH OFF CACHE BOOL "Wrapper: disable fmt precompiled header (alt2)" FORCE)
    message(STATUS "[pylabhub-third-party] Disabling fmt PCH")
  else()
    message(STATUS "[pylabhub-third-party] Allowing fmt to use PCH if its build requests it")
  endif()

  # --- 3. Add the fmt subproject ---
  # All CACHE variables set above will be snapshotted and only apply to this subdirectory.
  add_subdirectory(fmt EXCLUDE_FROM_ALL)

  # --- 4. Discover the canonical concrete target ---
  # Find the real, installable target for the fmt library, ignoring aliases.
  # ---------------------------
  set(_fmt_canonical_target "")
  set(_fmt_candidates fmt fmt::fmt) # Preference order
  foreach(_cand IN LISTS _fmt_candidates)
    if(TARGET "${_cand}" AND NOT TARGET "${_cand}" STREQUAL "INTERFACE")
      _resolve_alias_to_concrete("${_cand}" _fmt_canonical_target)
      message(STATUS "[pylabhub-third-party] Found canonical fmt target: ${_fmt_canonical_target} (from candidate '${_cand}')")
      break()
    endif()
  endforeach()

  # --- 5. Create wrapper target and define usage requirements ---
  # This provides a stable `pylabhub::third_party::fmt` for consumers.
  # ---------------------------
  _expose_wrapper(pylabhub_fmt pylabhub::third_party::fmt)

  if(_fmt_canonical_target)
    # Binary library case: Link the wrapper to the concrete target.
    # This transitively provides include directories and link information.
    target_link_libraries(pylabhub_fmt INTERFACE "${_fmt_canonical_target}")
    message(STATUS "[pylabhub-third-party] Linking pylabhub_fmt -> ${_fmt_canonical_target}")
  else()
    # Header-only case: Manually add include directories to the wrapper.
    message(STATUS "[pylabhub-third-party] No canonical binary target found for fmt. Configuring as header-only.")
    target_include_directories(pylabhub_fmt INTERFACE
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/fmt/include>
      $<INSTALL_INTERFACE:include>
    )
  endif()

  # --- 6. Stage artifacts for installation ---
  # Add custom commands to copy headers and libs to the staging directory.
  # ---------------------------
  if(THIRD_PARTY_INSTALL)
    message(STATUS "[pylabhub-third-party] Scheduling fmt artifacts for staging...")

    # Stage the header directory
    pylabhub_stage_headers(
      # Stage the fmt headers. The source directory is '.../fmt/include', and the
      # SUBDIR "" argument ensures its contents are copied directly into the
      # staging include directory.
      DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/fmt/include"
      SUBDIR ""
    )

    # Stage the library file, if a concrete target was found
    if(_fmt_canonical_target)
      pylabhub_register_library_for_staging(TARGET ${_fmt_canonical_target})
    endif()
  else()
    message(STATUS "[pylabhub-third-party] THIRD_PARTY_INSTALL is OFF; skipping staging for fmt.")
  endif()

  # --- 7. Add to export set for installation ---
  # The wrapper target must be exported so downstream projects can find it.
  install(TARGETS pylabhub_fmt
    EXPORT pylabhubTargets
  )

  # If fmt was built as a library, its canonical target must also be exported.
  if(_fmt_canonical_target)
    install(TARGETS ${_fmt_canonical_target}
      EXPORT pylabhubTargets
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
      PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_PREFIX}
    )
  endif()

  # --- 8. Restore cache variables ---
  # Prevent settings from leaking to other sub-projects.
  # ---------------------------
  restore_cache_var(BUILD_SHARED BOOL)
  restore_cache_var(BUILD_STATIC BOOL)
  restore_cache_var(BUILD_SHARED_LIBS BOOL)
  restore_cache_var(BUILD_TESTS BOOL)
  restore_cache_var(ENABLE_PRECOMPILED BOOL)

  restore_cache_var(FMT_TEST BOOL)
  restore_cache_var(FMT_INSTALL BOOL)
  restore_cache_var(FMT_HEADER_ONLY BOOL)

else()
  message(WARNING "[pylabhub-third-party] fmt submodule not found at ${CMAKE_CURRENT_SOURCE_DIR}/fmt. Skipping configuration.")
endif()
