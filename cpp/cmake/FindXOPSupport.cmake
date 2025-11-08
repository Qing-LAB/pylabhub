# FindXOPSupport.cmake
#
# Find or provide XOPSupport (a.k.a. XOPToolkit XOPSupport).
#
# Behavior:
# - This file is side-effect free at include() time — it only defines the function
#   find_xopsupport(<out-var>) and a couple of helper variables.
# - If the top-level sets XOP_VENDOR_DIR (CACHE PATH), vendor tree will be used
#   when present unless a user supplied USE_SYSTEM_XOPSUPPORT path is provided.
# - If the user sets USE_SYSTEM_XOPSUPPORT (CACHE PATH, non-empty), that path will
#   be used as the root for include/lib lookup and will *override* the vendor tree.
#
# On success find_xopsupport(<out-var>) will:
#  - set the <out-var> PARENT_SCOPE to TRUE
#  - create imported targets (guarded): XOP::XOPSupport and optionally XOP::IGOR
#  - set a cache variable XOP_SUPPORT_ROOT_USED (path) and XOP_SUPPORT_FOUND (BOOL)
#
# On failure it sets <out-var> to FALSE and XOP_SUPPORT_FOUND=FALSE.
#
# NOTE: Do not call find_xopsupport() at include time. Let the caller invoke it.

if(DEFINED _FIND_XOPSUPPORT_INCLUDED)
  # already included — just return so we do not redefine helpers
  return()
endif()
set(_FIND_XOPSUPPORT_INCLUDED TRUE)

# Allow override of vendor dir by top-level. Top-level should set this if vendor exists:
#   set(XOP_VENDOR_DIR "/path/to/third_party/XOPToolkit/XOPSupport" CACHE PATH "...")
if(NOT DEFINED XOP_VENDOR_DIR)
  # do not FORCE — only provide a reasonable fallback relative path as a default cache entry.
  set(XOP_VENDOR_DIR "${CMAKE_SOURCE_DIR}/third_party/XOPToolkit/XOPSupport" CACHE PATH "Vendor XOPSupport tree (fallback)" )
endif()

# The single user-switch: if non-empty it is treated as the system XOPSupport root.
# Usage (from the top-level or CLI):
#   -D USE_SYSTEM_XOPSUPPORT=/opt/XOPSupport
# If empty, vendor is preferred.
if(NOT DEFINED USE_SYSTEM_XOPSUPPORT)
  set(USE_SYSTEM_XOPSUPPORT "" CACHE PATH "Path to system-installed XOPSupport (overrides vendor). Leave empty to use vendored XOPSupport if present.")
endif()

