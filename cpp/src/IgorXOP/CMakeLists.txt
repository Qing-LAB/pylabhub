# src/IgorXOP/CMakeLists.txt
# ------------------------------------------------------------------------------
# Build the pylabhub XOP plugin for Igor Pro (Windows x64 and macOS x64)
#
# Produces:
#   - macOS: <name>.xop  (bundle folder with Contents/MacOS/<exec>, Info.plist, etc.)
#   - Windows: <name>.xop (DLL-style runtime)
#
# Notes:
#  - This file is organized so macOS bundle assembly is robust for both Xcode
#    generator and Makefile-style generator.
#  - Packaging is handled by cmake/assemble_xop.cmake which is invoked at POST_BUILD.
# ------------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.18)
project(IgorXOP LANGUAGES C CXX)

# Prevent CMake from injecting absolute link dirs into the RPATH at build time.
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH OFF)
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)

# Make local cmake helpers visible
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

# -------------------------
# Options & published variables
# -------------------------
if(NOT DEFINED BUILD_XOP)
  option(BUILD_XOP "Attempt to build Igor XOP addon (only supported on Windows x64 and macOS)" ON)
endif()

# Published internal state (for caller)
set(BUILD_XOP_FINAL_ENABLED OFF CACHE INTERNAL "Final: IgorXOP will be built")
set(XOP_TARGET_NAME "" CACHE INTERNAL "Final: IgorXOP target name")
set(XOPTOOLKIT_DIR "" CACHE INTERNAL "XOPSupport root used by IgorXOP")
set(XOP_SUPPORT_FOUND OFF CACHE INTERNAL "XOPSupport found (vendor or system)")

message(STATUS "==============================================")
message(STATUS "IgorXOP: BUILD_XOP = ${BUILD_XOP}")

if(NOT BUILD_XOP)
  message(STATUS "IgorXOP: BUILD_XOP=OFF; skipping XOP configuration.")
  message(STATUS "==============================================")
  return()
endif()

# -------------------------
# Find XOPSupport helper (may create imported targets XOP::XOPSupport, XOP::IGOR)
# -------------------------
if(NOT COMMAND find_xopsupport)
  include(cmake/FindXOPSupport.cmake OPTIONAL)
endif()

if(NOT COMMAND find_xopsupport)
  message(WARNING "IgorXOP: find_xopsupport() not available (FindXOPSupport.cmake not found). Skipping XOP.")
  set(BUILD_XOP_FINAL_ENABLED OFF CACHE INTERNAL "Final: IgorXOP will be built")
  set(XOP_SUPPORT_FOUND OFF CACHE INTERNAL "XOPSupport found (vendor or system)")
  message(STATUS "==============================================")
  return()
endif()

find_xopsupport(_xop_found)

if(NOT _xop_found)
  message(WARNING "IgorXOP: XOPSupport not found; skipping XOP configuration.")
  set(BUILD_XOP_FINAL_ENABLED OFF CACHE INTERNAL "Final: IgorXOP will be built")
  set(XOP_SUPPORT_FOUND OFF CACHE INTERNAL "XOPSupport found (vendor or system)")
  if(DEFINED XOP_SUPPORT_ROOT_USED)
    set(XOPTOOLKIT_DIR "${XOP_SUPPORT_ROOT_USED}" CACHE INTERNAL "XOPSupport root used by IgorXOP")
  else()
    set(XOPTOOLKIT_DIR "" CACHE INTERNAL "XOPSupport root used by IgorXOP")
  endif()
  message(STATUS "==============================================")
  return()
endif()

# publish detection result
set(XOP_SUPPORT_FOUND ON CACHE INTERNAL "XOPSupport found (vendor or system)")
if(DEFINED XOP_SUPPORT_ROOT_USED)
  set(XOPTOOLKIT_DIR "${XOP_SUPPORT_ROOT_USED}" CACHE INTERNAL "XOPSupport root used by IgorXOP")
else()
  set(XOPTOOLKIT_DIR "" CACHE INTERNAL "XOPSupport root used by IgorXOP")
endif()

message(STATUS "IgorXOP: XOPSupport detected; XOPSupport root: ${XOPTOOLKIT_DIR}")
message(STATUS "IgorXOP: configuring XOP target (platform behavior follows).")

# -------------------------
# Platform validation & macOS x86_64 detection
# -------------------------
if(NOT (DEFINED PLATFORM_WIN64 OR DEFINED PLATFORM_APPLE))
  message(WARNING "IgorXOP: top-level did not provide PLATFORM_WIN64 or PLATFORM_APPLE. Skipping.")
  set(BUILD_XOP_FINAL_ENABLED OFF CACHE INTERNAL "Final: IgorXOP will be built")
  message(STATUS "==============================================")
  return()
