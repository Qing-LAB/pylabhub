# FindXOPSupport.cmake - locate XOPToolkit/XOPSupport headers and libraries
#
# Provides:
#   - variable: XOP_SUPPORT_FOUND (ON / OFF)
#   - imported target: XOP::XOPSupport (if found)
#   - imported target: XOP::IGOR (if IGOR import lib found)
#
# Behavior:
#   * Prefers a vendored tree at XOP_VENDOR_DIR (cache variable set by top-level).
#   * If USE_SYSTEM_XOP=ON will search system include and lib paths.
#   * Caller may call find_xopsupport(<outvar>) - <outvar> will be set to TRUE/FALSE.
#
# Usage:
#   set(XOP_VENDOR_DIR ".../third_party/XOPToolkit/XOPSupport" CACHE PATH "...")
#   option(USE_SYSTEM_XOP "Use system-installed XOP" OFF)
#   include(cm/FindXOPSupport.cmake)  or call find_xopsupport(<var>)
#
cmake_minimum_required(VERSION 3.15)

# Guard to avoid multiple inclusion
if(DEFINED XOP_SUPPORT_FIND_INCLUDED)
  return()
endif()
set(XOP_SUPPORT_FIND_INCLUDED TRUE)

# Allow caller to call find_xopsupport(outvar)
function(find_xopsupport outvar)
  # internally call the implementation
  _find_xopsupport_impl()
  if(outvar)
    set(${outvar} ${XOP_SUPPORT_FOUND} PARENT_SCOPE)
  endif()
endfunction()

