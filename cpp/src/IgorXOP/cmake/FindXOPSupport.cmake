# FindXOPSupport.cmake
#
# Find or provide XOPSupport (a.k.a. XOPToolkit XOPSupport).
#
# Behavior:
# - This file is side-effect free at include() time — it only defines the function
#   find_xopsupport(<out-var>) and helper variables.
# - If the top-level sets XOP_VENDOR_DIR (CACHE PATH), vendor tree will be used
#   when present unless a user supplied USE_SYSTEM_XOPSUPPORT path is provided.
# - If the user sets USE_SYSTEM_XOPSUPPORT (CACHE PATH, non-empty), that path will
#   be used as the root for include/lib lookup and will *override* the vendor tree.
#
# On success find_xopsupport(<out-var>) will:
#  - set the <out-var> PARENT_SCOPE to TRUE
#  - create imported targets (guarded): XOP::XOPSupport and optionally XOP::IGOR
#  - set cache variables XOP_SUPPORT_ROOT_USED (path) and XOP_SUPPORT_FOUND (BOOL)
#
# On failure it sets <out-var> to FALSE and XOP_SUPPORT_FOUND=FALSE.
#
# NOTE: Do not call find_xopsupport() at include time. Let the caller invoke it.

if(DEFINED _FIND_XOPSUPPORT_INCLUDED)
  return()
endif()
set(_FIND_XOPSUPPORT_INCLUDED TRUE)

# Vendor dir default (top-level can override via cache)
if(NOT DEFINED XOP_VENDOR_DIR)
  set(XOP_VENDOR_DIR "${CMAKE_SOURCE_DIR}/third_party/XOPToolkit/XOPSupport" CACHE PATH "Vendor XOPSupport tree (fallback)")
endif()

# Allow user to override and provide a system-installed root
if(NOT DEFINED USE_SYSTEM_XOPSUPPORT)
  set(USE_SYSTEM_XOPSUPPORT "" CACHE PATH "Path to system-installed XOPSupport (overrides vendor). Leave empty to use vendored XOPSupport if present.")
endif()

