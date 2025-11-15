# src/IgorXOP/CMakeLists.txt
# ------------------------------------------------------------------------------
# Igor XOP subproject (macOS x64 and Windows x64)
#
# Responsibilities:
#   - build the XOP module (MODULE on macOS, SHARED on Windows)
#   - assemble .xop bundle on macOS via cmake/assemble_xop.cmake
#   - stage the bundle / dll into a post-build install root (default: <build>/install)
#   - export the XOP target name and an alias so the parent CMake can refer to it
#
# Expected top-level behavior (caller / parent CMake):
#   - Define platform macros before including this subdir: PLATFORM_APPLE or PLATFORM_WIN64
#   - Preferably define&create `pylabhub::corelib` and `XOP::XOPSupport` targets before adding this subdir,
#     so this subproject can link to them at configure time and acquire include dirs.
#   - Optionally pass DXOP_INSTALL_ROOT to override post-build staging root.
#
# Exports to parent:
#   - Cache var: XOP_TARGET_NAME  (name of the cmake target created here)
#   - Alias: pylabhub::pylabhubxop -> <actual target>
# ------------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.18)
project(IgorXOP LANGUAGES C CXX)

# -------------------------
# Option to enable/disable building XOP from parent
# -------------------------
if(NOT DEFINED BUILD_XOP)
  option(BUILD_XOP "Attempt to build Igor XOP addon" ON)
endif()

if(NOT BUILD_XOP)
  message(STATUS "IgorXOP: BUILD_XOP=OFF; skipping XOP configuration.")
  return()
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

# Make local cmake helpers visible (FindXOPSupport.cmake, assemble_xop.cmake)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

# -------------------------
# Publishable internal state (parent can query)
# -------------------------
set(BUILD_XOP_FINAL_ENABLED OFF CACHE INTERNAL "Final: IgorXOP will be built")
set(XOP_TARGET_NAME "" CACHE INTERNAL "Final: IgorXOP target name")
set(XOP_SUPPORT_FOUND OFF CACHE INTERNAL "XOPSupport found (vendor or system)")

