# cmake/FindXOPSupport.cmake
#
# Function: find_xopsupport(<out_var>)
# - Searches for a local XOP Toolkit vendor tree (default: ${CMAKE_SOURCE_DIR}/third_party/XOPToolkit/XOPSupport)
# - If found, creates imported targets:
#     XOP::XOPSupport  (library: .lib / .a)
#     XOP::IGOR        (import lib: IGOR64.lib / IGOR.lib)  [Windows only if present]
# - Sets INTERFACE_INCLUDE_DIRECTORIES to point at the vendor include dir
# - Returns boolean in <out_var> (TRUE if at least header dir found; target presence is indicated by TARGET)
#
# Usage in top-level CMakeLists.txt:
#   include(cmake/FindXOPSupport.cmake)
#   find_xopsupport(_xop_found)
#   if(_xop_found)
#     message(STATUS "XOPSupport available")
#   endif()

function(find_xopsupport out_var)
  # default vendor dir; allow override by cache var from the caller
  if(NOT DEFINED XOP_VENDOR_DIR)
    set(XOP_VENDOR_DIR "${CMAKE_SOURCE_DIR}/third_party/XOPToolkit/XOPSupport" CACHE PATH "Path to local XOPSupport vendor tree")
  endif()

  set(_found FALSE)
  set(_vendor "${XOP_VENDOR_DIR}")

  # Basic existence check for vendor directory
  if(EXISTS "${_vendor}")
    message(STATUS "XOPSupport vendor directory exists: ${_vendor}")
    set(_found TRUE)
  else()
    message(STATUS "XOPSupport vendor directory NOT found at: ${_vendor}")
  endif()

  # Helper: set INTERFACE_INCLUDE_DIR value (headers live directly in XOPSupport/)
  set(_include_dir "${_vendor}")

  # Clear any previously-created imported targets of same names to avoid redefinition warnings
  if(TARGET XOP::XOPSupport)
    # keep it; do not redefine
  endif()

  # Platform-specific detection and imported-target creation
  if(EXISTS "${_vendor}")
    if(WIN32)
      # prefer VC subdirectory (Visual C++ import libs)
      set(_vc_dir "${_vendor}/VC")
      if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        # 64-bit
        if(EXISTS "${_vc_dir}/XOPSupport64.lib")
          if(NOT TARGET XOP::XOPSupport)
            add_library(XOP::XOPSupport UNKNOWN IMPORTED)
            set_target_properties(XOP::XOPSupport PROPERTIES
              IMPORTED_LOCATION "${_vc_dir}/XOPSupport64.lib"
              INTERFACE_INCLUDE_DIRECTORIES "${_include_dir}"
            )
            message(STATUS "Imported XOP::XOPSupport -> ${_vc_dir}/XOPSupport64.lib")
          endif()
        elseif(EXISTS "${_vendor}/XOPSupport64.lib")
          if(NOT TARGET XOP::XOPSupport)
            add_library(XOP::XOPSupport UNKNOWN IMPORTED)
            set_target_properties(XOP::XOPSupport PROPERTIES
              IMPORTED_LOCATION "${_vendor}/XOPSupport64.lib"
              INTERFACE_INCLUDE_DIRECTORIES "${_include_dir}"
            )
            message(STATUS "Imported XOP::XOPSupport -> ${_vendor}/XOPSupport64.lib")
          endif()
        endif()

        # IGOR import lib (resolves tokens like MemError/NewPtr/etc.)
        if(EXISTS "${_vc_dir}/IGOR64.lib")
          if(NOT TARGET XOP::IGOR)
            add_library(XOP::IGOR UNKNOWN IMPORTED)
            set_target_properties(XOP::IGOR PROPERTIES
              IMPORTED_LOCATION "${_vc_dir}/IGOR64.lib"
              INTERFACE_INCLUDE_DIRECTORIES "${_include_dir}"
            )
            message(STATUS "Imported XOP::IGOR -> ${_vc_dir}/IGOR64.lib")
          endif()
        elseif(EXISTS "${_vendor}/IGOR64.lib")
          if(NOT TARGET XOP::IGOR)
            add_library(XOP::IGOR UNKNOWN IMPORTED)
            set_target_properties(XOP::IGOR PROPERTIES
              IMPORTED_LOCATION "${_vendor}/IGOR64.lib"
              INTERFACE_INCLUDE_DIRECTORIES "${_include_dir}"
            )
            message(STATUS "Imported XOP::IGOR -> ${_vendor}/IGOR64.lib")
          endif()
        endif()

      else()
        # 32-bit
        if(EXISTS "${_vc_dir}/XOPSupport.lib")
          if(NOT TARGET XOP::XOPSupport)
            add_library(XOP::XOPSupport UNKNOWN IMPORTED)
            set_target_properties(XOP::XOPSupport PROPERTIES
              IMPORTED_LOCATION "${_vc_dir}/XOPSupport.lib"
              INTERFACE_INCLUDE_DIRECTORIES "${_include_dir}"
            )
            message(STATUS "Imported XOP::XOPSupport -> ${_vc_dir}/XOPSupport.lib")
          endif()
        elseif(EXISTS "${_vendor}/XOPSupport.lib")
          if(NOT TARGET XOP::XOPSupport)
            add_library(XOP::XOPSupport UNKNOWN IMPORTED)
            set_target_properties(XOP::XOPSupport PROPERTIES
              IMPORTED_LOCATION "${_vendor}/XOPSupport.lib"
              INTERFACE_INCLUDE_DIRECTORIES "${_include_dir}"
            )
            message(STATUS "Imported XOP::XOPSupport -> ${_vendor}/XOPSupport.lib")
          endif()
        endif()

        if(EXISTS "${_vc_dir}/IGOR.lib")
          if(NOT TARGET XOP::IGOR)
            add_library(XOP::IGOR UNKNOWN IMPORTED)
            set_target_properties(XOP::IGOR PROPERTIES
              IMPORTED_LOCATION "${_vc_dir}/IGOR.lib"
              INTERFACE_INCLUDE_DIRECTORIES "${_include_dir}"
            )
            message(STATUS "Imported XOP::IGOR -> ${_vc_dir}/IGOR.lib")
          endif()
        elseif(EXISTS "${_vendor}/IGOR.lib")
          if(NOT TARGET XOP::IGOR)
            add_library(XOP::IGOR UNKNOWN IMPORTED)
            set_target_properties(XOP::IGOR PROPERTIES
              IMPORTED_LOCATION "${_vendor}/IGOR.lib"
              INTERFACE_INCLUDE_DIRECTORIES "${_include_dir}"
            )
            message(STATUS "Imported XOP::IGOR -> ${_vendor}/IGOR.lib")
          endif()
        endif()
      endif()

    elseif(APPLE)
      #
      # macOS: XOPSupport typically provides a static lib under Xcode/ or libXOPSupport64.a in vendor root.
      # Also check for architecture-specific names (arm64 vs x86_64) if present.
      #
      set(_xcode_dir "${_vendor}/Xcode")
      # prefer lib under Xcode/
      if(EXISTS "${_xcode_dir}/libXOPSupport64.a")
        if(NOT TARGET XOP::XOPSupport)
          add_library(XOP::XOPSupport STATIC IMPORTED)
          set_target_properties(XOP::XOPSupport PROPERTIES
            IMPORTED_LOCATION "${_xcode_dir}/libXOPSupport64.a"
            INTERFACE_INCLUDE_DIRECTORIES "${_include_dir}"
          )
          message(STATUS "Imported XOP::XOPSupport -> ${_xcode_dir}/libXOPSupport64.a")
        endif()
      elseif(EXISTS "${_vendor}/libXOPSupport64.a")
        if(NOT TARGET XOP::XOPSupport)
          add_library(XOP::XOPSupport STATIC IMPORTED)
          set_target_properties(XOP::XOPSupport PROPERTIES
            IMPORTED_LOCATION "${_vendor}/libXOPSupport64.a"
            INTERFACE_INCLUDE_DIRECTORIES "${_include_dir}"
          )
          message(STATUS "Imported XOP::XOPSupport -> ${_vendor}/libXOPSupport64.a")
        endif()
      else()
        # Check for architecture-specific filenames that some distributions might use
        if(EXISTS "${_xcode_dir}/libXOPSupport_arm64.a")
          if(NOT TARGET XOP::XOPSupport)
            add_library(XOP::XOPSupport STATIC IMPORTED)
            set_target_properties(XOP::XOPSupport PROPERTIES
              IMPORTED_LOCATION "${_xcode_dir}/libXOPSupport_arm64.a"
              INTERFACE_INCLUDE_DIRECTORIES "${_include_dir}"
            )
            message(STATUS "Imported XOP::XOPSupport -> ${_xcode_dir}/libXOPSupport_arm64.a")
          endif()
        endif()
        if(EXISTS "${_xcode_dir}/libXOPSupport_x86_64.a")
          if(NOT TARGET XOP::XOPSupport)
            add_library(XOP::XOPSupport STATIC IMPORTED)
            set_target_properties(XOP::XOPSupport PROPERTIES
              IMPORTED_LOCATION "${_xcode_dir}/libXOPSupport_x86_64.a"
              INTERFACE_INCLUDE_DIRECTORIES "${_include_dir}"
            )
            message(STATUS "Imported XOP::XOPSupport -> ${_xcode_dir}/libXOPSupport_x86_64.a")
          endif()
        endif()
      endif()

      # macOS doesn't supply IGOR import lib in the same way; usually libXOPSupport is self-contained.
      # If vendor provides an IGOR import library, check and import it as well:
      if(EXISTS "${_vendor}/IGOR64.lib" OR EXISTS "${_xcode_dir}/IGOR64.lib")
        set(_maybe_igor "${_vendor}/IGOR64.lib")
        if(EXISTS "${_xcode_dir}/IGOR64.lib")
          set(_maybe_igor "${_xcode_dir}/IGOR64.lib")
        endif()
        if(NOT TARGET XOP::IGOR)
          add_library(XOP::IGOR UNKNOWN IMPORTED)
          set_target_properties(XOP::IGOR PROPERTIES
            IMPORTED_LOCATION "${_maybe_igor}"
            INTERFACE_INCLUDE_DIRECTORIES "${_include_dir}"
          )
          message(STATUS "Imported XOP::IGOR -> ${_maybe_igor}")
        endif()
      endif()

    else()
      # Linux / other Unices: check for libXOPSupport64.a or libXOPSupport.a at vendor root
      if(EXISTS "${_vendor}/libXOPSupport64.a")
        if(NOT TARGET XOP::XOPSupport)
          add_library(XOP::XOPSupport STATIC IMPORTED)
          set_target_properties(XOP::XOPSupport PROPERTIES
            IMPORTED_LOCATION "${_vendor}/libXOPSupport64.a"
            INTERFACE_INCLUDE_DIRECTORIES "${_include_dir}"
          )
          message(STATUS "Imported XOP::XOPSupport -> ${_vendor}/libXOPSupport64.a")
        endif()
      elseif(EXISTS "${_vendor}/libXOPSupport.a")
        if(NOT TARGET XOP::XOPSupport)
          add_library(XOP::XOPSupport STATIC IMPORTED)
          set_target_properties(XOP::XOPSupport PROPERTIES
            IMPORTED_LOCATION "${_vendor}/libXOPSupport.a"
            INTERFACE_INCLUDE_DIRECTORIES "${_include_dir}"
          )
          message(STATUS "Imported XOP::XOPSupport -> ${_vendor}/libXOPSupport.a")
        endif()
      endif()
    endif()
  endif()  # vendor exists

  #
  # Finalize: If we set any imported target, ensure they carry the include dir
  #
  if(TARGET XOP::XOPSupport)
    get_target_property(_loc XOP::XOPSupport IMPORTED_LOCATION)
    if(_loc)
      message(STATUS "XOP::XOPSupport mapped to: ${_loc}")
    endif()
    # Ensure include dir is set even if some imported libs didn't set it
    set_target_properties(XOP::XOPSupport PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${_include_dir}")
  endif()

  if(TARGET XOP::IGOR)
    set_target_properties(XOP::IGOR PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${_include_dir}")
  endif()

  # Output boolean result to caller
  if(TARGET XOP::XOPSupport OR EXISTS "${_include_dir}")
    set(${out_var} TRUE PARENT_SCOPE)
  else()
    set(${out_var} FALSE PARENT_SCOPE)
  endif()
endfunction()
