# ---------------------------------------------------------------------------
# third_party/cmake/libzmq.cmake
# Wrapper for libzmq (ZeroMQ) third_party subproject
#
# Responsibilities:
#  - Configure upstream libzmq subproject according to wrapper-level policy
#    (THIRD_PARTY_ZMQ_FORCE_VARIANT, THIRD_PARTY_DISABLE_TESTS, PCH policy).
#  - Provide a stable public handle pylabhub::zmq for downstream consumers.
#  - Deterministically install headers/libs into our project's install tree
#    when THIRD_PARTY_INSTALL (global) and wrapper-level intent (FMT-like)
#    are set. Do not rely on upstream subproject to run its own install rules.
#  - Avoid calling install(TARGETS ...) on alias or interface targets by using
#    the shared helper _is_target_installable().
# ---------------------------------------------------------------------------

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libzmq/CMakeLists.txt")
  message(STATUS "third_party: libzmq submodule found (scoped handling)")

  # ---------------------------
  # Snapshot cache vars we will touch so we can restore them later and avoid leakage.
  # (snapshot_cache_var/restore_cache_var should be provided by your cmake helpers.)
  # ---------------------------
  snapshot_cache_var(BUILD_SHARED)
  snapshot_cache_var(BUILD_STATIC)
  snapshot_cache_var(BUILD_SHARED_LIBS)
  snapshot_cache_var(ZMQ_BUILD_TESTS)
  snapshot_cache_var(BUILD_TESTS)
  snapshot_cache_var(ENABLE_PRECOMPILED)

  snapshot_cache_var(ZMQ_USE_PRECOMPILED_HEADER)
  snapshot_cache_var(ZMQ_USE_PCH)
  snapshot_cache_var(ENABLE_PRECOMPILED_HEADER)

  # Additional knobs used to minimize libzmq build footprint (examples/docs/tools)
  snapshot_cache_var(WITH_PERF_TOOL)
  snapshot_cache_var(WITH_DOCS)
  snapshot_cache_var(WITH_DOC)
  snapshot_cache_var(ZMQ_BUILD_FRAMEWORK)
  snapshot_cache_var(WITH_OPENPGM)
  snapshot_cache_var(WITH_NORM)
  snapshot_cache_var(WITH_VMCI)
  snapshot_cache_var(ZMQ_BUILD_EXAMPLES)
  snapshot_cache_var(ZMQ_BUILD_TOOLS)

  # ---------------------------
  # Variant selection: static/shared
  # ---------------------------
  if(NOT DEFINED THIRD_PARTY_ZMQ_FORCE_VARIANT)
    set(THIRD_PARTY_ZMQ_FORCE_VARIANT "none")
  endif()

  if(THIRD_PARTY_ZMQ_FORCE_VARIANT STREQUAL "static")
    set(BUILD_SHARED OFF CACHE BOOL "Wrapper: prefer static libs for libzmq" FORCE)
    set(BUILD_STATIC ON CACHE BOOL "Wrapper: prefer static libs for libzmq" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Wrapper: prefer static libs (generic hint)" FORCE)
    set(ZMQ_BUILD_SHARED OFF CACHE BOOL "Wrapper: prefer static libs for libzmq (alt)" FORCE)
    set(ZMQ_STATIC ON CACHE BOOL "Wrapper: prefer static libs for libzmq (alt)" FORCE)
    message(STATUS "third_party wrapper: forcing libzmq variant = static (BUILD_SHARED=OFF, BUILD_STATIC=ON)")
  elseif(THIRD_PARTY_ZMQ_FORCE_VARIANT STREQUAL "shared")
    set(BUILD_SHARED ON CACHE BOOL "Wrapper: prefer shared libs for libzmq" FORCE)
    set(BUILD_STATIC OFF CACHE BOOL "Wrapper: prefer shared libs for libzmq" FORCE)
    set(BUILD_SHARED_LIBS ON CACHE BOOL "Wrapper: prefer shared libs (generic hint)" FORCE)
    set(ZMQ_BUILD_SHARED ON CACHE BOOL "Wrapper: prefer shared libs for libzmq (alt)" FORCE)
    set(ZMQ_STATIC OFF CACHE BOOL "Wrapper: prefer shared libs for libzmq (alt)" FORCE)
    message(STATUS "third_party wrapper: forcing libzmq variant = shared (BUILD_SHARED=ON, BUILD_STATIC=OFF)")
  else()
    message(STATUS "third_party wrapper: not forcing libzmq variant (none)")
  endif()

  # ---------------------------
  # Disable upstream tests if requested by wrapper policy (do not set global BUILD_TESTS)
  # ---------------------------
  if(THIRD_PARTY_DISABLE_TESTS)
    set(ZMQ_BUILD_TESTS OFF CACHE BOOL "Wrapper: disable libzmq tests only" FORCE)
    set(ZMQ_TESTS OFF CACHE BOOL "Wrapper: disable libzmq tests (alt)" FORCE)
    message(STATUS "third_party wrapper: disabling libzmq tests only (ZMQ_BUILD_TESTS=OFF)")
  endif()

  # ---------------------------
  # Precompiled header policy (scoped)
  # ---------------------------
  if(NOT DEFINED THIRD_PARTY_ALLOW_UPSTREAM_PCH)
    set(THIRD_PARTY_ALLOW_UPSTREAM_PCH OFF)
  endif()

  if(NOT THIRD_PARTY_ALLOW_UPSTREAM_PCH)
    set(ENABLE_PRECOMPILED OFF CACHE BOOL "Wrapper: disable libzmq precompiled headers" FORCE)
    set(ZMQ_USE_PRECOMPILED_HEADER OFF CACHE BOOL "Wrapper: disable libzmq precompiled headers (alt)" FORCE)
    set(ZMQ_USE_PCH OFF CACHE BOOL "Wrapper: disable libzmq precompiled headers (alt2)" FORCE)
    set(ENABLE_PRECOMPILED_HEADER OFF CACHE BOOL "Wrapper: disable libzmq precompiled headers (alt3)" FORCE)
    message(STATUS "third_party wrapper: attempted to disable libzmq PCH via ENABLE_PRECOMPILED / ZMQ_USE_PRECOMPILED_HEADER / ZMQ_USE_PCH / ENABLE_PRECOMPILED_HEADER")
  else()
    message(STATUS "third_party wrapper: allowing libzmq upstream PCH if upstream requests it")
  endif()

  # Extra note for MSVC + Ninja where PCH can cause duplicate-rule errors
  if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
    if(NOT THIRD_PARTY_ALLOW_UPSTREAM_PCH AND NOT THIRD_PARTY_FORCE_ALLOW_PCH)
      message(STATUS "third_party: MSVC+Ninja detected; keeping upstream PCH disabled by default to avoid duplicate-rule errors.")
    elseif(THIRD_PARTY_FORCE_ALLOW_PCH)
      message(WARNING "third_party: MSVC+Ninja detected but you forced allowing upstream PCH (may see ninja: 'multiple rules generate precompiled.hpp').")
    endif()
  endif()

  # ---------------------------
  # Minimize upstream libzmq build (turn off docs, examples, optional transports, etc.)
  # ---------------------------
  set(WITH_PERF_TOOL OFF CACHE BOOL "Wrapper: disable libzmq perf tools" FORCE)
  set(WITH_DOCS OFF CACHE BOOL "Wrapper: disable libzmq docs" FORCE)
  set(WITH_DOC OFF CACHE BOOL "Wrapper: disable libzmq docs (alt)" FORCE)
  set(ZMQ_BUILD_FRAMEWORK OFF CACHE BOOL "Wrapper: disable libzmq framework packaging" FORCE)

  set(WITH_OPENPGM OFF CACHE BOOL "Wrapper: disable OpenPGM support" FORCE)
  set(WITH_NORM OFF CACHE BOOL "Wrapper: disable NORM support" FORCE)
  set(WITH_VMCI OFF CACHE BOOL "Wrapper: disable VMCI support" FORCE)

  set(ZMQ_BUILD_EXAMPLES OFF CACHE BOOL "Wrapper: disable libzmq example binaries" FORCE)
  set(ZMQ_BUILD_TOOLS OFF CACHE BOOL "Wrapper: disable libzmq helper tools" FORCE)

  # ---------------------------
  # Add subproject (scoped)
  # ---------------------------
  add_subdirectory(libzmq EXCLUDE_FROM_ALL)

  # Print summary of effective settings for visibility
  message("")
  message("=========================================================")
  message(STATUS "Build libzmq (ZeroMQ) subproject with the following settings.")
  message(STATUS "  THIRD_PARTY_ZMQ_FORCE_VARIANT = ${THIRD_PARTY_ZMQ_FORCE_VARIANT}")
  message(STATUS "  BUILD_STATIC      = ${BUILD_STATIC}")
  message(STATUS "  BUILD_SHARED      = ${BUILD_SHARED}")
  message(STATUS "  ZMQ_STATIC        = ${ZMQ_STATIC}")
  message(STATUS "  ZMQ_BUILD_SHARED  = ${ZMQ_BUILD_SHARED}")
  message(STATUS "  BUILD_SHARED_LIBS = ${BUILD_SHARED_LIBS}")
  message(STATUS "  ZMQ_BUILD_TESTS   = ${ZMQ_BUILD_TESTS}")
  message(STATUS "  ENABLE_PRECOMPILED = ${ENABLE_PRECOMPILED}")
  message(STATUS "  ZMQ_USE_PRECOMPILED_HEADER = ${ZMQ_USE_PRECOMPILED_HEADER}")
  message(STATUS "  WITH_OPENPGM = ${WITH_OPENPGM}")
  message(STATUS "  ZMQ_BUILD_EXAMPLES = ${ZMQ_BUILD_EXAMPLES}")
  message("=========================================================")
  message("")

  # ---------------------------
  # Determine canonical concrete target to use for linking and installs.
  #
  # Strategy:
  #  - Prefer concrete targets produced by the subproject (libzmq-static/libzmq-shared).
  #  - Accept commonly exported target names (libzmq, zmq, zmq::zmq) and resolve aliases.
  #  - Use the centralized helper _is_target_installable() to test whether
  #    a candidate is safe to pass to install(TARGETS ...).
  #  - If no concrete target is available, still create the pylabhub wrapper and
  #    later fall back to manual file-based installs for library files.
  # ---------------------------
  set(_zmq_canonical_target "")
  set(_zmq_candidates "libzmq-static;libzmq-shared;libzmq;zmq;zmq::zmq")

  foreach(_cand IN LISTS _zmq_candidates)
    if(NOT _cand)
      continue()
    endif()

    # If candidate exists and is installable, pick it.
    _is_target_installable("${_cand}" _cand_installable)
    if(_cand_installable)
      set(_zmq_canonical_target "${_cand}")
      break()
    endif()

    # If candidate exists and is an alias, try to resolve ALIASED_TARGET to a real target.
    if(TARGET ${_cand})
      get_target_property(_aliased ${_cand} ALIASED_TARGET)
      if(DEFINED _aliased AND _aliased AND NOT _aliased STREQUAL "NOTFOUND")
        _is_target_installable("${_aliased}" _aliased_installable)
        if(_aliased_installable)
          set(_zmq_canonical_target "${_aliased}")
          break()
        endif()
      endif()
    endif()
  endforeach()

  # ---------------------------
  # Create stable pylabhub wrapper target and public alias.
  # - pylabhub_zmq is an INTERFACE wrapper (no '::').
  # - pylabhub::zmq is an ALIAS to pylabhub_zmq (stable namespaced handle).
  # - The wrapper will target_link_libraries(...) to the canonical upstream/concrete target
  #   if one was found; otherwise it remains an INTERFACE with include dirs set from vendored paths.
  # ---------------------------
  if(NOT TARGET pylabhub_zmq)
    add_library(pylabhub_zmq INTERFACE)
  endif()

  if(_zmq_canonical_target)
    message(STATUS "third_party: linking pylabhub_zmq -> ${_zmq_canonical_target}")
    target_link_libraries(pylabhub_zmq INTERFACE ${_zmq_canonical_target})
  else()
    # No concrete target found; ensure headers are at least available via include dirs.
    # Prefer ZMQ_INCLUDE_DIR (if set), otherwise use vendored layout.
    if(DEFINED ZMQ_INCLUDE_DIR AND EXISTS "${ZMQ_INCLUDE_DIR}")
      target_include_directories(pylabhub_zmq INTERFACE
        $<BUILD_INTERFACE:${ZMQ_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:include>
      )
      message(STATUS "third_party: pylabhub_zmq interface includes set -> ZMQ_INCLUDE_DIR (${ZMQ_INCLUDE_DIR})")
    else()
      set(_zmq_vendored_include "${CMAKE_CURRENT_SOURCE_DIR}/libzmq/include")
      if(EXISTS "${_zmq_vendored_include}")
        target_include_directories(pylabhub_zmq INTERFACE
          $<BUILD_INTERFACE:${_zmq_vendored_include}>
          $<INSTALL_INTERFACE:include>
        )
        message(STATUS "third_party: pylabhub_zmq interface includes set -> vendored ${_zmq_vendored_include}")
      else()
        message(STATUS "third_party: pylabhub_zmq: no include dirs discovered for header-only fallback")
      endif()
    endif()
  endif()

  if(NOT TARGET pylabhub::zmq)
    add_library(pylabhub::zmq ALIAS pylabhub_zmq)
    message(STATUS "third_party: created alias pylabhub::zmq -> pylabhub_zmq")
  endif()

  # Export variable for legacy/explicit consumers (only if target exists)
  if(TARGET pylabhub::zmq)
    set(ZMQ_LIB_TARGET "pylabhub::zmq" PARENT_SCOPE)
    message(STATUS "third_party: exported ZMQ_LIB_TARGET = pylabhub::zmq")
  else()
    set(ZMQ_LIB_TARGET "" PARENT_SCOPE)
    message(STATUS "third_party: pylabhub::zmq not created; ZMQ_LIB_TARGET empty")
  endif()

  # ---------------------------
  # INSTALL handling (wrapper-managed)
  #
  # Behavior:
  #  - When THIRD_PARTY_INSTALL is ON, the wrapper ensures headers/libs are placed
  #    into the project's install tree (install prefix) deterministically.
  #  - If _zmq_canonical_target is a concrete target, we call install(TARGETS ...) on it.
  #  - Otherwise we install headers (from ZMQ_INCLUDE_DIR or vendored include) and
  #    search the build tree for produced library files and install them via install(FILES ...).
  # ---------------------------
  if(THIRD_PARTY_INSTALL)
    message(STATUS "third_party: preparing install for libzmq (wrapper-managed)")

    set(_zmq_targets_to_install "")
    if(_zmq_canonical_target)
      # If canonical target was determined above, attempt to install it (it should be concrete).
      # We still call the helper to be safe in case something changed.
      _is_target_installable("${_zmq_canonical_target}" _can_install)
      if(_can_install)
        list(APPEND _zmq_targets_to_install "${_zmq_canonical_target}")
      else()
        # In rare cases the canonical target may be an alias; attempt to resolve aliased real target.
        if(TARGET ${_zmq_canonical_target})
          get_target_property(_aliased_real ${_zmq_canonical_target} ALIASED_TARGET)
          if(DEFINED _aliased_real AND _aliased_real AND NOT _aliased_real STREQUAL "NOTFOUND")
            _is_target_installable("${_aliased_real}" _aliased_real_ok)
            if(_aliased_real_ok)
              list(APPEND _zmq_targets_to_install "${_aliased_real}")
            endif()
          endif()
        endif()
      endif()
    endif()

    # Deduplicate
    list(REMOVE_DUPLICATES _zmq_targets_to_install)

    if(_zmq_targets_to_install)
      # install concrete targets; provide PUBLIC_HEADER DESTINATION to avoid dev warnings
      install(TARGETS ${_zmq_targets_to_install}
        EXPORT pylabhubThirdPartyTargets
        INCLUDES DESTINATION include
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
        PUBLIC_HEADER DESTINATION include/zmq
      )
      message(STATUS "third_party: installed libzmq targets via install(TARGETS ...): ${_zmq_targets_to_install}")
    else()
      # No concrete targets available to install via install(TARGETS ...).
      # Fallback: ensure headers are installed and try to find lib files in build tree.
      set(_zmq_headers_installed FALSE)

      if(DEFINED ZMQ_INCLUDE_DIR AND EXISTS "${ZMQ_INCLUDE_DIR}")
        message(STATUS "third_party: installing libzmq headers from ZMQ_INCLUDE_DIR (${ZMQ_INCLUDE_DIR})")
        install(DIRECTORY "${ZMQ_INCLUDE_DIR}/" DESTINATION include COMPONENT devel FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")
        set(_zmq_headers_installed TRUE)
      else()
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libzmq/include")
          set(_zmq_vendored_include "${CMAKE_CURRENT_SOURCE_DIR}/libzmq/include")
          message(STATUS "third_party: installing vendored libzmq headers from ${_zmq_vendored_include}")
          install(DIRECTORY "${_zmq_vendored_include}/" DESTINATION include COMPONENT devel FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")
          set(_zmq_headers_installed TRUE)
        endif()
      endif()

      if(NOT _zmq_headers_installed)
        message(STATUS "third_party: warning: no libzmq headers found to install (no ZMQ_INCLUDE_DIR or vendored include)")
      endif()

      # Search the build tree for produced lib artifacts (libzmq*.a / libzmq*.so / zmq*.lib / zmq*.dll)
      set(_zmq_lib_found_files "")
      file(GLOB_RECURSE _zmq_search_a RELATIVE "${CMAKE_CURRENT_BINARY_DIR}" "libzmq*.a" "*/libzmq*.a")
      foreach(_f IN LISTS _zmq_search_a)
        list(APPEND _zmq_lib_found_files "${CMAKE_CURRENT_BINARY_DIR}/${_f}")
      endforeach()

      file(GLOB_RECURSE _zmq_search_so RELATIVE "${CMAKE_CURRENT_BINARY_DIR}" "libzmq*.so" "*/libzmq*.so" "libzmq*.so.*" "*/libzmq*.so.*")
      foreach(_f IN LISTS _zmq_search_so)
        list(APPEND _zmq_lib_found_files "${CMAKE_CURRENT_BINARY_DIR}/${_f}")
      endforeach()

      file(GLOB_RECURSE _zmq_search_lib RELATIVE "${CMAKE_CURRENT_BINARY_DIR}" "zmq*.lib" "*/zmq*.lib")
      foreach(_f IN LISTS _zmq_search_lib)
        list(APPEND _zmq_lib_found_files "${CMAKE_CURRENT_BINARY_DIR}/${_f}")
      endforeach()

      file(GLOB_RECURSE _zmq_search_dll RELATIVE "${CMAKE_CURRENT_BINARY_DIR}" "zmq*.dll" "*/zmq*.dll")
      foreach(_f IN LISTS _zmq_search_dll)
        list(APPEND _zmq_lib_found_files "${CMAKE_CURRENT_BINARY_DIR}/${_f}")
      endforeach()

      list(REMOVE_DUPLICATES _zmq_lib_found_files)

      # Keep only existing files (defensive)
      set(_zmq_lib_files_existing "")
      foreach(_f IN LISTS _zmq_lib_found_files)
        if(EXISTS "${_f}")
          list(APPEND _zmq_lib_files_existing "${_f}")
        endif()
      endforeach()
      set(_zmq_lib_found_files ${_zmq_lib_files_existing})

      if(_zmq_lib_found_files)
        message(STATUS "third_party: found libzmq artifacts to install: ${_zmq_lib_found_files}")
        install(FILES ${_zmq_lib_found_files} DESTINATION lib COMPONENT devel)
      else()
        message(STATUS "third_party: no libzmq library artifacts found in build tree; nothing to install for libs")
      endif()
    endif() # if(_zmq_targets_to_install)
  else()
    message(STATUS "third_party: THIRD_PARTY_INSTALL is OFF; wrapper will not install libzmq into project.")
  endif() # if(THIRD_PARTY_INSTALL)

  # ---------------------------
  # Restore cached variables to avoid leakage to other third_party subprojects.
  # ---------------------------
  restore_cache_var(BUILD_SHARED BOOL)
  restore_cache_var(BUILD_STATIC BOOL)
  restore_cache_var(BUILD_SHARED_LIBS BOOL)
  restore_cache_var(ZMQ_BUILD_TESTS BOOL)
  restore_cache_var(BUILD_TESTS BOOL)
  restore_cache_var(ENABLE_PRECOMPILED BOOL)

  restore_cache_var(ZMQ_USE_PRECOMPILED_HEADER BOOL)
  restore_cache_var(ZMQ_USE_PCH BOOL)
  restore_cache_var(ENABLE_PRECOMPILED_HEADER BOOL)

  restore_cache_var(WITH_PERF_TOOL BOOL)
  restore_cache_var(WITH_DOCS BOOL)
  restore_cache_var(WITH_DOC BOOL)
  restore_cache_var(ZMQ_BUILD_FRAMEWORK BOOL)
  restore_cache_var(WITH_OPENPGM BOOL)
  restore_cache_var(WITH_NORM BOOL)
  restore_cache_var(WITH_VMCI BOOL)
  restore_cache_var(ZMQ_BUILD_EXAMPLES BOOL)
  restore_cache_var(ZMQ_BUILD_TOOLS BOOL)

  message(STATUS "third_party: libzmq wrapper configured (scoped).")
else()
  message(WARNING "third_party: libzmq submodule not found at ${CMAKE_CURRENT_SOURCE_DIR}/libzmq")
endif()
