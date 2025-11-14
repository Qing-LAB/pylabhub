# src/IgorXOP/CMakeLists.txt
# ------------------------------------------------------------------------------
# Build configuration for the Igor XOP plugin (Windows x64 and macOS x64)
#
# This file is intended to be included from the top-level CMake project.
# It expects (but does not strictly require) the top-level to set/declare
# certain platform selection macros (e.g. PLATFORM_APPLE or PLATFORM_WIN64).
#
# Expected variables / knobs (should be provided by top-level or via -D):
#   BUILD_XOP            - option (BOOL) to enable this subproject (default ON)
#   PLATFORM_APPLE       - if building for macOS (set by top-level platform logic)
#   PLATFORM_WIN64       - if building for Windows x64 (set by top-level)
#   XOP_VENDOR_DIR       - optional vendor root containing XOPSupport
#   USE_SYSTEM_XOPSUPPORT- optional system root to prefer for XOPSupport
#   XOP_INSTALL_ROOT     - optional post-build staging root (default: <CMAKE_BINARY_DIR>/install)
#
# Notes:
# - On macOS we build a MODULE to avoid CMake adding a .so/.dylib suffix we
#   don't want inside the final .xop bundle. The assembly script will create
#   the <name>.xop directory with Contents/{MacOS,Resources,Info.plist}.
# - On Windows we build a SHARED library (.dll). We stage the .dll into the
#   post-build install root under <install_root>/xop/, with the new suffix ".xop".
#
# - This file will add the CMake targets:
#     <XOP_TARGET>                (the compiled module/shared lib)
#     assemble_xop_marker_<XOP_TARGET>  (marker produced by assemble script)
#     assemble_xop_<XOP_TARGET>         (convenience assemble target)
#     <XOP_TARGET>_bundle               (alias bundle target)
#
# - The assemble step is intentionally done by a CMake script (cmake/assemble_xop.cmake)
#   to keep behavior consistent across generators (Xcode, Ninja, Make).
# ------------------------------------------------------------------------------

cmake_minimum_required(VERSION 3.18)
project(IgorXOP LANGUAGES C CXX)

# -------------------------
# Public option - allow top-level to disable XOP build
# -------------------------
if(NOT DEFINED BUILD_XOP)
  option(BUILD_XOP "Attempt to build Igor XOP addon (only supported on Windows x64 and macOS x64)" ON)
endif()

if(NOT BUILD_XOP)
  message(STATUS "IgorXOP: BUILD_XOP=OFF; skipping XOP configuration.")
  return()
endif()

# -------------------------
# Make local cmake helpers visible (FindXOPSupport.cmake, assemble_xop.cmake)
# -------------------------
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

# -------------------------
# Publishable internal state (so top-level can query)
# -------------------------
set(BUILD_XOP_FINAL_ENABLED OFF CACHE INTERNAL "Final: IgorXOP will be built")
set(XOP_TARGET_NAME "" CACHE INTERNAL "Final: IgorXOP target name")
set(XOP_SUPPORT_FOUND OFF CACHE INTERNAL "XOPSupport found (vendor or system)")

# -------------------------
# Top-level platform expectation:
# The project selects exactly one of PLATFORM_APPLE or PLATFORM_WIN64.
# If that has not been done by the top-level we attempt minimal detection,
# but we prefer the top-level to set these macros explicitly.
# -------------------------
if(NOT (DEFINED PLATFORM_APPLE OR DEFINED PLATFORM_WIN64))
  # try best-effort detection, but do not auto-guess in ambiguous cases
  if(APPLE)
    set(PLATFORM_APPLE TRUE CACHE BOOL "Detected macOS platform" FORCE)
    message(STATUS "IgorXOP: auto-detected PLATFORM_APPLE (APPLE CMake variable)")
  elseif(WIN32)
    # verify 64-bit
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
      set(PLATFORM_WIN64 TRUE CACHE BOOL "Detected Windows x64 platform" FORCE)
      message(STATUS "IgorXOP: auto-detected PLATFORM_WIN64 (WIN32 + 64-bit)")
    else()
      message(FATAL_ERROR "IgorXOP: detected Windows 32-bit target. This subproject supports Windows x64 only.")
    endif()
  else()
    message(FATAL_ERROR "IgorXOP: platform not specified (PLATFORM_APPLE or PLATFORM_WIN64). Top-level CMake should set one of these.")
  endif()
