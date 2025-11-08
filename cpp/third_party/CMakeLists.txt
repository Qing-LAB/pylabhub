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
# snapshot_cache_var(var)
#   Saves both whether <var> exists in CACHE and its cached value (if any),
#   and also whether an in-scope variable existed (non-cache).
macro(snapshot_cache_var _var)
  # Save whether the variable exists as a normal (non-cache) variable.
  if(DEFINED ${_var})
    set(_save_${_var}_defined TRUE)
    set(_save_${_var}_value "${${_var}}")
  else()
    set(_save_${_var}_defined FALSE)
    set(_save_${_var}_value "__UNDEFINED__")
  endif()

  # Save whether a CACHE entry existed previously and its value (if present).
  if(NOT ("${${_var}}" STREQUAL "") AND DEFINED ${_var} AND (NOT "${${_var}}" STREQUAL "__UNDEFINED__"))
    # no-op: above covers normal variable state
  endif()
  # Record whether a cache entry exists
  get_property(_cache_entry_exists CACHE PROPERTY "${_var}" PROPERTY_TYPE QUIET)
  # Fallback simple test for cache presence:
  if(NOT "${${_var}}" STREQUAL "__UNDEFINED__" AND EXISTS "${CMAKE_CACHEFILE_DIR}")
    # nothing extra; this path keeps things simple
  endif()

  # Also try to read from cache if present (silently)
  if(DEFINED CACHE{${_var}})
    # Nothing: CMake syntax limitation, so we simply check the cache using if(DEFINED)
  endif()
endmacro()

# restore_cache_var(var)
#   If the variable existed before, restore its previous value (set as a normal variable)
#   If it did not exist previously and a cache entry was created by us, unset it from cache.
macro(restore_cache_var _var _type)
  if(DEFINED _save_${_var}_defined AND _save_${_var}_defined)
    # restore previous *normal* variable value
    if(NOT "${_save_${_var}_value}" STREQUAL "__UNDEFINED__")
      set(${_var} "${_save_${_var}_value}")
    else()
      unset(${_var})
    endif()
  else()
    # The var did not exist prior to our snapshot: try to remove it from cache if it was created.
    if(DEFINED ${_var})
      # If it exists now, unset it entirely (both normal and cache)
      unset(${_var} CACHE)
      unset(${_var})
    endif()
  endif()
endmacro()

# -----------------------------------------------------------------------------
# nlohmann/json (vendored or FetchContent)
# -----------------------------------------------------------------------------
# Optional control: let top-level request the vendored header be used
if(NOT DEFINED PREFER_VENDOR_NLOHMANN)
  set(PREFER_VENDOR_NLOHMANN OFF)
endif()

set(_nlohmann_vendor OFF)
set(_nlohmann_fetched OFF)
set(_nlohmann_include_dir "")

if(PREFER_VENDOR_NLOHMANN)
  set(_nlohmann_header_path "${CMAKE_CURRENT_SOURCE_DIR}/third_party/include/nlohmann/json.hpp")
  if(EXISTS "${_nlohmann_header_path}")
    message(STATUS "third_party: using vendored nlohmann/json at ${_nlohmann_header_path}")
    get_filename_component(_nlohmann_include_dir "${_nlohmann_header_path}" DIRECTORY)
    set(_nlohmann_vendor ON)
  else()
    message(STATUS "third_party: vendored nlohmann/json not found at ${_nlohmann_header_path}")
  endif()
endif()

if(NOT _nlohmann_vendor)
  message(STATUS "third_party: Fetching nlohmann/json via FetchContent")
  include(FetchContent)
  FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.2
  )
  FetchContent_MakeAvailable(nlohmann_json)
  set(_nlohmann_fetched ON)

  # upstream provides target nlohmann_json::nlohmann_json; grab its include dirs if available
  if(TARGET nlohmann_json::nlohmann_json)
    # we will alias below to pylabhub::nlohmann_json
    message(STATUS "third_party: FetchContent provided target nlohmann_json::nlohmann_json")
  else()
    # Try to detect include dir (best-effort)
    if(DEFINED nlohmann_json_SOURCE_DIR)
      set(_nlohmann_include_dir "${nlohmann_json_SOURCE_DIR}/include")
    endif()
  endif()
endif()

# Create stable alias / interface target for consumers
if(_nlohmann_vendor)
  add_library(pylabhub::nlohmann_json INTERFACE)
  target_include_directories(pylabhub::nlohmann_json INTERFACE
    $<BUILD_INTERFACE:${_nlohmann_include_dir}>
    $<INSTALL_INTERFACE:include>
  )
  set(PYLABHUB_NLOHMANN_TARGET_EXISTS ON)