endif()

# Detect macOS x86_64 robustly
set(_macos_x86_64 OFF)
if(DEFINED PLATFORM_APPLE)
  if(DEFINED CMAKE_OSX_ARCHITECTURES)
    list(FIND CMAKE_OSX_ARCHITECTURES "x86_64" _idx)
    if(NOT _idx EQUAL -1)
      set(_macos_x86_64 ON)
    endif()
  endif()
  if(NOT _macos_x86_64 AND DEFINED CMAKE_HOST_SYSTEM_PROCESSOR)
    string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" _proc_lower)
    if(_proc_lower MATCHES "x86_64" OR _proc_lower MATCHES "amd64")
      set(_macos_x86_64 ON)
    endif()
  endif()
  if(NOT _macos_x86_64 AND DEFINED CMAKE_SYSTEM_PROCESSOR)
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _sysproc_lower)
    if(_sysproc_lower MATCHES "x86_64" OR _sysproc_lower MATCHES "amd64")
      set(_macos_x86_64 ON)
    endif()
  endif()

  if(NOT _macos_x86_64)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
      message(WARNING "IgorXOP: macOS pointer size is 64-bit; assuming x86_64. For arm64/universal, set CMAKE_OSX_ARCHITECTURES.")
      set(_macos_x86_64 ON)
    else()
      message(WARNING "IgorXOP: macOS target does not appear to be x86_64. Skipping (this XOP supports macOS x64 only).")
      set(BUILD_XOP_FINAL_ENABLED OFF CACHE INTERNAL "Final: IgorXOP will be built")
      message(STATUS "==============================================")
      return()
    endif()
  endif()
endif()

if(DEFINED PLATFORM_WIN64)
  message(STATUS "IgorXOP: target platform = Windows x64 (PLATFORM_WIN64=ON)")
endif()
if(DEFINED PLATFORM_APPLE)
  message(STATUS "IgorXOP: target platform = macOS (PLATFORM_APPLE=ON); x86_64 = ${_macos_x86_64}")
endif()

# -------------------------
# Sources & optional platform resources
# -------------------------
set(XOP_NAME pylabhubxop)
set(XOP_TARGET ${XOP_NAME})
set(XOP_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/WaveAccess.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/WaveAccess.h
  ${CMAKE_CURRENT_SOURCE_DIR}/resource.h
)

if(DEFINED PLATFORM_APPLE)
  set(_rfile "${CMAKE_CURRENT_SOURCE_DIR}/WaveAccess.r")
  if(EXISTS "${_rfile}")
    list(APPEND XOP_SOURCES "${_rfile}")
    message(STATUS "IgorXOP: adding macOS resource ${_rfile}")
  endif()
endif()

if(DEFINED PLATFORM_WIN64)
  set(_rcfile "${CMAKE_CURRENT_SOURCE_DIR}/WaveAccess.rc")
  if(EXISTS "${_rcfile}")
    list(APPEND XOP_SOURCES "${_rcfile}")
    message(STATUS "IgorXOP: adding Windows resource ${_rcfile}")
  endif()
endif()

# -------------------------
# Target creation: MODULE on macOS, SHARED otherwise
# -------------------------
if(DEFINED PLATFORM_APPLE)
  add_library(${XOP_TARGET} MODULE ${XOP_SOURCES})
else()
  add_library(${XOP_TARGET} SHARED ${XOP_SOURCES})
endif()
add_library(pylabhub::${XOP_TARGET} ALIAS ${XOP_TARGET})

# Ensure no lib prefix and canonical output name (include 64 suffix per toolkit)
set_target_properties(${XOP_TARGET} PROPERTIES
  PREFIX ""
  OUTPUT_NAME "${XOP_NAME}64"
)

