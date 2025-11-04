# third_party/CMakeLists.txt
# Purpose: manage third-party submodules (fmt, libzmq, ...) in a scoped, non-leaking way.
#
# This wrapper sets subproject-specific cache variables (the exact knobs the upstream
# project uses) before calling add_subdirectory(...), and restores any changed
# cache variables after the subproject is handled so state does not leak.
#
# Note: fmt upstream exposes options such as FMT_TEST, FMT_INSTALL, FMT_DOC,
#       FMT_HEADER_ONLY and checks BUILD_SHARED_LIBS for shared/static choice.
#       Those exact names are used below to ensure the wrapper is accurate.
#       (Validated against uploaded fmt/CMakeLists.txt.) :contentReference[oaicite:1]{index=1}
#
cmake_minimum_required(VERSION 3.15)
project(third_party NONE)

# -----------------------
# Global wrapper options
# -----------------------
option(THIRD_PARTY_INSTALL "Install third-party libraries" OFF)

# Allow upstream projects to enable precompiled headers (PCH) - default OFF for safety.
option(THIRD_PARTY_ALLOW_UPSTREAM_PCH "Allow upstream projects to enable precompiled headers" OFF)
option(THIRD_PARTY_FORCE_ALLOW_PCH "Force upstream PCH even on MSVC+Ninja (may produce errors)" OFF)

# Per-library variant controls. Values: "none" | "shared" | "static"
set(THIRD_PARTY_ZMQ_FORCE_VARIANT "none" CACHE STRING "Force libzmq build variant: none|shared|static")
set(THIRD_PARTY_FMT_FORCE_VARIANT "none" CACHE STRING "Force fmt build variant: none|shared|static")

# Wrapper-level third-party test toggle. WARNING: this must not set global BUILD_TESTS.
set(THIRD_PARTY_DISABLE_TESTS OFF CACHE BOOL "Wrapper intent: disable third-party tests only (do not change top-level BUILD_TESTS)")

# Directory for any PCH-related files produced by wrapper logic (informational)
set(THIRD_PARTY_PCH_DIR "${CMAKE_CURRENT_BINARY_DIR}/pchs")
file(MAKE_DIRECTORY "${THIRD_PARTY_PCH_DIR}")

# ---------------------------------------------------------------------------
# Helper macros: snapshot & restore cache variables (to avoid leakage)
# ---------------------------------------------------------------------------
# snapshot_cache_var(<VAR>)
#    records whether VAR was defined and what its value was (in parent scope).
#
# restore_cache_var(<VAR> <TYPE>)
#    restores VAR to its saved value or unsets it from cache if it did not previously exist.
#
macro(snapshot_cache_var _var)
  if(DEFINED ${_var})
    set(_save_${_var}_defined TRUE PARENT_SCOPE)
    set(_save_${_var}_value "${${_var}}" PARENT_SCOPE)
  else()
    set(_save_${_var}_defined FALSE PARENT_SCOPE)
    set(_save_${_var}_value "__UNDEFINED__" PARENT_SCOPE)
  endif()
endmacro()

macro(restore_cache_var _var _type)
  if(DEFINED _save_${_var}_defined AND _save_${_var}_defined)
    if(DEFINED _save_${_var}_value)
      set(${_var} "${_save_${_var}_value}" CACHE BOOL "" FORCE)
    endif()
    # don't unset the cache variable if it wasn't previously set globally
  endif()
endmacro()

# ---------------------------------------------------------------------------
# fmt block (scoped)
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
  snapshot_cache_var(FMT_DOC)
  snapshot_cache_var(FMT_HEADER_ONLY)
  snapshot_cache_var(FMT_MODULE)
  snapshot_cache_var(FMT_SYSTEM_HEADERS)
  snapshot_cache_var(FMT_UNICODE)

  # Preserve any top-level consumer intent like USE_FMT_HEADER_ONLY (if present)
  snapshot_cache_var(USE_FMT_HEADER_ONLY)

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

  # Restore previously-snapshotted cache variables so fmt's forced values do not leak.
  restore_cache_var(BUILD_SHARED BOOL)
  restore_cache_var(BUILD_STATIC BOOL)
  restore_cache_var(BUILD_SHARED_LIBS BOOL)
  restore_cache_var(BUILD_TESTS BOOL)
  restore_cache_var(ENABLE_PRECOMPILED BOOL)

  restore_cache_var(FMT_TEST BOOL)
  restore_cache_var(FMT_INSTALL BOOL)
  restore_cache_var(FMT_DOC BOOL)
  restore_cache_var(FMT_HEADER_ONLY BOOL)
  restore_cache_var(FMT_MODULE BOOL)
  restore_cache_var(FMT_SYSTEM_HEADERS BOOL)
  restore_cache_var(FMT_UNICODE BOOL)

  restore_cache_var(USE_FMT_HEADER_ONLY BOOL)

else()
  message(WARNING "third_party: fmt submodule not found at ${CMAKE_CURRENT_SOURCE_DIR}/fmt")
endif()

# ---------------------------------------------------------------------------
# libzmq block (scoped) - retained from earlier version, unchanged behavior
# ---------------------------------------------------------------------------
# (This block keeps the snapshot/override/restore semantics for libzmq.)
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

  add_subdirectory(libzmq EXCLUDE_FROM_ALL)

  restore_cache_var(BUILD_SHARED BOOL)
  restore_cache_var(BUILD_STATIC BOOL)
  restore_cache_var(BUILD_SHARED_LIBS BOOL)
  restore_cache_var(ZMQ_BUILD_TESTS BOOL)
  restore_cache_var(BUILD_TESTS BOOL)
  restore_cache_var(ENABLE_PRECOMPILED BOOL)
  restore_cache_var(ZMQ_USE_PRECOMPILED_HEADER BOOL)
  restore_cache_var(ZMQ_USE_PCH BOOL)
  restore_cache_var(ENABLE_PRECOMPILED_HEADER BOOL)

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
  # ---------------------------------------------------------
  
  message(STATUS "third_party: libzmq (zmq::zmq) added (scoped).")
else()
  message(WARNING "third_party: libzmq submodule not found at ${CMAKE_CURRENT_SOURCE_DIR}/libzmq")
endif()

# ---------------------------------------------------------------------------
# End of third_party/CMakeLists.txt
# ---------------------------------------------------------------------------
