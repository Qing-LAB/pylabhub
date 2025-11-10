
cmake(MINIMUM_REQUIRED(VERSION 3.18))

# ---------------------------------------------------------------------------
# ThirdPartyPolicy.cmake
# General policies for third-party subprojects (fmt, libzmq, etc.)
# These are wrapper-level intent variables that inform third_party/CMakeLists.txt
# how to configure each subproject.
# ---------------------------------------------------------------------------
option(THIRD_PARTY_INSTALL "Install third-party libraries" ON)

# Allow upstream projects to enable precompiled headers (PCH) - default OFF for safety.
option(THIRD_PARTY_ALLOW_UPSTREAM_PCH "Allow upstream projects to enable precompiled headers" OFF)
option(THIRD_PARTY_FORCE_ALLOW_PCH "Force upstream PCH even on MSVC+Ninja (may produce errors)" OFF)
# Directory for any PCH-related files produced by wrapper logic (informational)
set(THIRD_PARTY_PCH_DIR "${CMAKE_CURRENT_BINARY_DIR}/pchs")
file(MAKE_DIRECTORY "${THIRD_PARTY_PCH_DIR}")

# Per-library variant controls. Values: "none" | "shared" | "static"
option(THIRD_PARTY_ZMQ_FORCE_VARIANT "static" CACHE STRING "Force libzmq build variant: none|shared|static")
option(THIRD_PARTY_FMT_FORCE_VARIANT "static" CACHE STRING "Force fmt build variant: none|shared|static")

# Wrapper-level intent to disable third-party tests by default (ON/OFF).
# The wrapper will set subproject-specific test knobs (e.g. ZMQ_BUILD_TESTS, FMT_TEST)
# rather than changing global BUILD_TESTS.
set(THIRD_PARTY_DISABLE_TESTS ON CACHE BOOL "Wrapper intent: disable third-party tests by default" FORCE)

# --- snapshot_cache_var / restore_cache_var (minimal, correct) ---
#
# Requires CMake 3.18+ for "DEFINED CACHE{...}" support.
#
# Usage:
#   snapshot_cache_var(MY_VAR)
#   ... (code that may set MY_VAR in cache or normal variable) ...
#   restore_cache_var(MY_VAR BOOL)   # pass the cache type expected (if any)
#
macro(snapshot_cache_var _var)
  # Save whether the variable existed as a *normal* variable in this scope
  if(DEFINED ${_var})
    set(_save_${_var}_was_defined TRUE)
    set(_save_${_var}_value "${${_var}}")
  else()
    set(_save_${_var}_was_defined FALSE)
    set(_save_${_var}_value "__UNDEFINED__")
  endif()

  # Save whether the variable existed in the CACHE and its cached value (if so)
  if(DEFINED CACHE{${_var}})
    set(_save_${_var}_was_in_cache TRUE)
    # cached value is available as a normal variable expansion
    set(_save_${_var}_cache_value "${${_var}}")
  else()
    set(_save_${_var}_was_in_cache FALSE)
    set(_save_${_var}_cache_value "__UNDEFINED__")
  endif()
endmacro()

macro(restore_cache_var _var _cache_type)
  # If var existed in cache before snapshot -> restore it as a cache entry
  if(DEFINED _save_${_var}_was_in_cache AND _save_${_var}_was_in_cache)
    if(NOT "${_save_${_var}_cache_value}" STREQUAL "__UNDEFINED__")
      set(${_var} "${_save_${_var}_cache_value}" CACHE ${_cache_type} "" FORCE)
    else()
      # no previous value recorded; set empty cache entry
      set(${_var} "" CACHE ${_cache_type} "" FORCE)
    endif()

  else()
    # Variable was not in cache before snapshot.
    # If we created a cache entry during our work, remove it now.
    if(DEFINED CACHE{${_var}})
      unset(${_var} CACHE)
    endif()

    # Restore or remove the normal variable according to saved state.
    if(DEFINED _save_${_var}_was_defined AND _save_${_var}_was_defined)
      if(NOT "${_save_${_var}_value}" STREQUAL "__UNDEFINED__")
        set(${_var} "${_save_${_var}_value}")
      else()
        unset(${_var})
      endif()
    else()
      # It did not exist previously; ensure it's not present now.
      if(DEFINED ${_var})
        unset(${_var})
      endif()
    endif()
  endif()
endmacro()


message(STATUS "========================================================="
message(STATUS "General top-level third-party policy: ")
message(STATUS "THIRD_PARTY_INSTALL=${THIRD_PARTY_INSTALL}")
message(STATUS "THIRD_PARTY_ALLOW_UPSTREAM_PCH=${THIRD_PARTY_ALLOW_UPSTREAM_PCH}")
message(STATUS "THIRD_PARTY_FORCE_ALLOW_PCH=${THIRD_PARTY_FORCE_ALLOW_PCH}")
message(STATUS "THIRD_PARTY_PCH_DIR=${THIRD_PARTY_PCH_DIR}")
message(STATUS "THIRD_PARTY_ZMQ_FORCE_VARIANT=${THIRD_PARTY_ZMQ_FORCE_VARIANT}")
message(STATUS "THIRD_PARTY_FMT_FORCE_VARIANT=${THIRD_PARTY_FMT_FORCE_VARIANT}")
message(STATUS "THIRD_PARTY_DISABLE_TESTS=${THIRD_PARTY_DISABLE_TESTS}")
message(STATUS "========================================================="

# ----------------------------------------------------------------------------
# XOPToolkit / XOPSupport build options
# ----------------------------------------------------------------------------
# Prefer the project vendor copy; override by setting USE_SYSTEM_XOPSUPPORT to a path.
# Example to force system XOPSupport:
#   cmake -D USE_SYSTEM_XOPSUPPORT="/opt/XOPSupport" ...
#
# If vendor directory exists at the provided default, prefer it.
set(XOP_VENDOR_DIR "${CMAKE_SOURCE_DIR}/third_party/XOPToolkit/XOPSupport" CACHE PATH "Vendor XOPSupport tree (preferred when available).")
# Single override path: if non-empty, FindXOPSupport will use this path and bypass vendor.
set(USE_SYSTEM_XOPSUPPORT "" CACHE PATH "Optional: path to system-installed XOPSupport (overrides vendor tree when set).")

# Option to enable/disable building the XOP plugin entirely (user-visible)
option(BUILD_XOP "Build the pylabhub XOP plugin (only supported on macOS and Windows x64)" ON)

message("")
message("===============================================================")
message("XOPToolkit / XOPSupport build options:")
message("  XOP_VENDOR_DIR=${XOP_VENDOR_DIR}")
message("  USE_SYSTEM_XOPSUPPORT=${USE_SYSTEM_XOPSUPPORT}")
message("  BUILD_XOP=${BUILD_XOP}")
message("===============================================================")
message(")

# ----------------------------------------------------------------------------
# End of ThirddParty Policy
# ----------------------------------------------------------------------------