# -------------------------
# macOS-specific bundle configuration (PLATFORM_APPLE)
# This block is intentionally explicit so generators (Xcode) treat the target as a bundle.
# -------------------------
if(DEFINED PLATFORM_APPLE)
  # canonical names
  set(XOP_BUNDLE_NAME "${XOP_NAME}64")            # e.g. pylabhubxop64.xop
  set(XOP_BUNDLE_EXECUTABLE "${XOP_NAME}64")      # executable name inside Contents/MacOS
  set(XOP_BUNDLE_IDENTIFIER "edu.asu.physics.qinglab.${XOP_BUNDLE_NAME}")
  set(XOP_BUNDLE_VERSION "1.0")

  # Info.plist template and exports
  set(_plist_in "${CMAKE_CURRENT_SOURCE_DIR}/Info64.plist.in")
  set(_exports_file "${CMAKE_CURRENT_SOURCE_DIR}/Exports.exp")

  # Make the target a macOS bundle (Xcode will treat it accordingly)
  set_target_properties(${XOP_TARGET} PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_BUNDLE_NAME "${XOP_BUNDLE_NAME}"
    MACOSX_BUNDLE_EXECUTABLE "${XOP_BUNDLE_EXECUTABLE}"
    PREFIX ""
    SUFFIX ".xop"
  )

  # configure Info.plist at configure-time (if template exists)
  if(EXISTS "${_plist_in}")
    configure_file("${_plist_in}"
                   "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
                   @ONLY)
    set_target_properties(${XOP_TARGET} PROPERTIES
      MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
    )
    message(STATUS "IgorXOP: configured Info.plist for macOS bundle: ${CMAKE_CURRENT_BINARY_DIR}/Info.plist")
  else()
    message(WARNING "IgorXOP: Info64.plist.in not found; bundle will not have configured Info.plist")
  endif()

  # Avoid automatic code signing in Xcode projects; sign later if needed.
  if(CMAKE_GENERATOR MATCHES "Xcode")
    set_target_properties(${XOP_TARGET} PROPERTIES
      XCODE_ATTRIBUTE_PRODUCT_TYPE "com.apple.product-type.bundle"
      XCODE_ATTRIBUTE_WRAPPER_EXTENSION "xop"
      XCODE_ATTRIBUTE_SKIP_INSTALL "NO"
      XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
      XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO"
      XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY ""
    )
  endif()

  # RPATH settings for loader-relative usage inside the bundle
  set_target_properties(${XOP_TARGET} PROPERTIES
    BUILD_RPATH "@loader_path/../Frameworks"
    INSTALL_RPATH "@loader_path/../Frameworks"
    BUILD_WITH_INSTALL_RPATH TRUE
    SKIP_BUILD_RPATH TRUE
  )

  # exported symbols (prefer Exports.exp)
  if(EXISTS "${_exports_file}")
    target_link_options(${XOP_TARGET} PRIVATE
      $<$<CONFIG:Debug>:-Wl,-exported_symbols_list,${_exports_file}>
      $<$<NOT:$<CONFIG:Debug>>:-Wl,-exported_symbols_list,${_exports_file}>
    )
    message(STATUS "IgorXOP: using exported symbols file ${_exports_file}")
  else()
    # fallback to exporting _XOPMain; keep verbose so user can debug
    message(VERBOSE "IgorXOP: Exports.exp not found; adding explicit export for _XOPMain")
    target_link_options(${XOP_TARGET} PRIVATE
      "-Wl,-exported_symbol,_XOPMain"
    )
  endif()

  # ensure rpath is passed to linker
  target_link_options(${XOP_TARGET} PRIVATE
    $<$<CONFIG:Debug>:-Wl,-rpath,@loader_path/../Frameworks>
    $<$<NOT:$<CONFIG:Debug>>:-Wl,-rpath,@loader_path/../Frameworks>
  )

  # mac frameworks required by libXOPSupport64.a and XOP API
  target_link_libraries(${XOP_TARGET} PRIVATE
    "-framework Cocoa"
    "-framework CoreFoundation"
    "-framework CoreServices"
    "-framework Carbon"
    "-framework AudioToolbox"
  )

  # Packaging step: call cmake -P cmake/assemble_xop.cmake during POST_BUILD.
  # We use generator-expressions so Xcode will embed a script phase with correct values.
  set(_assemble_script "${CMAKE_SOURCE_DIR}/cmake/assemble_xop.cmake")
  if(EXISTS "${_assemble_script}")
    add_custom_command(TARGET ${XOP_TARGET} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E echo "IgorXOP: packaging for $<TARGET_FILE_NAME:${XOP_TARGET}> (config: $<CONFIG>)"
      COMMAND ${CMAKE_COMMAND} -E env CONFIGURATION=$<CONFIG>
              ${CMAKE_COMMAND} -P "${_assemble_script}"
              "$<TARGET_FILE:${XOP_TARGET}>"                             # path to build artifact
              "$<TARGET_FILE_DIR:${XOP_TARGET}>/${XOP_BUNDLE_NAME}.bundle" # desired bundle location
              "${XOP_BUNDLE_NAME}.xop"                                    # final filename (folder)
      COMMENT "Assemble XOP bundle for ${XOP_TARGET}"
      VERBATIM
    )
    message(STATUS "IgorXOP: packaging step registered (assemble_xop.cmake)")
  else()
    message(WARNING "IgorXOP: assemble_xop.cmake not found; bundle assembly will be skipped")
  endif()

  # Provide explicit assemble target (manual invocation possible)
  add_custom_target(assemble_xop_${XOP_TARGET}
    COMMENT "Assemble XOP bundle for ${XOP_TARGET}"
    DEPENDS ${XOP_TARGET}
  )
  add_dependencies(assemble_xop_${XOP_TARGET} ${XOP_TARGET})

  # Optional: build-time assembly target (can be removed if undesired)
  add_custom_target(${XOP_TARGET}_bundle ALL DEPENDS assemble_xop_${XOP_TARGET})