endif()

# -------------------------
# Enforce supported platforms
# -------------------------
if(NOT (DEFINED PLATFORM_APPLE OR DEFINED PLATFORM_WIN64))
  message(FATAL_ERROR "IgorXOP: Unsupported platform; only PLATFORM_APPLE and PLATFORM_WIN64 are supported.")
endif()

# -------------------------
# Basic project naming and canonical variables
# -------------------------
# XOP logical base name and canonical bundle name (64-bit variant)
set(XOP_NAME "pylabhubxop" CACHE STRING "Logical XOP name (base)")
set(XOP_TARGET "${XOP_NAME}" CACHE STRING "CMake target name for XOP")
set(XOP_BUNDLE_NAME "${XOP_NAME}64" CACHE STRING "Final bundle name (e.g. pylabhubxop64)")

# Source list (keeps the minimal list; platform files appended below)
set(XOP_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/WaveAccess.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/WaveAccess.h
  ${CMAKE_CURRENT_SOURCE_DIR}/resource.h
)

# Add platform-specific resource sources if available
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
# Find XOPSupport helper (optional file in cmake/)
# This sets up XOP::XOPSupport and XOP::IGOR imported targets when found.
# -------------------------
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/cmake/FindXOPSupport.cmake")
  include(cmake/FindXOPSupport.cmake OPTIONAL)
endif()

if(NOT COMMAND find_xopsupport)
  message(WARNING "IgorXOP: find_xopsupport() not available (FindXOPSupport.cmake not provided). The XOPSupport include/lib may not be configured.")
endif()

# Try to locate XOPSupport (if find_xopsupport is available)
if(COMMAND find_xopsupport)
  find_xopsupport(_xop_found)
  if(_xop_found)
    set(XOP_SUPPORT_FOUND ON CACHE INTERNAL "XOPSupport found (vendor or system)")
    message(STATUS "IgorXOP: XOPSupport detected and will be used.")
  else()
    set(XOP_SUPPORT_FOUND OFF CACHE INTERNAL "XOPSupport found (vendor or system)")
    message(WARNING "IgorXOP: XOPSupport not found; compile may fail if headers/libs are missing.")
  endif()
endif()

# -------------------------
# Default post-build staging root (post-build "virtual root")
# This is intentionally a staging area inside the build tree by default:
#   <CMAKE_BINARY_DIR>/install
# The top-level may override by passing -DXOP_INSTALL_ROOT=...
# -------------------------
if(NOT DEFINED XOP_INSTALL_ROOT)
  set(XOP_INSTALL_ROOT "${CMAKE_BINARY_DIR}/install" CACHE PATH "Post-build staging (virtual root) for XOP artifacts (default: <build>/install)")
endif()
message(STATUS "IgorXOP: post-build staging root = ${XOP_INSTALL_ROOT}")

# -------------------------
# Create target with platform-appropriate type
# -------------------------
if(DEFINED PLATFORM_APPLE)
  # macOS: build as MODULE so the linker output is a plain module (no .xop suffix)
  add_library(${XOP_TARGET} MODULE ${XOP_SOURCES})
  add_library(pylabhub::${XOP_TARGET} ALIAS ${XOP_TARGET})

  # canonical output name: include 64 suffix in the filename
  set_target_properties(${XOP_TARGET} PROPERTIES
    PREFIX ""                                 # no "lib" prefix
    OUTPUT_NAME "${XOP_BUNDLE_NAME}"          # e.g. pylabhubxop64
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_BUNDLE_NAME "${XOP_BUNDLE_NAME}"
    MACOSX_BUNDLE_EXECUTABLE "${XOP_BUNDLE_NAME}"
  )
elseif(DEFINED PLATFORM_WIN64)
  # Windows: build as shared library (DLL)
  add_library(${XOP_TARGET} SHARED ${XOP_SOURCES})
  add_library(pylabhub::${XOP_TARGET} ALIAS ${XOP_TARGET})

  # Keep consistent name, include 64 suffix
  set_target_properties(${XOP_TARGET} PROPERTIES
    PREFIX ""                                 # avoid lib prefix
    OUTPUT_NAME "${XOP_BUNDLE_NAME}"          # e.g. pylabhubxop64 => yields pylabhubxop64.dll
  )
