# ---------------------------------------------------------------------------
# third_party/cmake/libzmq.cmake
# Setup libzmq target
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# The upstream libzmq project defines and uses:
#   - options: ZMQ_BUILD_TESTS, ZMQ_USE_PRECOMPILED_HEADER
#   - it honors BUILD_SHARED_LIBS to choose shared/static behavior.
# See libzmq/CMakeLists.txt for exact option names.
# We first cache existing relevant variables so we can restore them later
# and avoid leakage to other third_party subprojects.
# ---------------------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libzmq/CMakeLists.txt")
  message(STATUS "third_party: libzmq submodule found (scoped handling)")

  snapshot_cache_var(BUILD_SHARED)
  snapshot_cache_var(BUILD_STATIC)
  snapshot_cache_var(BUILD_SHARED_LIBS)
  snapshot_cache_var(ZMQ_BUILD_TESTS)
  snapshot_cache_var(BUILD_TESTS)
  snapshot_cache_var(ENABLE_PRECOMPILED)
  snapshot_cache_var(ZMQ_USE_PRECOMPILED_HEADER)
  snapshot_cache_var(ZMQ_USE_PCH)
  snapshot_cache_var(ENABLE_PRECOMPILED_HEADER)

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

  if(THIRD_PARTY_DISABLE_TESTS)
    set(ZMQ_BUILD_TESTS OFF CACHE BOOL "Wrapper: disable libzmq tests only" FORCE)
    set(ZMQ_TESTS OFF CACHE BOOL "Wrapper: disable libzmq tests (alt)" FORCE)
    message(STATUS "third_party wrapper: disabling libzmq tests only (ZMQ_BUILD_TESTS=OFF)")
  endif()

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

  if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
    if(NOT THIRD_PARTY_ALLOW_UPSTREAM_PCH AND NOT THIRD_PARTY_FORCE_ALLOW_PCH)
      message(STATUS "third_party: MSVC+Ninja detected; keeping upstream PCH disabled by default to avoid duplicate-rule errors.")
    elseif(THIRD_PARTY_FORCE_ALLOW_PCH)
      message(WARNING "third_party: MSVC+Ninja detected but you forced allowing upstream PCH (may see ninja: 'multiple rules generate precompiled.hpp').")
    endif()
  endif()

  # --- Begin: additional libzmq minimalization knobs (insert BEFORE add_subdirectory) ---
  #
  # Snapshot any extra vars we will set so we can restore them and avoid leakage.
  snapshot_cache_var(WITH_PERF_TOOL)
  snapshot_cache_var(WITH_DOCS)
  snapshot_cache_var(WITH_DOC)
  snapshot_cache_var(ZMQ_BUILD_FRAMEWORK)
  snapshot_cache_var(WITH_OPENPGM)
  snapshot_cache_var(WITH_NORM)
  snapshot_cache_var(WITH_VMCI)
  snapshot_cache_var(ZMQ_BUILD_EXAMPLES)
  snapshot_cache_var(ZMQ_BUILD_TOOLS)

  # Set defaults to minimize libzmq build to only the library itself.
  # Force libzmq to build *only* the library (static by default per wrapper).
  # Turn off perf tools / docs / examples / optional transports / packaging that
  # generate extra targets and VS projects.
  set(WITH_PERF_TOOL OFF CACHE BOOL "Wrapper: disable libzmq perf tools" FORCE)
  set(WITH_DOCS OFF CACHE BOOL "Wrapper: disable libzmq docs" FORCE)
  set(WITH_DOC OFF CACHE BOOL "Wrapper: disable libzmq docs (alt)" FORCE)
  set(ZMQ_BUILD_FRAMEWORK OFF CACHE BOOL "Wrapper: disable libzmq framework packaging" FORCE)

  # Disable optional transports that bring extra targets.
  set(WITH_OPENPGM OFF CACHE BOOL "Wrapper: disable OpenPGM support" FORCE)
  set(WITH_NORM OFF CACHE BOOL "Wrapper: disable NORM support" FORCE)
  set(WITH_VMCI OFF CACHE BOOL "Wrapper: disable VMCI support" FORCE)

  # Disable example/tool targets that upstream sometimes creates.
  set(ZMQ_BUILD_EXAMPLES OFF CACHE BOOL "Wrapper: disable libzmq example binaries" FORCE)
  set(ZMQ_BUILD_TOOLS OFF CACHE BOOL "Wrapper: disable libzmq helper tools" FORCE)
  # --- End: additional libzmq minimalization knobs ---

  # Add libzmq subproject. It will read the variables we set above.
  # Use EXCLUDE_FROM_ALL to avoid pulling libzmq into default builds unless explicitly used.
  # Targets created: libzmq / libzmq-static / libzmq-shared
  add_subdirectory(libzmq EXCLUDE_FROM_ALL)

  message("")
  message("=========================================================")
  message(STATUS "Build libzmq (ZeroMQ) subproject with the following settings.")
  message(STATUS "  THIRD_PARTY_ZMQ_FORCE_VARIANT = ${THIRD_PARTY_ZMQ_FORCE_VARIANT}")
  message(STATUS "  BUILD_STATIC      = ${BUILD_STATIC}")
  message(STATUS "  BUILD_SHARED      = ${BUILD_SHARED}")
  message(STATUS "  ZMQ_STATIC       = ${ZMQ_STATIC}")
  message(STATUS "  ZMQ_BUILD_SHARED = ${ZMQ_BUILD_SHARED}")
  message(STATUS "  BUILD_SHARED_LIBS = ${BUILD_SHARED_LIBS}")
  message(STATUS "  ZMQ_BUILD_TESTS   = ${ZMQ_BUILD_TESTS}")
  message(STATUS "  ZMQ_TESTS         = ${ZMQ_TESTS}")
  message(STATUS "  ENABLE_PRECOMPILED = ${ENABLE_PRECOMPILED}")
  message(STATUS "  ZMQ_USE_PRECOMPILED_HEADER = ${ZMQ_USE_PRECOMPILED_HEADER}")
  message(STATUS "  ZMQ_USE_PCH = ${ZMQ_USE_PCH}")
  message(STATUS "  ENABLE_PRECOMPILED_HEADER = ${ENABLE_PRECOMPILED_HEADER}")
  message(STATUS "  WITH_PERF_TOOL = ${WITH_PERF_TOOL}")
  message(STATUS "  WITH_DOCS = ${WITH_DOCS}")
  message(STATUS "  WITH_DOC = ${WITH_DOC}")
  message(STATUS "  ZMQ_BUILD_FRAMEWORK = ${ZMQ_BUILD_FRAMEWORK}")
  message(STATUS "  WITH_OPENPGM = ${WITH_OPENPGM}")
  message(STATUS "  WITH_NORM = ${WITH_NORM}")
  message(STATUS "  WITH_VMCI = ${WITH_VMCI}")
  message(STATUS "  ZMQ_BUILD_EXAMPLES = ${ZMQ_BUILD_EXAMPLES}")
  message(STATUS "  ZMQ_BUILD_TOOLS = ${ZMQ_BUILD_TOOLS}")
  message("=========================================================")
  message("")
  
  # ----- additional robustness for ZeroMQ target names -----
  # (place after the existing libzmq-static / libzmq-shared aliasing)
  # Accept a few more candidate names (some packaging / forks use these).
  # Robust libzmq / zmq::zmq aliasing
  if(NOT TARGET libzmq) 
    if(TARGET libzmq-static) 
      add_library(libzmq ALIAS libzmq-static) 
      message(STATUS "third_party: libzmq alias created -> libzmq-static") 
      # Provide a stable namespaced alias so downstream code can use zmq::zmq 
      if(NOT TARGET zmq::zmq) 
        add_library(zmq::zmq ALIAS libzmq-static) 
        message(STATUS "third_party: created alias zmq::zmq -> libzmq-static") 
      endif() 
    elseif(TARGET libzmq-shared) 
      add_library(libzmq ALIAS libzmq-shared) 
      message(STATUS "third_party: libzmq alias created -> libzmq-shared") 
      if(NOT TARGET zmq::zmq) 
        add_library(zmq::zmq ALIAS libzmq-shared) 
        message(STATUS "third_party: created alias zmq::zmq -> libzmq-shared") 
      endif() 
    elseif(TARGET libzmq_shared) 
      add_library(libzmq ALIAS libzmq_shared) 
      message(STATUS "third_party: libzmq alias created -> libzmq_shared") 
      if(NOT TARGET zmq::zmq) 
        add_library(zmq::zmq ALIAS libzmq_shared) 
        message(STATUS "third_party: created alias zmq::zmq -> libzmq_shared") 
      endif() 
    endif() 
  else() 
    if(NOT TARGET zmq::zmq) 
      add_library(zmq::zmq ALIAS libzmq) 
      message(STATUS "third_party: created alias zmq::zmq -> libzmq") 
    endif() 
  endif()

  # Provide stable pylabhub::zmq alias for consumers
  if(NOT TARGET pylabhub::zmq)
    if(TARGET zmq::zmq)
      add_library(pylabhub::zmq ALIAS zmq::zmq)
      message(STATUS "third_party: created alias pylabhub::zmq -> zmq::zmq")
    elseif(TARGET libzmq)
      add_library(pylabhub::zmq ALIAS libzmq)
      message(STATUS "third_party: created alias pylabhub::zmq -> libzmq")
    endif()
  endif()

  # Export variable for legacy callers (only if target exists)
  if(TARGET pylabhub::zmq)
    set(ZMQ_LIB_TARGET "pylabhub::zmq" PARENT_SCOPE)
    message(STATUS "third_party: exported ZMQ_LIB_TARGET = pylabhub::zmq")
  else()
    set(ZMQ_LIB_TARGET "" PARENT_SCOPE)
    message(STATUS "third_party: pylabhub::zmq not created; ZMQ_LIB_TARGET empty")
  endif()

  # ---------------------------------------------------------
  # Setup installation of libzmq target and headers
  # ---------------------------------------------------------
  if(THIRD_PARTY_INSTALL)
    # Determine the real linkable target that exists (prefer names upstream creates)
    set(_real_zmq_target "")
    if(TARGET zmq::zmq)
      set(_real_zmq_target zmq::zmq)
    elseif(TARGET libzmq)
      set(_real_zmq_target libzmq)
    elseif(TARGET libzmq-static)
      set(_real_zmq_target libzmq-static)
    elseif(TARGET libzmq-shared)
      set(_real_zmq_target libzmq-shared)
    endif()

    message("")
    message("=========================================================")
    message(STATUS "INSTALL setup for libzmq (ZeroMQ):")
    if(_real_zmq_target)
      # If upstream does not already handle install, create install rules for the existing target.
      # Note: if upstream configured install, this may duplicate; guard as needed using upstream vars.
      message(STATUS "third_party: installing libzmq target ${_real_zmq_target}")

      install(TARGETS ${_real_zmq_target}
        EXPORT pylabhubThirdPartyTargets
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
        INCLUDES DESTINATION include
      )

      # Install headers: prefer ZMQ_INCLUDE_DIR if set, fallback to vendored path
      if(DEFINED ZMQ_INCLUDE_DIR AND EXISTS "${ZMQ_INCLUDE_DIR}")
        message(STATUS "third_party: installing libzmq headers from ${ZMQ_INCLUDE_DIR}")
        install(DIRECTORY "${ZMQ_INCLUDE_DIR}/" DESTINATION include COMPONENT devel FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")
      else()
        set(_zmq_vendored_include "${CMAKE_CURRENT_SOURCE_DIR}/libzmq/include")
        if(EXISTS "${_zmq_vendored_include}")
          message(STATUS "third_party: installing libzmq headers from ${_zmq_vendored_include}")
          install(DIRECTORY "${_zmq_vendored_include}/" DESTINATION include COMPONENT devel FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")
        endif()
      endif()
    else()
      message(WARNING "third_party: no libzmq target found to install; skipping libzmq install.")
    endif()
  endif()
  message("=========================================================")
  message("")

  # ---------------------------------------------------------
  # Restore cached variables to avoid leakage to other third_party subprojects.
  # ---------------------------------------------------------
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
  
  message(STATUS "third_party: libzmq (zmq::zmq) added (scoped).")
else()
  message(WARNING "third_party: libzmq submodule not found at ${CMAKE_CURRENT_SOURCE_DIR}/libzmq")
endif()


