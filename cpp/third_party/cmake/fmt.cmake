# ---------------------------------------------------------------------------
# third_party/cmake/fmt.cmake
# Wrapper for fmt (third_party/fmt)
#
# Purpose & high-level policy:
#  - Provide a stable handle pylabhub::fmt for downstream linkage.
#  - Avoid creating aliases that point to upstream aliases (alias-of-alias).
#    Instead, always create a local concrete wrapper target (pylabhub_fmt)
#    and link that wrapper to any upstream fmt target (fmt, fmt::fmt, etc.).
#  - The wrapper *owns* installation into the project's install tree when
#    THIRD_PARTY_INSTALL (global intent) and FMT_INSTALL (wrapper intent) are set.
#    We will not rely on the fmt subproject to run its own install rules.
#  - Preserve your original wrapper flags (THIRD_PARTY_FMT_FORCE_VARIANT,
#    THIRD_PARTY_DISABLE_TESTS, THIRD_PARTY_ALLOW_UPSTREAM_PCH, etc.)
# ---------------------------------------------------------------------------

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fmt/CMakeLists.txt")
  message(STATUS "third_party: fmt submodule found (scoped handling)")

  # ---------------------------
  # Snapshot cache variables we may touch for fmt so we can restore them later.
  # snapshot_cache_var/restore_cache_var are assumed to be provided by your helper
  # infrastructure (they were present in original file).
  # ---------------------------
  snapshot_cache_var(BUILD_SHARED)
  snapshot_cache_var(BUILD_STATIC)
  snapshot_cache_var(BUILD_SHARED_LIBS)
  snapshot_cache_var(BUILD_TESTS)
  snapshot_cache_var(ENABLE_PRECOMPILED)

  snapshot_cache_var(FMT_TEST)
  snapshot_cache_var(FMT_INSTALL)       # NOTE: in this wrapper FMT_INSTALL means "wrapper should install"
  snapshot_cache_var(FMT_HEADER_ONLY)

  # ---------------------------
  # Variant / build options (preserve original semantics)
  # ---------------------------
  if(NOT DEFINED THIRD_PARTY_FMT_FORCE_VARIANT)
    set(THIRD_PARTY_FMT_FORCE_VARIANT "none")
  endif()

  if(THIRD_PARTY_FMT_FORCE_VARIANT STREQUAL "static")
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Wrapper: prefer static libs for fmt (generic hint)" FORCE)
    set(BUILD_SHARED OFF CACHE BOOL "Wrapper: prefer static libs for fmt" FORCE)
    set(BUILD_STATIC ON CACHE BOOL "Wrapper: prefer static libs for fmt" FORCE)
    message(STATUS "third_party wrapper: forcing fmt variant = static (BUILD_SHARED_LIBS=OFF)")
  elseif(THIRD_PARTY_FMT_FORCE_VARIANT STREQUAL "shared")
    set(BUILD_SHARED_LIBS ON CACHE BOOL "Wrapper: prefer shared libs for fmt (generic hint)" FORCE)
    set(BUILD_SHARED ON CACHE BOOL "Wrapper: prefer shared libs for fmt" FORCE)
    set(BUILD_STATIC OFF CACHE BOOL "Wrapper: prefer shared libs for fmt" FORCE)
    message(STATUS "third_party wrapper: forcing fmt variant = shared (BUILD_SHARED_LIBS=ON)")
  else()
    message(STATUS "third_party wrapper: not forcing fmt variant (none)")
  endif()

  # Honor top-level USE_FMT_HEADER_ONLY if present (map to upstream FMT_HEADER_ONLY).
  if(DEFINED USE_FMT_HEADER_ONLY AND USE_FMT_HEADER_ONLY)
    set(FMT_HEADER_ONLY ON CACHE BOOL "Wrapper: honor USE_FMT_HEADER_ONLY (scoped to fmt)" FORCE)
    message(STATUS "third_party wrapper: honoring USE_FMT_HEADER_ONLY => FMT_HEADER_ONLY=ON")
  endif()

  # Optionally disable only fmt's tests (do NOT set global BUILD_TESTS).
  if(THIRD_PARTY_DISABLE_TESTS)
    set(FMT_TEST OFF CACHE BOOL "Wrapper: disable fmt tests only" FORCE)
    set(FMT_TEST OFF CACHE BOOL "Wrapper: disable fmt tests only (duplicate safe set)" FORCE)
    message(STATUS "third_party wrapper: disabling fmt tests only (FMT_TEST=OFF)")
  endif()

  # Map installation intent for fmt to wrapper-level variable FMT_INSTALL.
  # Important: In this wrapper, FMT_INSTALL means "we (the wrapper) should
  # install fmt headers/libs into our project's install tree".
  # It does NOT mean "ask the fmt subproject to run its own install() rules".
  if(THIRD_PARTY_INSTALL)
    # If top-level install intent is true, enable wrapper-managed install by default.
    # (This matches original logic where FMT_INSTALL was mapped from THIRD_PARTY_INSTALL.)
    set(FMT_INSTALL ON CACHE BOOL "Wrapper: enable fmt install because THIRD_PARTY_INSTALL=ON" FORCE)
  else()
    set(FMT_INSTALL OFF CACHE BOOL "Wrapper: disable fmt install (wrapper default)" FORCE)
  endif()

  # ---------------------------
  # Precompiled header handling (preserve original intent)
  # ---------------------------
  if(NOT DEFINED THIRD_PARTY_ALLOW_UPSTREAM_PCH)
    set(THIRD_PARTY_ALLOW_UPSTREAM_PCH OFF)
  endif()

  if(NOT THIRD_PARTY_ALLOW_UPSTREAM_PCH)
    set(ENABLE_PRECOMPILED OFF CACHE BOOL "Wrapper: disable fmt precompiled headers (scoped)" FORCE)
    set(FMT_USE_PRECOMPILED_HEADER OFF CACHE BOOL "Wrapper: disable fmt precompiled header (alt)" FORCE)
    set(FMT_USE_PCH OFF CACHE BOOL "Wrapper: disable fmt precompiled header (alt2)" FORCE)
    message(STATUS "third_party wrapper: attempted to disable fmt PCH (scoped)")
  else()
    message(STATUS "third_party wrapper: allowing fmt upstream PCH if upstream requests it")
  endif()

  # ---------------------------
  # Add fmt subproject (scoped)
  # ---------------------------
  add_subdirectory(fmt EXCLUDE_FROM_ALL)

  # ---------------------------
  # Create a stable local wrapper and public alias
  # ---------------------------
  # Always create a concrete local wrapper target (no '::' in the real target name).
  # Downstream will use pylabhub::fmt which is an alias to this wrapper.
  # This pattern avoids alias-of-alias issues and lets us control include dirs.
  if(NOT TARGET pylabhub_fmt)
    add_library(pylabhub_fmt INTERFACE)
  endif()

  # Prefer to link our wrapper to any upstream-provided fmt target rather than aliasing
  # directly to avoid alias-of-alias problems.
  if(TARGET fmt) # some fmt versions export a plain "fmt" target
    message(STATUS "third_party: upstream provided target 'fmt' detected; linking pylabhub_fmt -> fmt")
    target_link_libraries(pylabhub_fmt INTERFACE fmt)
    set(_upstream_fmt_candidate "fmt")
  elseif(TARGET fmt::fmt) # common modern exported name
    message(STATUS "third_party: upstream provided target 'fmt::fmt' detected; linking pylabhub_fmt -> fmt::fmt")
    target_link_libraries(pylabhub_fmt INTERFACE fmt::fmt)
    set(_upstream_fmt_candidate "fmt::fmt")
  elseif(TARGET fmt::fmt-header-only)
    message(STATUS "third_party: upstream provided target 'fmt::fmt-header-only' detected; linking pylabhub_fmt -> fmt::fmt-header-only")
    target_link_libraries(pylabhub_fmt INTERFACE fmt::fmt-header-only)
    set(_upstream_fmt_candidate "fmt::fmt-header-only")
  else()
    # Fallback: if fmt was configured header-only but didn't create a target,
    # attempt to find include dir variables or vendored include layout and set includes.
    if(DEFINED FMT_HEADER_ONLY AND FMT_HEADER_ONLY)
      if(DEFINED FMT_INCLUDE_DIR AND EXISTS "${FMT_INCLUDE_DIR}")
        target_include_directories(pylabhub_fmt INTERFACE
          $<BUILD_INTERFACE:${FMT_INCLUDE_DIR}>
          $<INSTALL_INTERFACE:include>
        )
        message(STATUS "third_party: created pylabhub_fmt INTERFACE -> FMT_INCLUDE_DIR (${FMT_INCLUDE_DIR})")
      else()
        set(_fmt_include_guess "${CMAKE_CURRENT_SOURCE_DIR}/fmt/include")
        if(EXISTS "${_fmt_include_guess}")
          target_include_directories(pylabhub_fmt INTERFACE
            $<BUILD_INTERFACE:${_fmt_include_guess}>
            $<INSTALL_INTERFACE:include>
          )
          message(STATUS "third_party: created pylabhub_fmt INTERFACE -> ${_fmt_include_guess}")
        else()
          message(STATUS "third_party: no upstream fmt target and no include dir found for header-only fallback")
        endif()
      endif()
    endif()
  endif()

  # Expose the public alias to our wrapper so downstream consumers can use pylabhub::fmt
  if(NOT TARGET pylabhub::fmt)
    add_library(pylabhub::fmt ALIAS pylabhub_fmt)
    message(STATUS "third_party: created alias pylabhub::fmt -> pylabhub_fmt")
  else()
    message(STATUS "third_party: pylabhub::fmt already exists")
  endif()

  # Export a variable for legacy/explicit consumers (only if alias exists)
  if(TARGET pylabhub::fmt)
    set(FMT_TARGET "pylabhub::fmt" PARENT_SCOPE)
  else()
    set(FMT_TARGET "" PARENT_SCOPE)
    message(STATUS "third_party: pylabhub::fmt target not available; FMT_TARGET set empty")
  endif()

  # Print summary of fmt configuration (useful for debugging)
  if(TARGET pylabhub::fmt)
    message("")
    message("==========================================================")
    message("third_party: build pylabhub::fmt with the following settings:")
    message(STATUS "  BUILD_SHARED_LIBS=${BUILD_SHARED_LIBS}")
    message(STATUS "  FMT_HEADER_ONLY=${FMT_HEADER_ONLY}")
    message(STATUS "  FMT_INSTALL=${FMT_INSTALL}  # wrapper-managed install intent")
    message(STATUS "  FMT_TEST=${FMT_TEST}")
    message(STATUS "  ENABLE_PRECOMPILED=${ENABLE_PRECOMPILED}")
    message(STATUS "  BUILD_STATIC=${BUILD_STATIC}")
    message(STATUS "  BUILD_SHARED=${BUILD_SHARED}")
    message(STATUS "  USE_FMT_HEADER_ONLY=${USE_FMT_HEADER_ONLY}")
    message("==========================================================")
    message("")
  endif()

  # ---------------------------
  # Installation handling (wrapper-managed)
  # Invariant: FMT_INSTALL in this wrapper means "we will install fmt headers/libs
  # into our project's install tree". We do not rely on fmt subproject calling install().
  # ---------------------------
  if(THIRD_PARTY_INSTALL AND FMT_INSTALL)
    # --- 1) Install headers (always attempt to install headers) ---
    # Preference order for header location:
    #  1. FMT_INCLUDE_DIR (upstream-provided variable)
    #  2. vendored include under third_party/fmt/include
    #  3. FetchContent source include (fmt_SOURCE_DIR or FMT_SOURCE_DIR)
    set(_fmt_header_installed FALSE)

    if(DEFINED FMT_INCLUDE_DIR AND EXISTS "${FMT_INCLUDE_DIR}")
      message(STATUS "third_party: installing fmt headers from FMT_INCLUDE_DIR (${FMT_INCLUDE_DIR})")
      install(DIRECTORY "${FMT_INCLUDE_DIR}/" DESTINATION include COMPONENT devel FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")
      set(_fmt_header_installed TRUE)
    else()
      set(_fmt_vendored_include "${CMAKE_CURRENT_SOURCE_DIR}/fmt/include")
      if(EXISTS "${_fmt_vendored_include}")
        message(STATUS "third_party: installing vendored fmt headers from ${_fmt_vendored_include}")
        install(DIRECTORY "${_fmt_vendored_include}/" DESTINATION include COMPONENT devel FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")
        set(_fmt_header_installed TRUE)
      else()
        # Try FetchContent-style variables if present
        if(DEFINED fmt_SOURCE_DIR AND EXISTS "${fmt_SOURCE_DIR}/include")
          set(_fmt_fc_include "${fmt_SOURCE_DIR}/include")
        elseif(DEFINED FMT_SOURCE_DIR AND EXISTS "${FMT_SOURCE_DIR}/include")
          set(_fmt_fc_include "${FMT_SOURCE_DIR}/include")
        else()
          set(_fmt_fc_include "")
        endif()

        if(_fmt_fc_include)
          message(STATUS "third_party: installing fmt headers from fetched source ${_fmt_fc_include}")
          install(DIRECTORY "${_fmt_fc_include}/" DESTINATION include COMPONENT devel FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")
          set(_fmt_header_installed TRUE)
        endif()
      endif()
    endif()

    if(NOT _fmt_header_installed)
      message(STATUS "third_party: warning: no fmt headers found to install (no FMT_INCLUDE_DIR, vendored include, or fetched include).")
      # We continue: libraries might still be present, but headers are missing (unlikely).
    endif()

    # --- 2) Attempt to install library artifacts ---
    # We prefer install(TARGETS ...) when a concrete (non-ALIAS, non-INTERFACE) target exists.
    # Otherwise, fall back to searching the build tree for library artifacts and install them with install(FILES ...).
    set(_thirdparty_fmt_targets_to_install "")

    # Candidate upstream target names to check (common variations)
    foreach(_cand IN ITEMS fmt fmt::fmt fmt::fmt-header-only)
      if(_cand)
        # Check if candidate target is concrete and installable
        _is_target_installable("${_cand}" _cand_installable)
        if(_cand_installable)
          list(APPEND _thirdparty_fmt_targets_to_install ${_cand})
        endif()
      endif()
    endforeach()

    # If we found concrete upstream targets, install them (with PUBLIC_HEADER DESTINATION).
    if(_thirdparty_fmt_targets_to_install)
      # install(TARGETS ...) on concrete targets; add PUBLIC_HEADER DESTINATION to silence CMake dev warning.
      install(TARGETS ${_thirdparty_fmt_targets_to_install}
        EXPORT pylabhubThirdPartyTargets
        INCLUDES DESTINATION include
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
        PUBLIC_HEADER DESTINATION include/fmt
      )
      message(STATUS "third_party: installed fmt targets via install(TARGETS ...): ${_thirdparty_fmt_targets_to_install}")
    else()
      # No concrete target available. Fallback: search common build output locations for libfmt artifacts
      # and install them into lib/ (so your project's install layout is satisfied even when upstream
      # did not provide concrete, installable CMake targets).
      set(_fmt_lib_found_files "")

      # Search patterns (adjust if your build layout is different).
      # Use CMAKE_CURRENT_BINARY_DIR as primary search root; also look in "fmt" subfolder outputs.
      file(GLOB _fmt_search_a RELATIVE "${CMAKE_CURRENT_BINARY_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}/fmt/**/libfmt*.a"
        "${CMAKE_CURRENT_BINARY_DIR}/**/libfmt*.a"
        "${CMAKE_CURRENT_BINARY_DIR}/libfmt*.a"
      )
      foreach(_f IN LISTS _fmt_search_a)
        list(APPEND _fmt_lib_found_files "${CMAKE_CURRENT_BINARY_DIR}/${_f}")
      endforeach()

      file(GLOB _fmt_search_so RELATIVE "${CMAKE_CURRENT_BINARY_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}/fmt/**/libfmt*.so"
        "${CMAKE_CURRENT_BINARY_DIR}/**/libfmt*.so"
        "${CMAKE_CURRENT_BINARY_DIR}/libfmt*.so"
      )
      foreach(_f IN LISTS _fmt_search_so)
        list(APPEND _fmt_lib_found_files "${CMAKE_CURRENT_BINARY_DIR}/${_f}")
      endforeach()

      # Versioned so (libfmt.so.1.2.3)
      file(GLOB _fmt_search_so_ver RELATIVE "${CMAKE_CURRENT_BINARY_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}/fmt/**/libfmt*.so.*"
        "${CMAKE_CURRENT_BINARY_DIR}/**/libfmt*.so.*"
      )
      foreach(_f IN LISTS _fmt_search_so_ver)
        list(APPEND _fmt_lib_found_files "${CMAKE_CURRENT_BINARY_DIR}/${_f}")
      endforeach()

      # Windows-ish names / import libs / dlls
      file(GLOB _fmt_search_lib RELATIVE "${CMAKE_CURRENT_BINARY_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}/fmt/**/fmt*.lib"
        "${CMAKE_CURRENT_BINARY_DIR}/**/fmt*.lib"
        "${CMAKE_CURRENT_BINARY_DIR}/*.lib"
      )
      foreach(_f IN LISTS _fmt_search_lib)
        list(APPEND _fmt_lib_found_files "${CMAKE_CURRENT_BINARY_DIR}/${_f}")
      endforeach()

      file(GLOB _fmt_search_dll RELATIVE "${CMAKE_CURRENT_BINARY_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}/fmt/**/fmt*.dll"
        "${CMAKE_CURRENT_BINARY_DIR}/**/fmt*.dll"
      )
      foreach(_f IN LISTS _fmt_search_dll)
        list(APPEND _fmt_lib_found_files "${CMAKE_CURRENT_BINARY_DIR}/${_f}")
      endforeach()

      # Deduplicate and filter out non-existent entries (just in case)
      list(REMOVE_DUPLICATES _fmt_lib_found_files)
      set(_fmt_lib_files_existing "")
      foreach(_f IN LISTS _fmt_lib_found_files)
        if(EXISTS "${_f}")
          list(APPEND _fmt_lib_files_existing "${_f}")
        endif()
      endforeach()
      set(_fmt_lib_found_files ${_fmt_lib_files_existing})

      if(_fmt_lib_found_files)
        message(STATUS "third_party: found fmt library files to install: ${_fmt_lib_found_files}")
        install(FILES ${_fmt_lib_found_files} DESTINATION lib COMPONENT devel)
      else()
        message(STATUS "third_party: no fmt library artifacts found in build tree; nothing to install for libs")
      endif()
    endif()

    # Print final install summary for visibility
    message("")
    message("=========================================================")
    message(STATUS "INSTALL setup for fmt (wrapper-managed):")
    if(_fmt_header_installed)
      message(STATUS "  headers: installed to <install-prefix>/include")
    else()
      message(STATUS "  headers: NOT installed (none found)")
    endif()
    if(_thirdparty_fmt_targets_to_install)
      message(STATUS "  libraries: installed via install(TARGETS ...) -> lib/")
    elseif(_fmt_lib_found_files)
      message(STATUS "  libraries: installed via file fallback -> lib/")
    else()
      message(STATUS "  libraries: none installed")
    endif()
    message("=========================================================")
  else()
    # Either global wrapper-level install intent disabled or wrapper decided not to install.
    if(NOT THIRD_PARTY_INSTALL)
      message(STATUS "third_party: THIRD_PARTY_INSTALL is OFF; wrapper will not install fmt into project.")
    elseif(NOT FMT_INSTALL)
      message(STATUS "third_party: FMT_INSTALL is OFF (wrapper-managed); wrapper will not install fmt into project.")
    endif()
  endif() # if(THIRD_PARTY_INSTALL AND FMT_INSTALL)

  # ---------------------------
  # Restore previously-snapshotted cache variables so we do not leak settings
  # ---------------------------
  restore_cache_var(BUILD_SHARED BOOL)
  restore_cache_var(BUILD_STATIC BOOL)
  restore_cache_var(BUILD_SHARED_LIBS BOOL)
  restore_cache_var(BUILD_TESTS BOOL)
  restore_cache_var(ENABLE_PRECOMPILED BOOL)

  restore_cache_var(FMT_TEST BOOL)
  restore_cache_var(FMT_INSTALL BOOL)
  restore_cache_var(FMT_HEADER_ONLY BOOL)

else() # if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fmt")
  message(WARNING "third_party: fmt submodule not found at ${CMAKE_CURRENT_SOURCE_DIR}/fmt")
endif()

# ---------------------------------------------------------------------------
# pylabhub::fmt is the canonical target to use fmt
#
# Usage example:
#   if(TARGET pylabhub::fmt)
#     target_link_libraries(myexe PRIVATE pylabhub::fmt)
#   elseif(DEFINED FMT_TARGET AND FMT_TARGET)
#     target_link_libraries(myexe PRIVATE ${FMT_TARGET})
#   endif()
#
# ---------------------------------------------------------------------------