endif()

# -------------------------
# Common compile options & language standard
# -------------------------
target_compile_features(${XOP_TARGET} PRIVATE cxx_std_17)

# Define platform-specific compile definitions for XOP API
if(DEFINED PLATFORM_APPLE)
  target_compile_definitions(${XOP_TARGET} PRIVATE MACIGOR IGOR64)
  message(STATUS "IgorXOP: adding compile definitions for macOS: MACIGOR IGOR64")
endif()
if(DEFINED PLATFORM_WIN64)
  target_compile_definitions(${XOP_TARGET} PRIVATE WINIGOR IGOR64)
  message(STATUS "IgorXOP: adding compile definitions for Windows: WINIGOR IGOR64")
endif()

# -------------------------
# Include directories: prefer imported XOP::XOPSupport
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

# -------------------------
# Link to XOPSupport library (prefer imported target)
# -------------------------
if(TARGET XOP::XOPSupport)
  target_link_libraries(${XOP_TARGET} PRIVATE XOP::XOPSupport)
  message(STATUS "IgorXOP: linking to XOP::XOPSupport imported target")
else()
  if(DEFINED XOP_SUPPORT_LIB_PATH AND EXISTS "${XOP_SUPPORT_LIB_PATH}")
    target_link_libraries(${XOP_TARGET} PRIVATE "${XOP_SUPPORT_LIB_PATH}")
    message(STATUS "IgorXOP: linking directly to ${XOP_SUPPORT_LIB_PATH}")
  endif()
endif()

# Optional IGOR import lib (Windows)
if(TARGET XOP::IGOR)
  target_link_libraries(${XOP_TARGET} PRIVATE XOP::IGOR)
  message(STATUS "IgorXOP: linking to XOP::IGOR imported target")
elseif(DEFINED XOP_SUPPORT_IGOR_LIB_PATH AND EXISTS "${XOP_SUPPORT_IGOR_LIB_PATH}")
  target_link_libraries(${XOP_TARGET} PRIVATE "${XOP_SUPPORT_IGOR_LIB_PATH}")
  message(STATUS "IgorXOP: linking to IGOR import lib ${XOP_SUPPORT_IGOR_LIB_PATH}")
endif()

# -------------------------
# Linker/export flags (platform specifics)
# -------------------------
# macOS: prefer Exports.exp (used with -Wl,-exported_symbols_list)
set(_exports_exp "${CMAKE_CURRENT_SOURCE_DIR}/Exports.exp")
if(DEFINED PLATFORM_APPLE)
  if(EXISTS "${_exports_exp}")
    target_link_options(${XOP_TARGET} PRIVATE
      $<$<CONFIG:Debug>:-Wl,-exported_symbols_list,${_exports_exp}>
      $<$<NOT:$<CONFIG:Debug>>:-Wl,-exported_symbols_list,${_exports_exp}>
    )
    message(STATUS "IgorXOP: using exported symbols file ${_exports_exp}")
  else()
    # fallback: export _XOPMain explicitly
    target_link_options(${XOP_TARGET} PRIVATE "-Wl,-exported_symbol,_XOPMain")
    message(STATUS "IgorXOP: Exports.exp not found; explicitly exporting _XOPMain")
  endif()

  # Add standard frameworks used by XOPSupport / XOP API
  target_link_libraries(${XOP_TARGET} PRIVATE
    "-framework Cocoa"
    "-framework CoreFoundation"
    "-framework CoreServices"
    "-framework Carbon"
    "-framework AudioToolbox"
  )

  # rpath settings for loader-relative usage inside bundle
  set_target_properties(${XOP_TARGET} PROPERTIES
    BUILD_RPATH "@loader_path/../Frameworks"
    INSTALL_RPATH "@loader_path/../Frameworks"
    BUILD_WITH_INSTALL_RPATH TRUE
  )
endif()