# Implementation - separate so direct include still configures detection immediately
function(_find_xopsupport_impl)
  if(NOT TARGET XOP::XOPSupport)
      # Default exported variable names (top-level may have set these)
      if(NOT DEFINED XOP_VENDOR_DIR)
        set(XOP_VENDOR_DIR "${CMAKE_SOURCE_DIR}/third_party/XOPToolkit/XOPSupport" CACHE PATH "Local XOPSupport tree")
      endif()

      if(NOT DEFINED USE_SYSTEM_XOP)
        set(USE_SYSTEM_XOP OFF)
      endif()

      set(XOP_SUPPORT_FOUND OFF)
      unset(XOP::XOPSupport CACHE)   # ensure not leftover
      unset(XOP::IGOR CACHE)

      # Helper: check vendor layout heuristics
      function(_check_vendor_tree vendor_dir result_var)
        # vendor dir should contain include/ and lib/ (or similar). We'll check common markers.
        set(_found_vendor FALSE)
        if(EXISTS "${vendor_dir}")
          # check for any header files or include dir
          if(EXISTS "${vendor_dir}/include" OR EXISTS "${vendor_dir}/XOPSupport.h" OR EXISTS "${vendor_dir}/XOPSupport.hpp")
            set(_found_vendor TRUE)
          endif()
        endif()
        set(${result_var} ${_found_vendor} PARENT_SCOPE)
      endfunction()

      # 1) Try vendor tree (unless user specifically requested system)
      set(_vendor_ok FALSE)
      if(NOT USE_SYSTEM_XOP)
        _check_vendor_tree("${XOP_VENDOR_DIR}" _vendor_ok)
      endif()

      if(_vender_ok)
        set(_XOP_ROOT_DIR "${XOP_VENDOR_DIR}")
        set(_xop_root_directory TRUE)
      elseif(EXISTS "${USE_SYSTEM_XOP}")
        set(_XOP_ROOT_DIR "${USE_SYSTEM_XOP}")
        set(_xop_root_directory TRUE)
      else()
        set(_XOP_ROOT_DIR "")
        set(_xop_root_directory FALSE)
      endif()

      if(_xop_root_directory)
        message(STATUS "FindXOPSupport: using vendored XOPSupport at ${_XOP_ROOT_DIR}")
        # include dir candidates
        set(_inc_candidates
          "${_XOP_ROOT_DIR}"
          "${_XOP_ROOT_DIR}/include"
          "${_XOP_ROOT_DIR}/Headers"
        )

        if(DEFINED PLATFORM_WIN64)
            # library candidates (common names) for windows x64
            set(_lib_candidates
              "${_XOP_ROOT_DIR}/lib"
              "${_XOP_ROOT_DIR}/lib64"
              "${_XOP_ROOT_DIR}/lib/Debug"
              "${_XOP_ROOT_DIR}/lib/Release"
              "${_XOP_ROOT_DIR}/lib/x64"
              "${_XOP_ROOT_DIR}/VC"
              "${_XOP_ROOT_DIR}/VC/Release"
              "${_XOP_ROOT_DIR}/VC/Debug"
            )
        elseif(DEFINED PLATFORM_APPLE)
                set(_lib_candidates
              "${_XOP_ROOT_DIR}/lib"
              "${_XOP_ROOT_DIR}/lib64"
              "${_XOP_ROOT_DIR}/lib/Release"
              "${_XOP_ROOT_DIR}/XCode"
              "${_XOP_ROOT_DIR}/XCode/Release"
              "${_XOP_ROOT_DIR}/XCode/Debug"
            )
        else()
            message("FindXOPSupport: vendored XOPSupport is only supported on Windows x64 and macOS platforms.")
            set(XOP_SUPPORT_FOUND OFF)
            return()
        endif()

        # prefer .lib for Windows, .a/.dylib/.so on other systems
        # Try to find a library file under vendor dir; if none, still provide include path (headers-only case)
        # Find include:
        foreach(_p IN LISTS _inc_candidates)
          if(EXISTS "${_p}")
            set(_xop_include "${_p}")
            break()
          endif()
        endforeach()

        # Find a library file (best-effort)
        set(_xop_lib "")
        foreach(_p IN LISTS _lib_candidates)
          if(IS_DIRECTORY "${_p}")
            file(GLOB _candidates LIST_DIRECTORIES FALSE
                 "${_p}/*.lib" "${_p}/*.a" "${_p}/*.so" "${_p}/*.dylib" "${_p}/*.dll"
                 "${_p}/*XOPSupport*.lib" "${_p}/*XOPSupport*.a" "${_p}/*XOPSupport*.so" "${_p}/*XOPSupport*.dylib")
            if(_candidates)
              list(SORT _candidates)
              list(GET _candidates 0 _xop_lib)
              break()
            endif()
          endif()
        endforeach()

        # Create imported target if possible
        if(DEFINED _xop_include)
          add_library(XOP::XOPSupport UNKNOWN IMPORTED GLOBAL)
          set_target_properties(XOP::XOPSupport PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${_xop_include}"
          )
          if(_xop_lib)
            # set imported location for the single-configuration generator case
            set_target_properties(XOP::XOPSupport PROPERTIES
              IMPORTED_LOCATION "${_xop_lib}"
            )
          endif()
          set(XOP_SUPPORT_FOUND ON)
        else()
          message(WARNING "FindXOPSupport: vendored tree found but no include directory detected under ${XOP_VENDOR_DIR}. Will fall back to system search (if allowed).")
          set(XOP_SUPPORT_FOUND OFF)
        endif()
      endif()
  else()
      message(STATUS "FindXOPSupport: XOP::XOPSupport target already defined, skipping detection.")
      set(XOP_SUPPORT_FOUND ON)
  endif()

  # 2) If not vendored or USE_SYSTEM_XOP, try system paths
  if(NOT XOP_SUPPORT_FOUND AND (USE_SYSTEM_XOP OR NOT _vendor_ok))
    message(STATUS "FindXOPSupport: searching system paths for XOPSupport (USE_SYSTEM_XOP=${USE_SYSTEM_XOP})")
    # common header name; allow override by XOP_HEADER_NAME
    if(NOT DEFINED XOP_HEADER_NAME)
      set(XOP_HEADER_NAME "XOPSupport.h")
    endif()

    find_path(_sys_xop_include
      NAMES ${XOP_HEADER_NAME}
      PATHS
        ENV CPATH
        ENV CPLUS_INCLUDE_PATH
        /usr/local/include /usr/include /opt/local/include
      NO_DEFAULT_PATH
    )

    # try default search (allow default paths)
    if(NOT _sys_xop_include)
      find_path(_sys_xop_include NAMES ${XOP_HEADER_NAME})
    endif()

    # find library (heuristic)
    find_library(_sys_xop_lib
      NAMES XOPSupport xopsupport XOPSupport64
      PATHS /usr/local/lib /usr/lib /opt/local/lib
      NO_DEFAULT_PATH
    )
    if(NOT _sys_xop_lib)
      find_library(_sys_xop_lib NAMES XOPSupport xopsupport XOPSupport64)
    endif()

    if(_sys_xop_include)
      add_library(XOP::XOPSupport UNKNOWN IMPORTED GLOBAL)
      set_target_properties(XOP::XOPSupport PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${_sys_xop_include}"
      )
      if(_sys_xop_lib)
        set_target_properties(XOP::XOPSupport PROPERTIES
          IMPORTED_LOCATION "${_sys_xop_lib}"
        )
      endif()
      set(XOP_SUPPORT_FOUND ON)
      message(STATUS "FindXOPSupport: found system XOPSupport include: ${_sys_xop_include} lib: ${_sys_xop_lib}")
    else()
      message(STATUS "FindXOPSupport: system XOPSupport headers not found.")
      set(XOP_SUPPORT_FOUND OFF)
    endif()
  endif()

  # 3) Optional: try to locate IGOR import lib (Windows) and provide XOP::IGOR target
  unset(_igor_lib)
  if(MSVC OR WIN32)
    # well-known IGOR import lib names
    find_library(_igor_lib
      NAMES IGOR IGOR64 Igor64
      PATHS
        "${XOP_VENDOR_DIR}/lib" "${XOP_VENDOR_DIR}/Lib" "${CMAKE_SOURCE_DIR}/third_party/XOPToolkit"
        /usr/local/lib /usr/lib
      NO_DEFAULT_PATH
    )
    if(NOT _igor_lib)
      find_library(_igor_lib NAMES IGOR IGOR64 Igor64)
    endif()
    if(_igor_lib)
      add_library(XOP::IGOR UNKNOWN IMPORTED GLOBAL)
      set_target_properties(XOP::IGOR PROPERTIES IMPORTED_LOCATION "${_igor_lib}")
      message(STATUS "FindXOPSupport: found IGOR import lib: ${_igor_lib}")
    endif()
  endif()

  # Expose cached result
  set(XOP_SUPPORT_FOUND ${XOP_SUPPORT_FOUND} CACHE BOOL "XOP support found" FORCE)

  # Provide backward compatible variable if caller used _xop_found previously
  set(_xop_found ${XOP_SUPPORT_FOUND} PARENT_SCOPE)
endfunction()

# Also set an exported alias variable for easier checks
if(XOP_SUPPORT_FOUND)
  set(XOP_SUPPORT_FOUND TRUE)
else()
  set(XOP_SUPPORT_FOUND FALSE)
endif()