# Helper: internal find implementation
function(_find_xopsupport_impl out_var)
  set(_found FALSE)
  set(_include_dir "")
  set(_lib_path "")
  set(_igor_lib_path "")

  # Helper to check candidate root for both header and lib
  function(_check_root_for_xop root)
    # search header(s)
    set(_header_names XOPSupport.h XOPSupport/XOPSupport.h)
    message(STATUS "  ** Looking for head file [${_header_names}] in ${root}")
    find_path(_try_include
      NAMES ${_header_names}
      HINTS "${root}" "${root}/include" "${root}/Headers" "${root}/VC" "${root}/XCode"
      NO_DEFAULT_PATH
    )

    # search library names common on Windows/Mac
    if(DEFINED PLATFORM_WIN64)
      set(_lib_names XOPSupport.lib XOPSupport64.lib)
    elseif(DEFINE PLATFORM_APPLE)
      set(_lib_names libXOPSupport.a libXOPSupport64.a)
    endif()

    message(STATUS "  ** Looking for library file [${_lib_names}] in ${root}")
    find_library(_try_lib
      NAMES ${_lib_names}
      HINTS "${root}/lib" "${root}/lib64" "${root}/VC" "${root}/XCode" "${root}"
      NO_DEFAULT_PATH
    )

    # optional IGOR library (useful on Windows)
    if(WIN32)
      find_library(_try_igor
        NAMES IGOR IGOR64
        HINTS "${root}/lib" "${root}"
        NO_DEFAULT_PATH
      )
    else()
      set(_try_igor "")
    endif()

    if(_try_include AND _try_lib)
      message(STATUS "  ** Both include heads and library found in ${root}")
      message(STATUS "     Include path: ${_try_include}")
      message(STATUS "     Library file: ${_try_lib}")
      set(_include_dir "${_try_include}" PARENT_SCOPE)
      set(_lib_path "${_try_lib}" PARENT_SCOPE)
      set(_igor_lib_path "${_try_igor}" PARENT_SCOPE)
      set(_found TRUE PARENT_SCOPE)
      return()
    else()
      message(STATUS "  ** XOPSupport not found in ${root}")
    endif()

    # leave _found unchanged if not both found
    set(_found FALSE PARENT_SCOPE)
  endfunction()

  # 1) If user provided system root, try that and treat it as authoritative.
  if(USE_SYSTEM_XOPSUPPORT)
    _check_root_for_xop("${USE_SYSTEM_XOPSUPPORT}")
    if(_found)
      set(_found TRUE)
      # set variables from the PARENT_SCOPE of helper
      set(_include_dir "${_include_dir}")
      set(_lib_path "${_lib_path}")
      set(_igor_lib_path "${_igor_lib_path}")
      set(_root_used "${USE_SYSTEM_XOPSUPPORT}")
    else()
      # message only; do not fail — allow fallback to vendor if present
      message(WARNING "FindXOPSupport: USE_SYSTEM_XOPSUPPORT='${USE_SYSTEM_XOPSUPPORT}' provided but XOPSupport header+lib not found there.")
    endif()
  endif()

  # 2) If not found yet, try vendor dir (if present)
  if(NOT _found AND DEFINED XOP_VENDOR_DIR AND EXISTS "${XOP_VENDOR_DIR}")
    _check_root_for_xop("${XOP_VENDOR_DIR}")
    if(_found)
      set(_found TRUE)
      set(_include_dir "${_include_dir}")
      set(_lib_path "${_lib_path}")
      set(_igor_lib_path "${_igor_lib_path}")
      set(_root_used "${XOP_VENDOR_DIR}")
    endif()
  endif()

  # 3) If still not found and USE_SYSTEM_XOPSUPPORT not set, try a generic system search
  if(NOT _found AND NOT USE_SYSTEM_XOPSUPPORT)
    # A best-effort find using default system paths
    find_path(_try_include_sys NAMES XOPSupport.h XOPSupport/XOPSupport.h)
    find_library(_try_lib_sys NAMES XOPSupport XOPSupport64)
    if(_try_include_sys AND _try_lib_sys)
      set(_found TRUE)
      set(XOPSUPPORT_INCLUDE_DIR "${_try_include}" PARENT_SCOPE)
      set(XOPSUPPORT_LIBRARY "${_try_lib}" PARENT_SCOPE)
    endif()
  endif()

  # Create imported targets (only when found)
  if(_found)
    # Report
    message(STATUS "FindXOPSupport: XOPSupport found. include='${_include_dir}', lib='${_lib_path}', root='${_root_used}'")
    if(_igor_lib_path)
      message(STATUS "FindXOPSupport: IGOR import lib found: ${_igor_lib_path}")
    endif()

    # create imported targets guarded to avoid duplicate target error
    if(NOT TARGET XOP::XOPSupport)
      add_library(XOP::XOPSupport INTERFACE IMPORTED)
      set_target_properties(XOP::XOPSupport PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${_include_dir}"
        INTERFACE_LINK_LIBRARIES "${_lib_path}"
      )
    else()
      message(STATUS "FindXOPSupport: XOP::XOPSupport target already exists; skipping add_library().")
    endif()

    if(_igor_lib_path AND NOT TARGET XOP::IGOR)
      add_library(XOP::IGOR INTERFACE IMPORTED)
      set_target_properties(XOP::IGOR PROPERTIES
        INTERFACE_LINK_LIBRARIES "${_igor_lib_path}"
      )
    elseif(_igor_lib_path)
      message(STATUS "FindXOPSupport: XOP::IGOR target already exists; skipping add_library().")
    endif()
  else()
    message(STATUS "FindXOPSupport: XOPSupport not found (vendor='${XOP_VENDOR_DIR}', USE_SYSTEM_XOPSUPPORT='${USE_SYSTEM_XOPSUPPORT}').")
  endif()

  # export results
  set(${out_var} ${_found} PARENT_SCOPE)
  set(XOP_SUPPORT_FOUND ${_found} CACHE BOOL "XOPSupport available" )
  if(_found)
    # record the root that was used
    set(XOP_SUPPORT_ROOT_USED "${_root_used}" CACHE PATH "Resolved XOPSupport root used by CMake" )
    # make the include/lib visible to caller (non-cache)
    set(XOP_SUPPORT_INCLUDE_DIR "${_include_dir}" PARENT_SCOPE)
    set(XOP_SUPPORT_LIB_PATH "${_lib_path}" PARENT_SCOPE)
    if(_igor_lib_path)
      set(XOP_SUPPORT_IGOR_LIB_PATH "${_igor_lib_path}" PARENT_SCOPE)
    endif()
  endif()
endfunction()

# Public function: wrapper that callers use
# Usage: find_xopsupport(<result-var>)
function(find_xopsupport out_var)
    message("  Trying to configure XOP::XOPSupport and XOP::IGOR with the following settings")
    message("    XOP_VENDOR_DIR='${XOP_VENDOR_DIR}'")
    message("    USE_SYSTEM_XOPSUPPORT='${USE_SYSTEM_XOPSUPPORT}'")
    message("    BUILD_XOP='${BUILD_XOP}'")
    if(DEFINED PLATFORM_WIN64)
      message(STATUS "  Configure for PLATFORM_WIN64 (Windows x64)")
    elseif(DEFINED PLATFORM_APPLE)
      message(STATUS "  Configure for PLATFORM_APPLE (macOS / Apple)")
    else()
      message(STATUS "  The current platform is not supported. Will not continue")
      return()
    endif()
    message("")
  if(NOT out_var)
    message(FATAL_ERROR "find_xopsupport requires a variable name to return the boolean result")
  endif()

  # call internal implemention
  _find_xopsupport_impl(_local_found)

  # return final boolean
  set(${out_var} ${_local_found} PARENT_SCOPE)
endfunction()