# Windows: use .def file if present to control exports; else expect user to provide
if(DEFINED PLATFORM_WIN64)
  set(_exports_def "${CMAKE_CURRENT_SOURCE_DIR}/Exports.def")
  if(EXISTS "${_exports_def}")
    target_link_options(${XOP_TARGET} PRIVATE "/DEF:${_exports_def}")
    message(STATUS "IgorXOP: using module definition ${_exports_def} to export XOPMain (Windows)")
  else()
    message(VERBOSE "IgorXOP: Exports.def not found; ensure XOPMain is exported (Windows).")
  endif()

  # linker option to make DLL large address aware
  if(MSVC)
    target_link_options(${XOP_TARGET} PRIVATE /LARGEADDRESSAWARE)
  endif()
endif()

# -------------------------
# Windows system libs (if needed)
# -------------------------
if(DEFINED PLATFORM_WIN64)
  target_link_libraries(${XOP_TARGET} PRIVATE version)
endif()

# -------------------------
# Install/packaging behavior
# - We always register an install rule for the compiled artifact.
# - Additionally we add a post-build assemble/staging step:
#     - macOS: run cmake/assemble_xop.cmake to create <XOP_BUNDLE_NAME>.xop under build tree
#              and copy it into ${XOP_INSTALL_ROOT}/xop/
#     - Windows: copy the generated .dll into ${XOP_INSTALL_ROOT}/xop/
#
# The assemble process is implemented as a single custom-command that produces
# a single marker file. A custom target depends on that marker so different
# generators handle dependencies correctly.
# -------------------------
set(_assemble_script "${CMAKE_CURRENT_LIST_DIR}/cmake/assemble_xop.cmake")
set(_bundle_marker "${CMAKE_BINARY_DIR}/src/IgorXOP/${XOP_BUNDLE_NAME}.assembled")
set(_bundle_dir "${CMAKE_BINARY_DIR}/src/IgorXOP/${XOP_BUNDLE_NAME}.xop")  # intended mac bundle destination

if(DEFINED PLATFORM_APPLE)
  if(EXISTS "${_assemble_script}")
    add_custom_command(
      OUTPUT "${_bundle_marker}"
      BYPRODUCTS "${_bundle_marker}"
      WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
      COMMAND ${CMAKE_COMMAND} -E echo "IgorXOP: packaging ${XOP_BUNDLE_NAME}.xop from module $<TARGET_FILE:${XOP_TARGET}> (config: $<CONFIG>)"
      COMMAND ${CMAKE_COMMAND}
              -D_mach_o="$<TARGET_FILE:${XOP_TARGET}>"
              -D_bundle_dir="${_bundle_dir}"
              -D_xop_name="${XOP_BUNDLE_NAME}"
              -D_build_root="${CMAKE_BINARY_DIR}"
              -D_config="$<CONFIG>"
              -D_configured_plist="${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
              -D_r_file="${CMAKE_CURRENT_SOURCE_DIR}/WaveAccess.r"
              -D_rsrc_basename="${XOP_BUNDLE_NAME}.rsrc"
              -D_install_root="${XOP_INSTALL_ROOT}"
              -P "${_assemble_script}"
      COMMAND ${CMAKE_COMMAND} -E touch "${_bundle_marker}"
      DEPENDS ${XOP_TARGET}
      VERBATIM
    )

    add_custom_target(assemble_xop_marker_${XOP_TARGET} DEPENDS "${_bundle_marker}")
    add_custom_target(assemble_xop_${XOP_TARGET} COMMENT "Assemble XOP bundle for ${XOP_TARGET}")
    add_dependencies(assemble_xop_${XOP_TARGET} assemble_xop_marker_${XOP_TARGET})

    add_custom_target(${XOP_TARGET}_bundle COMMENT "Bundle target for ${XOP_TARGET}")
    add_dependencies(${XOP_TARGET}_bundle assemble_xop_marker_${XOP_TARGET})

    # make bundle target part of the default build so 'cmake --build' will assemble by default
    add_dependencies(${XOP_TARGET}_bundle assemble_xop_marker_${XOP_TARGET})
    set_target_properties(assemble_xop_marker_${XOP_TARGET} PROPERTIES EXCLUDE_FROM_ALL FALSE)

    message(STATUS "IgorXOP: registered assemble_xop marker target; assemble script: ${_assemble_script}")
  else()
    message(WARNING "IgorXOP: assemble_xop.cmake not found; bundle assembly will be skipped")
  endif()

  # Install rules for mac: install the module BUNDLE into xop/ (so 'make install' works)
  install(TARGETS ${XOP_TARGET}
    BUNDLE DESTINATION xop
    LIBRARY DESTINATION xop
    INCLUDES DESTINATION include
  )