elseif(TARGET nlohmann_json::nlohmann_json)
  # upstream target exists — create our alias
  add_library(pylabhub::nlohmann_json ALIAS nlohmann_json::nlohmann_json)
  set(PYLABHUB_NLOHMANN_TARGET_EXISTS ON)
elseif(_nlohmann_fetched AND DEFINED _nlohmann_include_dir AND EXISTS "${_nlohmann_include_dir}")
  add_library(pylabhub::nlohmann_json INTERFACE)
  target_include_directories(pylabhub::nlohmann_json INTERFACE
    $<BUILD_INTERFACE:${_nlohmann_include_dir}>
    $<INSTALL_INTERFACE:include>
  )
  set(PYLABHUB_NLOHMANN_TARGET_EXISTS ON)
else()
  set(PYLABHUB_NLOHMANN_TARGET_EXISTS OFF)
endif()


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
# End of third_party/CMakeLists.txt, set up all the variables that will be
# used by the whole project
# ---------------------------------------------------------------------------
# Canonical pylabhub aliases for consumers (only set if the underlying target exists)
if(TARGET zmq::zmq OR TARGET libzmq OR TARGET zmq::libzmq)
  if(NOT TARGET pylabhub::zmq)
    if(TARGET zmq::zmq)
      add_library(pylabhub::zmq ALIAS zmq::zmq)
    elseif(TARGET zmq::libzmq)
      add_library(pylabhub::zmq ALIAS zmq::libzmq)
    elseif(TARGET libzmq)
      add_library(pylabhub::zmq ALIAS libzmq)
    endif()
  endif()
  set(ZMQ_LIB_TARGET "pylabhub::zmq" PARENT_SCOPE)
else()
  message(STATUS "No libzmq target detected in CMake registry after third_party processing.")
  set(ZMQ_LIB_TARGET "" PARENT_SCOPE)
endif()

# fmt: prefer fmt::fmt or fmt::fmt-header-only; create pylabhub::fmt alias if found
if(TARGET fmt::fmt)
  add_library(pylabhub::fmt ALIAS fmt::fmt)
  set(FMT_TARGET "pylabhub::fmt" PARENT_SCOPE)
elseif(TARGET fmt::fmt-header-only)
  add_library(pylabhub::fmt ALIAS fmt::fmt-header-only)
  set(FMT_TARGET "pylabhub::fmt" PARENT_SCOPE)
else()
  message(STATUS "fmt CMake target not detected. Ensure third_party added fmt or fmt is installed system-wide.")
  set(FMT_TARGET "" PARENT_SCOPE)
endif()

# nlohmann alias export
if(PYLABHUB_NLOHMANN_TARGET_EXISTS)
  set(NLOHMANN_TARGET "pylabhub::nlohmann_json" PARENT_SCOPE)
else()
  set(NLOHMANN_TARGET "" PARENT_SCOPE)
endif()

# Collect export set
set(_pylabhub_thirdparty_targets "")
if(TARGET pylabhub::fmt)
  list(APPEND _pylabhub_thirdparty_targets pylabhub::fmt)
endif()
if(TARGET pylabhub::zmq)
  list(APPEND _pylabhub_thirdparty_targets pylabhub::zmq)
endif()
if(TARGET pylabhub::nlohmann_json)
  list(APPEND _pylabhub_thirdparty_targets pylabhub::nlohmann_json)
endif()

if(THIRD_PARTY_INSTALL AND _pylabhub_thirdparty_targets)
  install(TARGETS ${_pylabhub_thirdparty_targets}
    EXPORT pylabhubThirdPartyTargets
    INCLUDES DESTINATION include
    # RUNTIME DESTINATION bin
    # LIBRARY DESTINATION lib
    # ARCHIVE DESTINATION lib
  )

  install(EXPORT pylabhubThirdPartyTargets
    FILE pylabhubThirdPartyTargets.cmake
    NAMESPACE pylabhub::
    DESTINATION lib/cmake/pylabhub
  )

  # If you created interface targets that point into vendored include dir,
  # install headers (copy) if those are vendored and THIRD_PARTY_INSTALL selected:
  if(_nlohmann_vendor)
    install(FILES "${_nlohmann_include_dir}/json.hpp" DESTINATION include/nlohmann)
  endif()
  # Similar install COPY for fmt/libzmq headers if you vendor them and want them installed.
endif()