endif()

# -------------------------
# Include directories & linking (prefer imported XOP::XOPSupport)
# -------------------------
if(TARGET XOP::XOPSupport)
  target_include_directories(${XOP_TARGET} PRIVATE
    $<TARGET_PROPERTY:XOP::XOPSupport,INTERFACE_INCLUDE_DIRECTORIES>
  )
  message(STATUS "IgorXOP: using include dirs from XOP::XOPSupport imported target")
elseif(DEFINED XOP_SUPPORT_ROOT_USED AND EXISTS "${XOP_SUPPORT_ROOT_USED}/include")
  target_include_directories(${XOP_TARGET} PRIVATE "${XOP_SUPPORT_ROOT_USED}/include")
  message(STATUS "IgorXOP: using include dir ${XOP_SUPPORT_ROOT_USED}/include")
elseif(DEFINED XOP_VENDOR_DIR AND EXISTS "${XOP_VENDOR_DIR}")
  target_include_directories(${XOP_TARGET} PRIVATE "${XOP_VENDOR_DIR}")
  message(STATUS "IgorXOP: using vendor include dir ${XOP_VENDOR_DIR}")
endif()

# Link to XOPSupport library (prefer imported target)
if(TARGET XOP::XOPSupport)
  target_link_libraries(${XOP_TARGET} PRIVATE XOP::XOPSupport)
  message(STATUS "IgorXOP: linking to XOP::XOPSupport imported target")
else()
  if(DEFINED XOP_SUPPORT_LIB_PATH AND EXISTS "${XOP_SUPPORT_LIB_PATH}")
    target_link_libraries(${XOP_TARGET} PRIVATE "${XOP_SUPPORT_LIB_PATH}")
    message(STATUS "IgorXOP: linking directly to ${XOP_SUPPORT_LIB_PATH}")
  endif()
endif()

# Optional IGOR import lib (unchanged)
if(TARGET XOP::IGOR)
  target_link_libraries(${XOP_TARGET} PRIVATE XOP::IGOR)
  message(STATUS "IgorXOP: linking to XOP::IGOR imported target")
elseif(DEFINED XOP_SUPPORT_IGOR_LIB_PATH AND EXISTS "${XOP_SUPPORT_IGOR_LIB_PATH}")
  target_link_libraries(${XOP_TARGET} PRIVATE "${XOP_SUPPORT_IGOR_LIB_PATH}")
  message(STATUS "IgorXOP: linking to IGOR import lib ${XOP_SUPPORT_IGOR_LIB_PATH}")
endif()

# -------------------------
# Compiler flags & MSVC handling
# -------------------------
target_compile_features(${XOP_TARGET} PRIVATE cxx_std_17)