# Internal implementation
function(_find_xopsupport_impl out_var)
  set(_found FALSE)
  set(_include_dir "")
  set(_lib_path "")
  set(_igor_lib_path "")
  set(_root_used "")

  # Helper: check a candidate root for both headers and library
  #
  # Behavior:
  #  - Look for header files in a set of plausible include locations under the root.
  #  - Look for a platform-appropriate library file that follows the vendor naming
  #    convention for 64-bit bits (filename contains "64" marker). We probe an ordered
  #    list of candidate subdirectories (platform-specific preferred first) and only
  #    accept a found library if its filename matches the expected "64" marker.
  #
  # Rationale:
  #  - The project supports only 64-bit builds. Accepting a 32-bit import library
  #    causes LNK4272 and a flood of unresolved symbols. The strict filename check
  #    matches the vendor's convention and gives deterministic behavior.
  function(_check_root_for_xop root)
    # look for headers
    set(_header_names "XOPSupport.h" "XOPSupport/XOPSupport.h")
    message(STATUS "  ** Looking for header file(s) [${_header_names}] in ${root}")

    find_path(_try_include
      NAMES ${_header_names}
      HINTS "${root}" "${root}/include" "${root}/Headers" "${root}/VC" "${root}/XCode"
      NO_DEFAULT_PATH
    )

    #
    # Library detection — strict 64-bit name requirement
    #
    # We support only two platforms (set by higher-level cmake):
    #  - PLATFORM_WIN64  -> Windows x64, expect lib file to include "64" and end with .lib
    #  - PLATFORM_APPLE  -> macOS (Apple), expect lib file to include "64" and use .a
    #
    # Build an ordered list of candidate directories to probe. Probe each dir
    # individually (using find_library scoped to that dir) and verify the found
    # filename contains the expected 64-bit marker. If none match, we consider
    # the root not suitable for 64-bit XOPSupport.
    if(DEFINED PLATFORM_WIN64)
      set(_wanted_lib_name "XOPSupport64")   # prefer explicit 64-suffixed candidate name
      set(_expected_marker "64.lib")         # required marker in filename (Windows)
      set(_candidate_lib_dirs
        "${root}/VC/x64"
        "${root}/VC/x64/Release"
        "${root}/lib64"
        "${root}/lib"
        "${root}/VC"
        "${root}"
      )
    elseif(DEFINED PLATFORM_APPLE)
      set(_wanted_lib_name "XOPSupport64")
      set(_expected_marker "64.a")           # required marker in filename (macOS static lib)
      set(_candidate_lib_dirs
        "${root}/lib64"
        "${root}/lib"
        "${root}/VC"
        "${root}"
      )
    else()
      # Should not happen - callers gate platform support; keep behavior consistent.
      set(_wanted_lib_name "XOPSupport64")
      set(_expected_marker "64")
      set(_candidate_lib_dirs
        "${root}/lib64"
        "${root}/lib"
        "${root}"
      )
    endif()

    message(STATUS "  ** Looking for library with 64-bit naming (want='${_wanted_lib_name}', marker='${_expected_marker}') under ${root}")

    # default: not found
    set(_try_lib "")
    # iterate candidate subdirs and probe them one-by-one
    foreach(_candidate_dir IN LISTS _candidate_lib_dirs)
      if(NOT EXISTS "${_candidate_dir}")
        # skip non-existing candidate dir quickly
        continue()
      endif()

      # Use find_library scoped to this candidate dir only to avoid accidental matches
      find_library(_found_in_dir
        NAMES ${_wanted_lib_name}
        PATHS "${_candidate_dir}"
        NO_DEFAULT_PATH
      )

      if(_found_in_dir)
        # get filename and check marker
        get_filename_component(_found_name "${_found_in_dir}" NAME)  # e.g., XOPSupport64.lib
        string(FIND "${_found_name}" "${_expected_marker}" _marker_index)
        if(NOT _marker_index EQUAL -1)
          # this candidate matches the naming convention — accept it
          set(_try_lib "${_found_in_dir}")
          message(STATUS "  ** Found matching 64-bit library: ${_try_lib} (from ${_candidate_dir})")
          break()
        else()
          # Found a library file in the dir but it does not follow the 64-bit naming convention.
          # Ignore it and continue searching other candidate dirs.
          message(STATUS "  ** Ignoring library (does not match 64-bit naming): ${_found_in_dir}")
          unset(_found_in_dir)
        endif()
      endif()
    endforeach()

    # optional IGOR import lib (useful on Windows) — same strictness: prefer IGOR64
    set(_try_igor "")
    if(DEFINED PLATFORM_WIN64)
      # Probe likely candidate locations as above and accept only IGOR64*.lib
      foreach(_candidate_dir IN LISTS _candidate_lib_dirs)
        if(NOT EXISTS "${_candidate_dir}")
          continue()
        endif()
        find_library(_found_igor_in_dir
          NAMES "IGOR64"
          PATHS "${_candidate_dir}"
          NO_DEFAULT_PATH
        )
        if(_found_igor_in_dir)
          get_filename_component(_igor_name "${_found_igor_in_dir}" NAME)
          string(FIND "${_igor_name}" "64.lib" _igor_marker_index)
          if(NOT _igor_marker_index EQUAL -1)
            set(_try_igor "${_found_igor_in_dir}")
            message(STATUS "  ** Found matching IGOR import lib: ${_try_igor}")
            break()
          else()
            message(STATUS "  ** Ignoring IGOR lib (does not match 64-bit naming): ${_found_igor_in_dir}")
            unset(_found_igor_in_dir)
          endif()
        endif()
      endforeach()
    endif()

    if(_try_include AND _try_lib)
      message(STATUS "  ** Both include headers and library found in ${root}")
      message(STATUS "     Include path: ${_try_include}")
      message(STATUS "     Library file: ${_try_lib}")
      if(_try_igor)
        message(STATUS "     IGOR import lib: ${_try_igor}")
      endif()

      # export values to caller scope of _find_xopsupport_impl
      set(_include_dir "${_try_include}" PARENT_SCOPE)
      set(_lib_path "${_try_lib}" PARENT_SCOPE)
      set(_igor_lib_path "${_try_igor}" PARENT_SCOPE)
      set(_found TRUE PARENT_SCOPE)
      return()
    else()
      message(STATUS "  ** XOPSupport not found in ${root} (headers: ${_try_include}, lib: ${_try_lib})")
      set(_found FALSE PARENT_SCOPE)
    endif()
  endfunction()

  # 1) if user provided USE_SYSTEM_XOPSUPPORT, try it first (authoritative)
  if(USE_SYSTEM_XOPSUPPORT)
    _check_root_for_xop("${USE_SYSTEM_XOPSUPPORT}")
    if(_found)
      set(_root_used "${USE_SYSTEM_XOPSUPPORT}")
    else()
      message(WARNING "FindXOPSupport: USE_SYSTEM_XOPSUPPORT='${USE_SYSTEM_XOPSUPPORT}' provided but XOPSupport header+lib not found there.")
    endif()
  endif()

  # 2) vendor dir
  if(NOT _found AND DEFINED XOP_VENDOR_DIR AND EXISTS "${XOP_VENDOR_DIR}")
    _check_root_for_xop("${XOP_VENDOR_DIR}")
    if(_found)
      set(_root_used "${XOP_VENDOR_DIR}")
    endif()
  endif()

  # If found, create imported targets (guarded) and set cache info
  if(_found)
    message(STATUS "FindXOPSupport: XOPSupport found. include='${_include_dir}', lib='${_lib_path}', root='${_root_used}'")
    if(_igor_lib_path)
      message(STATUS "FindXOPSupport: IGOR import lib found: ${_igor_lib_path}")
    endif()

    if(NOT TARGET XOP::XOPSupport)
      add_library(XOP::XOPSupport INTERFACE IMPORTED)
      set_target_properties(XOP::XOPSupport PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${_include_dir}"
        INTERFACE_LINK_LIBRARIES "${_lib_path}"
      )
    else()
      message(STATUS "FindXOPSupport: XOP::XOPSupport already exists; skipping add_library().")
    endif()

    if(_igor_lib_path AND NOT TARGET XOP::IGOR)
      add_library(XOP::IGOR INTERFACE IMPORTED)
      set_target_properties(XOP::IGOR PROPERTIES
        INTERFACE_LINK_LIBRARIES "${_igor_lib_path}"
      )
    elseif(_igor_lib_path)
      message(STATUS "FindXOPSupport: XOP::IGOR already exists; skipping add_library().")
    endif()

    # record cache-visible values
    set(XOP_SUPPORT_FOUND TRUE CACHE BOOL "XOPSupport available")
    if(_root_used)
      set(XOP_SUPPORT_ROOT_USED "${_root_used}" CACHE PATH "Resolved XOPSupport root used by CMake")
    endif()
    # expose non-cache values to the caller scope
    set(XOP_SUPPORT_INCLUDE_DIR "${_include_dir}" PARENT_SCOPE)
    set(XOP_SUPPORT_LIB_PATH "${_lib_path}" PARENT_SCOPE)
    if(_igor_lib_path)
      set(XOP_SUPPORT_IGOR_LIB_PATH "${_igor_lib_path}" PARENT_SCOPE)
    endif()
  else()
    message(STATUS "FindXOPSupport: XOPSupport not found (vendor='${XOP_VENDOR_DIR}', USE_SYSTEM_XOPSUPPORT='${USE_SYSTEM_XOPSUPPORT}').")
    set(XOP_SUPPORT_FOUND FALSE CACHE BOOL "XOPSupport available")
  endif()

  # return the boolean via out_var (to the caller scope)
  set(${out_var} ${_found} PARENT_SCOPE)
endfunction()

# Public wrapper
function(find_xopsupport out_var)
  message(STATUS "  Trying to configure XOP::XOPSupport and XOP::IGOR with settings:")
  message(STATUS "    XOP_VENDOR_DIR='${XOP_VENDOR_DIR}'")
  message(STATUS "    USE_SYSTEM_XOPSUPPORT='${USE_SYSTEM_XOPSUPPORT}'")
  message(STATUS "    BUILD_XOP='${BUILD_XOP}'")

  if(DEFINED PLATFORM_WIN64)
    message(STATUS "  Configure for PLATFORM_WIN64 (Windows x64)")
  elseif(DEFINED PLATFORM_APPLE)
    message(STATUS "  Configure for PLATFORM_APPLE (macOS / Apple)")
  else()
    message(STATUS "  The current platform is not supported. Will not continue")
    set(${out_var} FALSE PARENT_SCOPE)
    set(XOP_SUPPORT_FOUND FALSE CACHE BOOL "XOPSupport available")
    return()
  endif()

  if(NOT out_var)
    message(FATAL_ERROR "find_xopsupport requires a variable name to return the boolean result")
  endif()

  _find_xopsupport_impl(_local_found)

  set(${out_var} ${_local_found} PARENT_SCOPE)
endfunction()