elseif(DEFINED PLATFORM_WIN64)
  # Windows packaging: simple post-build staging copy to ${XOP_INSTALL_ROOT}/xop/
  # Build a custom-command that will copy the resulting DLL into the staging area.
  get_target_property(_target_file ${XOP_TARGET} LOCATION)
  set(_win_marker "${CMAKE_BINARY_DIR}/src/IgorXOP/${XOP_BUNDLE_NAME}.dll.staged")

  add_custom_command(
    OUTPUT "${_win_marker}"
    BYPRODUCTS "${_win_marker}"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMAND ${CMAKE_COMMAND} -E echo "IgorXOP: staging ${XOP_BUNDLE_NAME}.dll to ${XOP_INSTALL_ROOT}/xop/${XOP_BUNDLE_NAME}.xop"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${XOP_INSTALL_ROOT}/xop"
    COMMAND ${CMAKE_COMMAND} -E remove -f "${XOP_INSTALL_ROOT}/xop/${XOP_BUNDLE_NAME}.xop"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${XOP_TARGET}>" "${XOP_INSTALL_ROOT}/xop/${XOP_BUNDLE_NAME}.xop"
    COMMAND ${CMAKE_COMMAND} -E touch "${_win_marker}"
    DEPENDS ${XOP_TARGET}
    VERBATIM
  )

  add_custom_target(assemble_xop_marker_${XOP_TARGET} DEPENDS "${_win_marker}")
  add_custom_target(assemble_xop_${XOP_TARGET} COMMENT "Stage XOP DLL for ${XOP_TARGET}")
  add_dependencies(assemble_xop_${XOP_TARGET} assemble_xop_marker_${XOP_TARGET})

  add_custom_target(${XOP_TARGET}_bundle COMMENT "Bundle (stage) target for ${XOP_TARGET}")
  add_dependencies(${XOP_TARGET}_bundle assemble_xop_marker_${XOP_TARGET})

  add_dependencies(${XOP_TARGET}_bundle assemble_xop_marker_${XOP_TARGET})

  # Install rule for Windows: install the runtime/dll under xop/
  install(TARGETS ${XOP_TARGET}
    RUNTIME DESTINATION xop
    LIBRARY DESTINATION xop
    INCLUDES DESTINATION include
  )
endif()

# -------------------------
# Install helper files (.ihf/.ipf) into <install_root>/xop/ if present in source dir
# (these should not be embedded inside the .xop bundle)
# -------------------------
file(GLOB _xop_ihf_files RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/*.ihf")
file(GLOB _xop_ipf_files RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/*.ipf")
set(_xop_package_files "")
list(APPEND _xop_package_files ${_xop_ihf_files} ${_xop_ipf_files})

if(_xop_package_files)
  foreach(_rel IN LISTS _xop_package_files)
    set(_src "${CMAKE_CURRENT_SOURCE_DIR}/${_rel}")
    if(EXISTS "${_src}")
      install(FILES "${_src}" DESTINATION xop)
      message(STATUS "IgorXOP: scheduling install of ${_rel} -> <install-root>/xop/")
    else()
      message(WARNING "IgorXOP: expected package file ${_src} not found at install scheduling time")
    endif()
  endforeach()
endif()

# -------------------------
# Final state publishing and summary
# -------------------------
set(BUILD_XOP_FINAL_ENABLED ON CACHE INTERNAL "Final: IgorXOP will be built")
set(XOP_TARGET_NAME "${XOP_TARGET}" CACHE INTERNAL "Final: IgorXOP target name")

message(STATUS "IgorXOP: configured XOP target '${XOP_TARGET}'.")
message(STATUS "  XOP build target:    ${XOP_TARGET}")
message(STATUS "  XOP bundle name:     ${XOP_BUNDLE_NAME}")
message(STATUS "  Post-build staging:  ${XOP_INSTALL_ROOT}")
message(STATUS "==============================================")
