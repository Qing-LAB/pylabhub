# src/IgorXOP/CMakeLists.txt - build pylabhub XOP plugin (Igor Pro)
#
# This file assumes it is included from the top-level CMakeLists via add_subdirectory
# and that cmake/FindXOPSupport.cmake was included earlier.
#
# It is defensive: it only configures when:
#   - BUILD_XOP is ON (top-level option), AND
#   - platform is supported (Windows x64 or macOS), AND
#   - XOP support (vendored or system) was found
#
# Clear diagnostic messages are emitted in every decision branch.

# Declared at top-level; respect top-level build switch
if(NOT DEFINED BUILD_XOP)
  option(BUILD_XOP "Attempt to build Igor XOP addon (only supported on Windows x64 and macOS)" ON)
endif()

if(NOT BUILD_XOP)
  message(STATUS "IgorXOP: BUILD_XOP=OFF; skipping XOP subproject.")
  return()
endif()

# Platform detection (consistent with top-level macros)
if(NOT (DEFINED PLATFORM_WIN64 OR DEFINED PLATFORM_APPLE))
  # If the top-level didn't define the PLATFORM_* macros, we check now.
  if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(PLATFORM_WIN64 1)
  elseif(APPLE)
    set(PLATFORM_APPLE 1)
  endif()
endif()

if(NOT (DEFINED PLATFORM_WIN64 OR DEFINED PLATFORM_APPLE))
  message(STATUS "IgorXOP: skipping — XOP only supported on Windows x64 or macOS.")
  return()
endif()

# Prefer top-level variable for vendor dir (set by top-level)
if(NOT DEFINED XOP_VENDOR_DIR)
  set(XOP_VENDOR_DIR "${CMAKE_SOURCE_DIR}/../../third_party/XOPToolkit/XOPSupport" CACHE PATH "Local XOPSupport tree (fallback from IgorXOP)")
endif()

# Use the find helper (this sets XOP_SUPPORT_FOUND and may create XOP::XOPSupport)
if(NOT DEFINED XOP_SUPPORT_FOUND)
  # call the function provided by cmake/FindXOPSupport.cmake
  find_xopsupport(_xop_found)
else()
  # If already set at top-level, expose to local variable
  set(_xop_found ${XOP_SUPPORT_FOUND})
endif()

if(NOT _xop_found)
  message(WARNING "IgorXOP: XOPSupport not available (vendor missing and no system XOP). Skipping Igor XOP target.")
  return()
endif()

# At this point XOPSupport is available (XOP::XOPSupport imported target exists)
message(STATUS "IgorXOP: XOPSupport available; configuring XOP target.")

# --- target / sources ---
set(XOP_NAME pylabhub-xop)
set(XOP_TARGET ${XOP_NAME})

set(XOP_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/WaveAccess.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/WaveAccess.h
  ${CMAKE_CURRENT_SOURCE_DIR}/resource.h
  ${CMAKE_CURRENT_SOURCE_DIR}/WaveAccess.r
  ${CMAKE_CURRENT_SOURCE_DIR}/WaveAccessWinCustom.rc
  # add other sources here
)

add_library(${XOP_TARGET} SHARED ${XOP_SOURCES})
add_library(pylabhub::${XOP_TARGET} ALIAS ${XOP_TARGET})

if(XOPSUPPORT_INCLUDE_DIR)
    include_directories(${XOPSUPPORT_INCLUDE_DIR})
endif()

if(XOPSUPPORT_LIBRARY)
    target_link_libraries(${XOP_TARGET} ${XOPSUPPORT_LIBRARY})
endif()

# Windows-specific output name/extension adjustments (produce .xop instead of .dll)
if(WIN32)
  # produce pylabhub-xop64.xop (consistent)
  set_target_properties(${XOP_TARGET} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "${XOP_NAME}64"
    SUFFIX ".xop"
  )
endif()

# --- compile features and MSVC flags ---
target_compile_features(${XOP_TARGET} PRIVATE cxx_std_17)

if(MSVC)
  # Prefer static CRT to match vendored XOPSupport's expectation (if set globally that is fine).
  # Set per-target options so other targets aren't affected unexpectedly.
  target_compile_definitions(${XOP_TARGET} PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_compile_options(${XOP_TARGET} PRIVATE /utf-8)
  message(STATUS "IgorXOP: MSVC options applied to ${XOP_TARGET}")
endif()

# --- include directories ---
# Prefer the imported target's advertised include dirs if present
if(TARGET XOP::XOPSupport)
  target_include_directories(${XOP_TARGET} PRIVATE $<TARGET_PROPERTY:XOP::XOPSupport,INTERFACE_INCLUDE_DIRECTORIES>)
else()
  # Fallback: use the vendor dir if present
  if(DEFINED XOP_VENDOR_DIR AND EXISTS "${XOP_VENDOR_DIR}")
    target_include_directories(${XOP_TARGET} PRIVATE "${XOP_VENDOR_DIR}")
  endif()
endif()

# --- link libraries ---
if(TARGET XOP::XOPSupport)
  target_link_libraries(${XOP_TARGET} PRIVATE XOP::XOPSupport)
  message(STATUS "IgorXOP: linked -> XOP::XOPSupport")
else()
  message(WARNING "IgorXOP: XOP::XOPSupport imported target not present; linking may fail.")
endif()

# IGOR import lib (optional)
if(TARGET XOP::IGOR)
  target_link_libraries(${XOP_TARGET} PRIVATE XOP::IGOR)
  message(STATUS "IgorXOP: linked -> XOP::IGOR")
else()
  message(STATUS "IgorXOP: IGOR import lib not found (this may be okay on macOS).")
endif()

# Windows specifics: link required system libs
if(WIN32)
  target_link_libraries(${XOP_TARGET} PRIVATE version)
endif()

# --- post-build steps (help/resource copy) ---
set(XOP_HELP "${CMAKE_CURRENT_SOURCE_DIR}/${XOP_NAME} Help.ihf")
if(EXISTS "${XOP_HELP}")
  add_custom_command(TARGET ${XOP_TARGET} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${XOP_HELP}"
      "$<TARGET_FILE_DIR:${XOP_TARGET}>/${XOP_NAME} Help.ihf"
    COMMENT "Copying XOP help file next to built xop"
  )
endif()

# Install rule
install(TARGETS ${XOP_TARGET}
  RUNTIME DESTINATION dist
  BUNDLE DESTINATION dist
  LIBRARY DESTINATION dist
)

message(STATUS "IgorXOP: configured XOP target '${XOP_TARGET}'.")