# -------------------------
# Basic expectations from parent / environment
# -------------------------
# Parent should set one of PLATFORM_APPLE or PLATFORM_WIN64 (preferred). Attempt minimal detection otherwise.
if(NOT (DEFINED PLATFORM_APPLE OR DEFINED PLATFORM_WIN64))
  if(APPLE)
    set(PLATFORM_APPLE TRUE CACHE BOOL "Auto-detected macOS platform" FORCE)
    message(STATUS "IgorXOP: auto-detected PLATFORM_APPLE")
  elseif(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(PLATFORM_WIN64 TRUE CACHE BOOL "Auto-detected Windows x64 platform" FORCE)
    message(STATUS "IgorXOP: auto-detected PLATFORM_WIN64 (64-bit Windows)")
  else()
    message(FATAL_ERROR "IgorXOP: platform not set or unsupported. Parent must set PLATFORM_APPLE or PLATFORM_WIN64.")
  endif()
endif()

# Enforce supported platforms only
if(NOT (DEFINED PLATFORM_APPLE OR DEFINED PLATFORM_WIN64))
  message(FATAL_ERROR "IgorXOP: only PLATFORM_APPLE and PLATFORM_WIN64 are supported.")
endif()

# -------------------------
# Canonical naming
# -------------------------
set(XOP_NAME "pylabhubxop" CACHE STRING "Logical XOP base name")
set(XOP_BUNDLE_NAME "${XOP_NAME}64" CACHE STRING "Final bundle name (e.g. pylabhubxop64)")
set(XOP_TARGET "${XOP_NAME}" CACHE STRING "CMake target name for XOP")

# default post-build staging root (virtual root) - parent may override
if(NOT DEFINED XOP_INSTALL_ROOT)
  set(XOP_INSTALL_ROOT "${CMAKE_BINARY_DIR}/install" CACHE PATH "Post-build staging root for XOP artifacts (default: <build>/install)")
endif()

message(STATUS "IgorXOP: XOP_NAME = ${XOP_NAME}, XOP_BUNDLE_NAME = ${XOP_BUNDLE_NAME}")
message(STATUS "IgorXOP: post-build staging root = ${XOP_INSTALL_ROOT}")

# -------------------------
# Sources
# -------------------------
set(XOP_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/WaveAccess.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/WaveAccess.h
  ${CMAKE_CURRENT_SOURCE_DIR}/resource.h
)

# platform resources
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
# Create the build target (MODULE for mac, SHARED for win)
# -------------------------
if(DEFINED PLATFORM_APPLE)
  add_library(${XOP_TARGET} MODULE ${XOP_SOURCES})
  add_library(pylabhub::${XOP_TARGET} ALIAS ${XOP_TARGET})   # allow internal referencing by canonical name
  # Ensure no lib prefix and canonical output name includes "64"
  set_target_properties(${XOP_TARGET} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "${XOP_BUNDLE_NAME}"
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_BUNDLE_NAME "${XOP_BUNDLE_NAME}"
    MACOSX_BUNDLE_EXECUTABLE "${XOP_BUNDLE_NAME}"
  )
elseif(DEFINED PLATFORM_WIN64)
  add_library(${XOP_TARGET} SHARED ${XOP_SOURCES})
  add_library(pylabhub::${XOP_TARGET} ALIAS ${XOP_TARGET})
  set_target_properties(${XOP_TARGET} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "${XOP_BUNDLE_NAME}"
  )
endif()

# Also expose the canonical alias name requested by parent: pylabhub::pylabhubxop
# (some parents may prefer this canonical namespaced alias)
if(NOT TARGET pylabhub::pylabhubxop)
  add_library(pylabhub::pylabhubxop ALIAS ${XOP_TARGET})
endif()

# -------------------------
# Standard compile/link flags
# -------------------------
target_compile_features(${XOP_TARGET} PRIVATE cxx_std_17)

if(DEFINED PLATFORM_APPLE)
  target_compile_definitions(${XOP_TARGET} PRIVATE MACIGOR IGOR64)
  message(STATUS "IgorXOP: adding compile definitions for macOS: MACIGOR IGOR64")
endif()
if(DEFINED PLATFORM_WIN64)
  target_compile_definitions(${XOP_TARGET} PRIVATE WINIGOR IGOR64)
  message(STATUS "IgorXOP: adding compile definitions for Windows: WINIGOR IGOR64")
endif()

# -------------------------
# Consume top-level provided helper/core targets if available
# Parent is expected to create pylabhub::corelib and XOP::XOPSupport (recommended).
# If present, link to them so include dirs and other interface properties propagate.
# -------------------------
if(TARGET pylabhub::corelib)
  target_link_libraries(${XOP_TARGET} PRIVATE pylabhub::corelib)
  message(STATUS "IgorXOP: linked to pylabhub::corelib")
endif()

if(TARGET XOP::XOPSupport)
  # XOP::XOPSupport should be an IMPORTED interface target (FindXOPSupport or parent provides it)
  target_link_libraries(${XOP_TARGET} PRIVATE XOP::XOPSupport)
  message(STATUS "IgorXOP: linked to XOP::XOPSupport")
endif()

# -------------------------
# Export/include handling for compile (fallbacks)
# -------------------------
# If imported targets are not present, attempt to use cache vars that parent may set:
if(NOT TARGET XOP::XOPSupport AND DEFINED XOP_SUPPORT_ROOT_USED AND EXISTS "${XOP_SUPPORT_ROOT_USED}/include")
  target_include_directories(${XOP_TARGET} PRIVATE "${XOP_SUPPORT_ROOT_USED}/include")
  message(STATUS "IgorXOP: using fallback include dir ${XOP_SUPPORT_ROOT_USED}/include")
endif()

# -------------------------
# Linker/export flags
# -------------------------
set(_exports_exp "${CMAKE_CURRENT_SOURCE_DIR}/Exports.exp")
set(_exports_def "${CMAKE_CURRENT_SOURCE_DIR}/Exports.def")

if(DEFINED PLATFORM_APPLE)
  if(EXISTS "${_exports_exp}")
    target_link_options(${XOP_TARGET} PRIVATE
      $<$<CONFIG:Debug>:-Wl,-exported_symbols_list,${_exports_exp}>
      $<$<NOT:$<CONFIG:Debug>>:-Wl,-exported_symbols_list,${_exports_exp}>
    )
    message(STATUS "IgorXOP: using exported symbols file ${_exports_exp}")
  else()
    target_link_options(${XOP_TARGET} PRIVATE "-Wl,-exported_symbol,_XOPMain")
    message(STATUS "IgorXOP: Exports.exp not found; explicitly exporting _XOPMain")
  endif()

  # frameworks commonly required by XOPSupport
  target_link_libraries(${XOP_TARGET} PRIVATE
    "-framework Cocoa"
    "-framework CoreFoundation"
    "-framework CoreServices"
    "-framework Carbon"
    "-framework AudioToolbox"
  )
endif()

if(DEFINED PLATFORM_WIN64)
  if(EXISTS "${_exports_def}")
    target_link_options(${XOP_TARGET} PRIVATE "/DEF:${_exports_def}")
    message(STATUS "IgorXOP: using Exports.def for Windows exports")
  else()
    message(VERBOSE "IgorXOP: Exports.def not found; ensure XOPMain is exported in Windows build")
  endif()

  if(MSVC)
    target_link_options(${XOP_TARGET} PRIVATE /LARGEADDRESSAWARE)
  endif()
  target_link_libraries(${XOP_TARGET} PRIVATE version)
endif()

# -------------------------
# Prepare r_include_dirs_list (semicol-separated) to pass to assemble script.
# Preference order:
#   1) INTERFACE_INCLUDE_DIRECTORIES of XOP::XOPSupport (if target exists)
#   2) INTERFACE_INCLUDE_DIRECTORIES of pylabhub::corelib (if exists)
#   3) value of cache var XOP_SUPPORT_INCLUDE_DIR (fallback from parent)
# -------------------------
set(_r_include_dirs_list "")

if(TARGET XOP::XOPSupport)
  get_target_property(_tmp_inc XOP::XOPSupport INTERFACE_INCLUDE_DIRECTORIES)
  if(NOT _tmp_inc STREQUAL "INTERFACE_INCLUDE_DIRECTORIES-NOTFOUND" AND NOT _tmp_inc STREQUAL "NOTFOUND")
    set(_r_include_dirs_list "${_tmp_inc}")
  endif()
endif()

if(_r_include_dirs_list STREQUAL "" AND TARGET pylabhub::corelib)
  get_target_property(_tmp_inc2 pylabhub::corelib INTERFACE_INCLUDE_DIRECTORIES)
  if(NOT _tmp_inc2 STREQUAL "INTERFACE_INCLUDE_DIRECTORIES-NOTFOUND" AND NOT _tmp_inc2 STREQUAL "NOTFOUND")
    set(_r_include_dirs_list "${_tmp_inc2}")
  endif()
endif()

if(_r_include_dirs_list STREQUAL "" AND DEFINED XOP_SUPPORT_INCLUDE_DIR)
  set(_r_include_dirs_list "${XOP_SUPPORT_INCLUDE_DIR}")
endif()

# Log for visibility
if(NOT "${_r_include_dirs_list}" STREQUAL "")
  message(STATUS "IgorXOP: will pass Rez include dirs to assemble script: ${_r_include_dirs_list}")
else()
  message(STATUS "IgorXOP: no Rez include dirs discovered (Rez may fail if .r includes toolkit headers)")
endif()

# -------------------------
# Assembler script invocation (macOS) or staging (Windows).
# We create a single custom-command that produces a single marker file to keep it
# robust across generators. The final assemble marker target is added as a dependency.
# -------------------------
set(_assemble_script "${CMAKE_CURRENT_LIST_DIR}/cmake/assemble_xop.cmake")
set(_bundle_marker "${CMAKE_BINARY_DIR}/src/IgorXOP/${XOP_BUNDLE_NAME}.assembled")
set(_bundle_dir "${CMAKE_BINARY_DIR}/src/IgorXOP/${XOP_BUNDLE_NAME}.xop")

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
              -D_r_include_dirs="${_r_include_dirs_list}"
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

    # include marker target in default build (so building default will stage bundle too)
    add_dependencies(${XOP_TARGET}_bundle assemble_xop_marker_${XOP_TARGET})
    set_target_properties(assemble_xop_marker_${XOP_TARGET} PROPERTIES EXCLUDE_FROM_ALL FALSE)

    message(STATUS "IgorXOP: assemble_xop marker target registered; assemble script: ${_assemble_script}")
  else()
    message(WARNING "IgorXOP: assemble_xop.cmake not found; bundle assembly will be skipped")
  endif()

  # install rule for 'make install' - keeps module bundling semantics
  install(TARGETS ${XOP_TARGET}
    BUNDLE DESTINATION xop
    LIBRARY DESTINATION xop
    INCLUDES DESTINATION include
  )
endif()

if(DEFINED PLATFORM_WIN64)
  # Stage DLL into XOP_INSTALL_ROOT/xop/ and rename to .xop (so Igor can find it).
  set(_win_marker "${CMAKE_BINARY_DIR}/src/IgorXOP/${XOP_BUNDLE_NAME}.dll.staged")
  add_custom_command(
    OUTPUT "${_win_marker}"
    BYPRODUCTS "${_win_marker}"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMAND ${CMAKE_COMMAND} -E echo "IgorXOP: staging ${XOP_BUNDLE_NAME}.dll -> ${XOP_INSTALL_ROOT}/xop/${XOP_BUNDLE_NAME}.xop"
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

  # Install rule for Windows: runtime goes to xop/ so `make install` will place the dll
  install(TARGETS ${XOP_TARGET}
    RUNTIME DESTINATION xop
    LIBRARY DESTINATION xop
    INCLUDES DESTINATION include
  )
endif()

# -------------------------
# Install helper files (.ihf/.ipf) into <install_root>/xop/ if present
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
# Publish final state to top-level (cache vars & messages)
# -------------------------
set(BUILD_XOP_FINAL_ENABLED ON CACHE INTERNAL "Final: IgorXOP will be built")
set(XOP_TARGET_NAME "${XOP_TARGET}" CACHE INTERNAL "Final: IgorXOP target name")
set(XOP_TARGET_EXECUTABLE "${XOP_BUNDLE_NAME}" CACHE INTERNAL "Final: XOP executable name (inside bundle)")

message(STATUS "IgorXOP: configured XOP target '${XOP_TARGET}'.")
message(STATUS "  XOP build target:    ${XOP_TARGET}")
message(STATUS "  XOP bundle name:     ${XOP_BUNDLE_NAME}")
message(STATUS "  Post-build staging:  ${XOP_INSTALL_ROOT}")
message(STATUS "==============================================")