if(MSVC)
  target_compile_definitions(${XOP_TARGET} PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_compile_options(${XOP_TARGET} PRIVATE /utf-8)
  target_compile_definitions(${XOP_TARGET} PRIVATE
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<CONFIG:Debug>:DEBUG>
    $<$<NOT:$<CONFIG:Debug>>:NDEBUG>
  )
  target_compile_options(${XOP_TARGET} PRIVATE
    $<$<CONFIG:Debug>:/MTd>
    $<$<NOT:$<CONFIG:Debug>>:/MT>
  )

  target_link_options(${XOP_TARGET} PRIVATE /LARGEADDRESSAWARE)

  # Windows export via .def if present
  set(_exports_def "${CMAKE_CURRENT_SOURCE_DIR}/Exports.def")
  if(EXISTS "${_exports_def}")
    target_link_options(${XOP_TARGET} PRIVATE "/DEF:${_exports_def}")
    message(STATUS "IgorXOP: using module definition ${_exports_def} to export XOPMain (Windows)")
  else()
    message(VERBOSE "IgorXOP: Exports.def not found; ensure XOPMain is exported (Windows).")
  endif()
else()
  target_compile_definitions(${XOP_TARGET} PRIVATE
    $<$<CONFIG:Debug>:DEBUG>
    $<$<NOT:$<CONFIG:Debug>>:NDEBUG>
  )
endif()

# Windows system libs
if(DEFINED PLATFORM_WIN64)
  target_link_libraries(${XOP_TARGET} PRIVATE version)
endif()

# -------------------------
# Define XOP-standard macros (MACIGOR/IGOR64 / WINIGOR/IGOR64)
# -------------------------
if(DEFINED PLATFORM_APPLE)
  if(_macos_x86_64)
    target_compile_definitions(${XOP_TARGET} PRIVATE MACIGOR IGOR64)
    message(STATUS "IgorXOP: adding compile definitions for macOS: MACIGOR IGOR64")
  else()
    message(WARNING "IgorXOP: macOS x86_64 detection failed; not adding MACIGOR/IGOR64")
  endif()
endif()

if(DEFINED PLATFORM_WIN64)
  target_compile_definitions(${XOP_TARGET} PRIVATE WINIGOR IGOR64)
  message(STATUS "IgorXOP: adding compile definitions for Windows: WINIGOR IGOR64")
endif()

# -------------------------
# Packaging / helper file discovery (.ihf / .ipf) - these must NOT be embedded
# -------------------------
file(GLOB _xop_ihf_files RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/*.ihf")
file(GLOB _xop_ipf_files RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/*.ipf")
set(_xop_package_files "")
list(APPEND _xop_package_files ${_xop_ihf_files} ${_xop_ipf_files})

if(_xop_package_files)
  message(STATUS "IgorXOP: will install the following helper files next to .xop/.dll:")
  foreach(_f IN LISTS _xop_package_files)
    message(STATUS "  - ${_f}")
  endforeach()
else()
  message(VERBOSE "IgorXOP: no .ihf/.ipf files found in ${CMAKE_CURRENT_SOURCE_DIR}")
endif()

# -------------------------
# Install rules (final packaging)
# -------------------------
if(DEFINED PLATFORM_APPLE)
  install(TARGETS ${XOP_TARGET}
    BUNDLE DESTINATION xop
    LIBRARY DESTINATION xop   # satisfy MODULE install requirements
    INCLUDES DESTINATION include
  )

  # Remove stray lib artifacts from the install tree (best-effort)
  install(CODE "message(STATUS \"IgorXOP: cleaning stray library artifacts from install tree\")
    file(REMOVE \"\${CMAKE_INSTALL_PREFIX}/lib/${XOP_BUNDLE_NAME}.dylib\")
    file(REMOVE \"\${CMAKE_INSTALL_PREFIX}/xop/${XOP_BUNDLE_NAME}.dylib\")
    file(REMOVE \"\${CMAKE_INSTALL_PREFIX}/xop/${XOP_BUNDLE_NAME}.so\")"
    OPTIONAL)
else()
  install(TARGETS ${XOP_TARGET}
    RUNTIME DESTINATION xop
    LIBRARY DESTINATION xop
    INCLUDES DESTINATION include
  )
endif()

# Install helpers (.ihf/.ipf) next to bundle/dll (NOT inside bundle)
if(DEFINED _xop_package_files AND _xop_package_files)
  foreach(_rel IN LISTS _xop_package_files)
    set(_src "${CMAKE_CURRENT_SOURCE_DIR}/${_rel}")
    if(EXISTS "${_src}")
      install(FILES "${_src}" DESTINATION xop)
      message(STATUS "IgorXOP: scheduling install of ${_rel} -> <install-root>/xop/")
    else()
      message(WARNING "IgorXOP: expected package file ${_src} not found at install scheduling time")
    endif()
  endforeach()
else()
  message(VERBOSE "IgorXOP: no packaging/help files to install into xop/")
endif()

# -------------------------
# Publish final state to top-level and print summary
# -------------------------
set(BUILD_XOP_FINAL_ENABLED ON CACHE INTERNAL "Final: IgorXOP will be built")
set(XOP_TARGET_NAME "${XOP_TARGET}" CACHE INTERNAL "Final: IgorXOP target name")

message(STATUS "IgorXOP: configured XOP target '${XOP_TARGET}'.")
message(STATUS "  XOP build target:    ${XOP_TARGET}")
if(XOPTOOLKIT_DIR)
  message(STATUS "  XOPTOOLKIT used:     ${XOPTOOLKIT_DIR}")
else()
  message(STATUS "  XOPTOOLKIT used:     <unknown>")
endif()
message(STATUS "==============================================")
