# cmake/FindXOPSupport.cmake
#
# Usage: include(cmake/FindXOPSupport.cmake) then call find_xopsupport(<result_var>)
#
function(find_xopsupport out_var)
  if(NOT DEFINED XOP_VENDOR_DIR)
    set(XOP_VENDOR_DIR "${CMAKE_SOURCE_DIR}/third_party/XOPToolkit/XOPSupport" CACHE PATH "Path to XOPSupport vendor folder")
  endif()

  set(_found FALSE)

  if(EXISTS "${XOP_VENDOR_DIR}")
    # include dir = vendor root (headers live directly in XOPSupport/)
    set(XOP_INCLUDE_DIR "${XOP_VENDOR_DIR}")
    set(_found TRUE)

    # choose platform-specific libs
    if(WIN32)
      if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        if(EXISTS "${XOP_VENDOR_DIR}/VC/XOPSupport64.lib")
          add_library(XOP::XOPSupport UNKNOWN IMPORTED)
          set_target_properties(XOP::XOPSupport PROPERTIES IMPORTED_LOCATION "${XOP_VENDOR_DIR}/VC/XOPSupport64.lib"
            INTERFACE_INCLUDE_DIRECTORIES "${XOP_INCLUDE_DIR}")
        elseif(EXISTS "${XOP_VENDOR_DIR}/XOPSupport64.lib")
          add_library(XOP::XOPSupport UNKNOWN IMPORTED)
          set_target_properties(XOP::XOPSupport PROPERTIES IMPORTED_LOCATION "${XOP_VENDOR_DIR}/XOPSupport64.lib"
            INTERFACE_INCLUDE_DIRECTORIES "${XOP_INCLUDE_DIR}")
        endif()

        if(EXISTS "${XOP_VENDOR_DIR}/IGOR64.lib")
          add_library(XOP::IGOR UNKNOWN IMPORTED)
          set_target_properties(XOP::IGOR PROPERTIES IMPORTED_LOCATION "${XOP_VENDOR_DIR}/IGOR64.lib"
            INTERFACE_INCLUDE_DIRECTORIES "${XOP_INCLUDE_DIR}")
        endif()
      else()
        # 32-bit
        if(EXISTS "${XOP_VENDOR_DIR}/VC/XOPSupport.lib")
          add_library(XOP::XOPSupport UNKNOWN IMPORTED)
          set_target_properties(XOP::XOPSupport PROPERTIES IMPORTED_LOCATION "${XOP_VENDOR_DIR}/VC/XOPSupport.lib"
            INTERFACE_INCLUDE_DIRECTORIES "${XOP_INCLUDE_DIR}")
        elseif(EXISTS "${XOP_VENDOR_DIR}/XOPSupport.lib")
          add_library(XOP::XOPSupport UNKNOWN IMPORTED)
          set_target_properties(XOP::XOPSupport PROPERTIES IMPORTED_LOCATION "${XOP_VENDOR_DIR}/XOPSupport.lib"
            INTERFACE_INCLUDE_DIRECTORIES "${XOP_INCLUDE_DIR}")
        endif()

        if(EXISTS "${XOP_VENDOR_DIR}/IGOR.lib")
          add_library(XOP::IGOR UNKNOWN IMPORTED)
          set_target_properties(XOP::IGOR PROPERTIES IMPORTED_LOCATION "${XOP_VENDOR_DIR}/IGOR.lib"
            INTERFACE_INCLUDE_DIRECTORIES "${XOP_INCLUDE_DIR}")
        endif()
      endif()
    elseif(APPLE)
      if(EXISTS "${XOP_VENDOR_DIR}/Xcode/libXOPSupport64.a")
        add_library(XOP::XOPSupport STATIC IMPORTED)
        set_target_properties(XOP::XOPSupport PROPERTIES IMPORTED_LOCATION "${XOP_VENDOR_DIR}/Xcode/libXOPSupport64.a"
          INTERFACE_INCLUDE_DIRECTORIES "${XOP_INCLUDE_DIR}")
      elseif(EXISTS "${XOP_VENDOR_DIR}/libXOPSupport64.a")
        add_library(XOP::XOPSupport STATIC IMPORTED)
        set_target_properties(XOP::XOPSupport PROPERTIES IMPORTED_LOCATION "${XOP_VENDOR_DIR}/libXOPSupport64.a"
          INTERFACE_INCLUDE_DIRECTORIES "${XOP_INCLUDE_DIR}")
      endif()
    else()
      # Linux / other: check for libXOPSupport64.a
      if(EXISTS "${XOP_VENDOR_DIR}/libXOPSupport64.a")
        add_library(XOP::XOPSupport STATIC IMPORTED)
        set_target_properties(XOP::XOPSupport PROPERTIES IMPORTED_LOCATION "${XOP_VENDOR_DIR}/libXOPSupport64.a"
          INTERFACE_INCLUDE_DIRECTORIES "${XOP_INCLUDE_DIR}")
      endif()
    endif()
  endif()

  if(TARGET XOP::XOPSupport)
    set(${out_var} TRUE PARENT_SCOPE)
  else()
    set(${out_var} ${_found} PARENT_SCOPE)
  endif()
endfunction()
