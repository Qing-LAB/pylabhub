# third_party/cmake/XOPToolKit.cmake
#
# Finds the XOP Toolkit SDK using a robust, manual search implementation.
#
include(ThirdPartyPolicyAndHelper)

# --- XOP Build Policy ---
option(BUILD_XOP "Build the pylabhub XOP plugin (only supported on macOS and Windows x64)" ON)
set(XOP_VENDOR_DIR "${CMAKE_SOURCE_DIR}/third_party/XOPToolkit/XOPSupport" CACHE PATH "Default path for a vendored XOPSupport tree.")

if(BUILD_XOP AND NOT (PLATFORM_WIN64 OR PLATFORM_APPLE))
  message(WARNING "[pylabhub-third-party] BUILD_XOP is ON but the current platform is not supported. Disabling XOP build.")
  set(BUILD_XOP OFF CACHE BOOL "Disabling XOP build on unsupported platform." FORCE)
endif()

if(NOT BUILD_XOP)
  message(STATUS "[pylabhub-third-party] BUILD_XOP is OFF. Skipping XOPToolKit discovery.")
  return()
endif()

message(STATUS "[pylabhub-third-party] Configuring XOPToolKit dependency...")

# ----------------------------------------------------------------------------
# Internal implementation for finding XOP Support SDK
# ----------------------------------------------------------------------------
function(_find_xopsupport_impl)
  set(_found FALSE)
  set(_include_dir "")
  set(_lib_path "")
  set(_igor_lib_path "")
  set(_root_used "")

  # Helper function: check a given root for headers+lib
  function(_check_root_for_xop root)
    if(NOT EXISTS "${root}")
      message(STATUS "  ** Search root does not exist: ${root}")
      set(_found FALSE PARENT_SCOPE)
      return()
    endif()

    find_path(_try_include
      NAMES "XOP.h"
      HINTS "${root}"
      PATH_SUFFIXES "include" "XOP Support" # Check root/include, and root/XOP Support
      NO_DEFAULT_PATH
    )

    if(NOT _try_include)
      # As a fallback, check the root itself
      if(EXISTS "${root}/XOP.h")
        set(_try_include "${root}")
      endif()
    endif()

    if(PLATFORM_WIN64)
      set(_wanted_lib_names "XOPSupport64")
      set(_expected_marker "64.lib")
      set(_candidate_lib_dirs
        "${root}/VC/x64"
        "${root}/VC/x64/Release"
        "${root}/lib64"
        "${root}/lib"
        "${root}/VC"
        "${root}"
      )
    elseif(PLATFORM_APPLE)
      set(_wanted_lib_names "XOPSupport64" "libXOPSupport64")
      set(_expected_marker "64.a")
      set(_candidate_lib_dirs
        "${root}/Xcode"
        "${root}/Xcode/Release"
        "${root}/lib64"
        "${root}/lib"
        "${root}"
      )
    else()
      set(_wanted_lib_names "")
      set(_expected_marker "")
      set(_candidate_lib_dirs "")
    endif()

    message(STATUS "  ** Looking for library (names='${_wanted_lib_names}') in candidate dirs...")

    set(_try_lib "")
    foreach(_candidate_dir IN LISTS _candidate_lib_dirs)
      if(NOT EXISTS "${_candidate_dir}")
        continue()
      endif()

      find_library(_found_in_dir
        NAMES ${_wanted_lib_names}
        PATHS "${_candidate_dir}"
        NO_DEFAULT_PATH
      )

      if(_found_in_dir)
        get_filename_component(_found_name "${_found_in_dir}" NAME)
        string(FIND "${_found_name}" "${_expected_marker}" _marker_index)
        if(NOT _marker_index EQUAL -1)
          set(_try_lib "${_found_in_dir}")
          message(STATUS "  ** Found matching 64-bit library: ${_try_lib}")
          break()
        else()
          message(STATUS "  ** Ignoring library (does not match 64-bit naming): ${_found_in_dir}")
          unset(_found_in_dir CACHE)
        endif()
      endif()
    endforeach()

    set(_try_igor "")
    if(PLATFORM_WIN64)
      foreach(_candidate_dir IN LISTS _candidate_lib_dirs)
        if(NOT EXISTS "${_candidate_dir}")
          continue()
        endif()
        find_library(_found_igor_in_dir
          NAMES "IGOR64" "IGOR"
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
            unset(_found_igor_in_dir CACHE)
          endif()
        endif()
      endforeach()
    endif()

    if(_try_include AND _try_lib)
      set(_include_dir "${_try_include}" PARENT_SCOPE)
      set(_lib_path "${_try_lib}" PARENT_SCOPE)
      set(_igor_lib_path "${_try_igor}" PARENT_SCOPE)
      set(_found TRUE PARENT_SCOPE)
    else()
      set(_found FALSE PARENT_SCOPE)
    endif()
  endfunction()

  # --- Search Logic ---
  # 1. Prefer user-provided system path
  if(DEFINED USE_SYSTEM_XOPSUPPORT)
    message(STATUS "[pylabhub-third-party] Checking user-provided XOP SDK path: ${USE_SYSTEM_XOPSUPPORT}")
    _check_root_for_xop("${USE_SYSTEM_XOPSUPPORT}")
    set(_root_used "${USE_SYSTEM_XOPSUPPORT}")
  endif()

  # 2. Fallback to vendored path
  if(NOT _found)
    message(STATUS "[pylabhub-third-party] Checking vendored XOP SDK path: ${XOP_VENDOR_DIR}")
    _check_root_for_xop("${XOP_VENDOR_DIR}")
    set(_root_used "${XOP_VENDOR_DIR}")
  endif()

  # --- Set parent scope variables ---
  if(_found)
    set(XOP_SDK_INCLUDE_DIR "${_include_dir}" PARENT_SCOPE)
    set(XOP_SDK_LIBRARY "${_lib_path}" PARENT_SCOPE)
    set(XOP_SDK_IGOR_LIBRARY "${_igor_lib_path}" PARENT_SCOPE)
    set(XOP_SDK_FOUND TRUE PARENT_SCOPE)
    message(STATUS "[pylabhub-third-party] Found XOP SDK in: ${_root_used}")
    message(STATUS "  - Includes: ${XOP_SDK_INCLUDE_DIR}")
    message(STATUS "  - Library: ${XOP_SDK_LIBRARY}")
  else()
    set(XOP_SDK_FOUND FALSE PARENT_SCOPE)
  endif()
endfunction()

# ----------------------------------------------------------------------------
# Main script execution
# ----------------------------------------------------------------------------
_find_xopsupport_impl()

if(NOT XOP_SDK_FOUND)
  message(FATAL_ERROR "[pylabhub-third-party] Could not find a valid XOP SDK. "
    "Please set USE_SYSTEM_XOPSUPPORT to the root of your XOP SDK installation, "
    "or ensure the vendored copy at '${XOP_VENDOR_DIR}' is complete.")
endif()

# --- Create and populate the wrapper target ---
_expose_wrapper(pylabhub_xoptoolkit pylabhub::third_party::XOPToolKit)

target_include_directories(pylabhub_xoptoolkit INTERFACE
  $<BUILD_INTERFACE:${XOP_SDK_INCLUDE_DIR}>
)

target_link_libraries(pylabhub_xoptoolkit INTERFACE
  "${XOP_SDK_LIBRARY}"
)

if(XOP_SDK_IGOR_LIBRARY)
  target_link_libraries(pylabhub_xoptoolkit INTERFACE "${XOP_SDK_IGOR_LIBRARY}")
endif()

if(PLATFORM_APPLE)
  target_link_libraries(pylabhub_xoptoolkit INTERFACE
    "-framework Cocoa"
    "-framework CoreFoundation"
    "-framework CoreServices"
    "-framework Carbon"
    "-framework AudioToolbox"
  )
elseif(PLATFORM_WIN64)
  target_link_libraries(pylabhub_xoptoolkit INTERFACE version)
endif()

message(STATUS "[pylabhub-third-party] XOPToolKit configuration complete.")