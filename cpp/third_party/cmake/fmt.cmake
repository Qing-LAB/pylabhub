# ---------------------------------------------------------------------------
# third_party/cmake/fmt.cmake
# Setup fmt library
# Set up an INTERFACE target 'pylabhub::fmt' that uses the fmt submodule.
# ---------------------------------------------------------------------------
# The upstream fmt project defines and uses:
#   - options: FMT_TEST, FMT_INSTALL, FMT_DOC, FMT_MODULE, FMT_HEADER_ONLY, FMT_UNICODE, ...
#   - it honors BUILD_SHARED_LIBS to choose shared/static behavior.
# See fmt/CMakeLists.txt for exact option names. :contentReference[oaicite:2]{index=2}
#
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fmt/CMakeLists.txt")
  message(STATUS "third_party: fmt submodule found (scoped handling)")

  # Snapshot cache variables we may touch for fmt.
  snapshot_cache_var(BUILD_SHARED)
  snapshot_cache_var(BUILD_STATIC)
  snapshot_cache_var(BUILD_SHARED_LIBS)
  snapshot_cache_var(BUILD_TESTS)
  snapshot_cache_var(ENABLE_PRECOMPILED)

  # fmt-specific variables (match upstream names)
  snapshot_cache_var(FMT_TEST)
  snapshot_cache_var(FMT_INSTALL)
  snapshot_cache_var(FMT_HEADER_ONLY)

  # Map wrapper-level variant intent to variables upstream expects.
  if(NOT DEFINED THIRD_PARTY_FMT_FORCE_VARIANT)
    set(THIRD_PARTY_FMT_FORCE_VARIANT "none")
  endif()

  if(THIRD_PARTY_FMT_FORCE_VARIANT STREQUAL "static")
    # fmt uses BUILD_SHARED_LIBS as canonical way to select shared/static
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Wrapper: prefer static libs for fmt (generic hint)" FORCE)
    # also set BUILD_SHARED / BUILD_STATIC for downstreams that check them
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

  # Honor top-level USE_FMT_HEADER_ONLY if present: map to FMT_HEADER_ONLY for upstream.
  # Note: upstream fmt uses FMT_HEADER_ONLY internally to build and expose the header-only interface.
  if(DEFINED USE_FMT_HEADER_ONLY AND USE_FMT_HEADER_ONLY)
    set(FMT_HEADER_ONLY ON CACHE BOOL "Wrapper: honor USE_FMT_HEADER_ONLY (scoped to fmt)" FORCE)
    message(STATUS "third_party wrapper: honoring USE_FMT_HEADER_ONLY => FMT_HEADER_ONLY=ON")
  endif()

  # Optionally disable only fmt's tests (do NOT set global BUILD_TESTS).
  if(THIRD_PARTY_DISABLE_TESTS)
    # upstream fmt uses FMT_TEST to control test generation
    set(FMT_TEST OFF CACHE BOOL "Wrapper: disable fmt tests only" FORCE)
    # Also set generic hints (harmless) so other naming styles are covered.
    set(FMT_TEST OFF CACHE BOOL "Wrapper: disable fmt tests only (duplicate safe set)" FORCE)
    message(STATUS "third_party wrapper: disabling fmt tests only (FMT_TEST=OFF)")
  endif()

  # Map installation intent for fmt to upstream variable to keep behavior consistent.
  # If wrapper-level THIRD_PARTY_INSTALL is ON, enable fmt install; otherwise turn it off.
  if(THIRD_PARTY_INSTALL)
    set(FMT_INSTALL ON CACHE BOOL "Wrapper: enable fmt install because THIRD_PARTY_INSTALL=ON" FORCE)
  else()
    set(FMT_INSTALL OFF CACHE BOOL "Wrapper: disable fmt install (wrapper default)" FORCE)
  endif()

  # Scope precompiled-header handling to fmt-specific variables (do not alter others)
  if(NOT DEFINED THIRD_PARTY_ALLOW_UPSTREAM_PCH)
    set(THIRD_PARTY_ALLOW_UPSTREAM_PCH OFF)
  endif()
  if(NOT THIRD_PARTY_ALLOW_UPSTREAM_PCH)
    set(ENABLE_PRECOMPILED OFF CACHE BOOL "Wrapper: disable fmt precompiled headers (scoped)" FORCE)
    # fmt upstream does not use ZMQ-specific PCH vars; set common alternates if present
    set(FMT_USE_PRECOMPILED_HEADER OFF CACHE BOOL "Wrapper: disable fmt precompiled header (alt)" FORCE)
    set(FMT_USE_PCH OFF CACHE BOOL "Wrapper: disable fmt precompiled header (alt2)" FORCE)
    message(STATUS "third_party wrapper: attempted to disable fmt PCH (scoped)")
  else()
    message(STATUS "third_party wrapper: allowing fmt upstream PCH if upstream requests it")
  endif()

  # Add fmt subproject. It will read the variables we set above.
  # Use EXCLUDE_FROM_ALL to avoid pulling fmt into default builds unless explicitly used.
  add_subdirectory(fmt EXCLUDE_FROM_ALL)

  # ----------------------------------------------------------
  # Create canonical pylabhub::fmt alias or INTERFACE target for consumers
  #
  # Prefer upstream-provided CMake targets (common)
  if(TARGET fmt::fmt)
    if(NOT TARGET pylabhub::fmt)
      add_library(pylabhub::fmt ALIAS fmt::fmt)
      message(STATUS "third_party: created alias pylabhub::fmt -> fmt::fmt")
    endif()
  elseif(TARGET fmt::fmt-header-only)
    if(NOT TARGET pylabhub::fmt)
      add_library(pylabhub::fmt ALIAS fmt::fmt-header-only)
      message(STATUS "third_party: created alias pylabhub::fmt -> fmt::fmt-header-only")
    endif()
  else()
    # Defensive fallback: if fmt was configured header-only but upstream
    # did not create a target, create an INTERFACE target with include dirs.
    # Upstream fmt sometimes sets FMT_HEADER_ONLY and FMT_INCLUDE_DIR or FMT_INSTALL.
    if(DEFINED FMT_HEADER_ONLY AND FMT_HEADER_ONLY)
      if(NOT TARGET pylabhub::fmt)
        add_library(pylabhub::fmt INTERFACE)

        # Prefer an upstream-provided include dir variable if available.
        if(DEFINED FMT_INCLUDE_DIR AND EXISTS "${FMT_INCLUDE_DIR}")
          target_include_directories(pylabhub::fmt INTERFACE
            $<BUILD_INTERFACE:${FMT_INCLUDE_DIR}>
            $<INSTALL_INTERFACE:include>
          )
          message(STATUS "third_party: created INTERFACE pylabhub::fmt -> FMT_INCLUDE_DIR (${FMT_INCLUDE_DIR})")
        else()
          # best-effort detection: common include layout under fmt source tree
          set(_fmt_include_guess "${CMAKE_CURRENT_SOURCE_DIR}/fmt/include")
          if(EXISTS "${_fmt_include_guess}")
            target_include_directories(pylabhub::fmt INTERFACE
              $<BUILD_INTERFACE:${_fmt_include_guess}>
              $<INSTALL_INTERFACE:include>
            )
            message(STATUS "third_party: created INTERFACE pylabhub::fmt -> ${_fmt_include_guess}")
          endif()
        endif()
      endif()
    endif()
  endif()

  # Export a variable for legacy/explicit consumers (only if target created)
  if(TARGET pylabhub::fmt)
    set(FMT_TARGET "pylabhub::fmt" PARENT_SCOPE)
  else()
    # do not export an invalid name — provide empty value to indicate missing target
    set(FMT_TARGET "" PARENT_SCOPE)
    message(STATUS "third_party: pylabhub::fmt target not available; FMT_TARGET set empty")
  endif()

  if(TARGET pylabhub::fmt)
    message("")
    message("==========================================================")
    message("third_party: build ${FMT_TARGET} with the following settings:")
    message(STATUS "  BUILD_SHARED_LIBS=${BUILD_SHARED_LIBS}")
    message(STATUS "  FMT_HEADER_ONLY=${FMT_HEADER_ONLY}")
    message(STATUS "  FMT_INSTALL=${FMT_INSTALL}")
    message(STATUS "  FMT_TEST=${FMT_TEST}")
    message(STATUS "  ENABLE_PRECOMPILED=${ENABLE_PRECOMPILED}")
    message(STATUS "  BUILD_STATIC=${BUILD_STATIC}")
    message(STATUS "  BUILD_SHARED=${BUILD_SHARED}")
    message(STATUS "  USE_FMT_HEADER_ONLY=${USE_FMT_HEADER_ONLY}")
    message("==========================================================")
    message("")
  endif()

  # ----------------------------------------------------------
  # Setup installation
  # fmt install handler: install headers or trust upstream
  # ----------------------------------------------------------
  if(THIRD_PARTY_INSTALL)
    # If upstream fmt produced install rules (we set FMT_INSTALL), prefer that.
    if(DEFINED FMT_INSTALL AND FMT_INSTALL)
      message(STATUS "third_party: relying on upstream fmt install (FMT_INSTALL=ON).")
    else()
      # Upstream didn't create install rules — if we made an INTERFACE target
      # from a vendored include dir, install that include tree.
      if(TARGET pylabhub::fmt)
        # If pylabhub::fmt is INTERFACE and we have an include dir variable, install it
        if(TARGET pylabhub::fmt AND NOT TARGET fmt::fmt AND DEFINED FMT_INCLUDE_DIR AND EXISTS "${FMT_INCLUDE_DIR}")
          message(STATUS "third_party: installing fmt headers from ${FMT_INCLUDE_DIR}")
          install(DIRECTORY "${FMT_INCLUDE_DIR}/" DESTINATION include COMPONENT devel FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")
        else()
          # Best-effort fallback: vendored layout under third_party/fmt/include
          set(_fmt_vendored_include "${CMAKE_CURRENT_SOURCE_DIR}/fmt/include")
          if(EXISTS "${_fmt_vendored_include}")
            message(STATUS "third_party: installing vendored fmt headers from ${_fmt_vendored_include}")
            install(DIRECTORY "${_fmt_vendored_include}/" DESTINATION include COMPONENT devel FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")
          endif()
        endif()
      endif()
    endif()

    # Export the third-party targets into an export set so we can provide pylabhubThirdPartyTargets
    # (collect only real targets that are installable)
    set(_thirdparty_targets_to_install "")
    # If upstream provided an installable fmt target (fmt::fmt) and FMT_INSTALL was used, include it
    if(TARGET fmt::fmt)
      list(APPEND _thirdparty_targets_to_install fmt::fmt)
    elseif(TARGET fmt::fmt-header-only)
      list(APPEND _thirdparty_targets_to_install fmt::fmt-header-only)
    elseif(TARGET pylabhub::fmt AND (NOT TARGET fmt::fmt) AND (NOT TARGET fmt::fmt-header-only))
      # Can't install ALIAS; but if pylabhub::fmt is INTERFACE (headers only), we already installed headers above.
      # Nothing else to add here.
    endif()

    if(_thirdparty_targets_to_install)
      install(TARGETS ${_thirdparty_targets_to_install}
        EXPORT pylabhubThirdPartyTargets
        INCLUDES DESTINATION include
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
      )
    endif()

    message("")
    message("=========================================================")
    message(STATUS "INSTALL setup for fmt:")
    message(STATUS "third_party: ${_thirdparty_targets_to_install} install enabled")
    message("=========================================================")
  endif() # THIRD_PARTY_INSTALL


  # Restore previously-snapshotted cache variables so fmt's forced values do not leak.
  restore_cache_var(BUILD_SHARED BOOL)
  restore_cache_var(BUILD_STATIC BOOL)
  restore_cache_var(BUILD_SHARED_LIBS BOOL)
  restore_cache_var(BUILD_TESTS BOOL)
  restore_cache_var(ENABLE_PRECOMPILED BOOL)

  restore_cache_var(FMT_TEST BOOL)
  restore_cache_var(FMT_INSTALL BOOL)
  restore_cache_var(FMT_HEADER_ONLY BOOL)

else() #if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/fmt")
  message(WARNING "third_party: fmt submodule not found at ${CMAKE_CURRENT_SOURCE_DIR}/fmt")
endif()

# ---------------------------------------------------------------------------
# pylabhub::fmt is the canonical target to use fmt
# as a fallback, FMT_TARGET variable is set to the target name or empty if not available
# the following is an example of how to use this target:
#
#   if(TARGET pylabhub::fmt)
#     target_link_libraries(myexe PRIVATE pylabhub::fmt)
#   elseif(DEFINED FMT_TARGET AND FMT_TARGET)
#     target_link_libraries(myexe PRIVATE ${FMT_TARGET})
#   endif()
#
# ---------------------------------------------------------------------------