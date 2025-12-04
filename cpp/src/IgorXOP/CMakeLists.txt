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

# Make local cmake helpers visible (FindXOPSupport.cmake, assemble_xop.cmake)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

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
set(XOP_TARGET "pylabhubxop") # The logical name for the CMake target.
set(XOP_BUNDLE_NAME "${XOP_TARGET}64") # The final on-disk name, e.g., pylabhubxop64.xop

# The post-build staging root is now inherited from the top-level project.
# We will use PYLABHUB_STAGING_DIR, which is expected to be defined.
if(NOT DEFINED PYLABHUB_STAGING_DIR)
  message(FATAL_ERROR "IgorXOP expects PYLABHUB_STAGING_DIR to be defined by the parent project.")
endif()
message(STATUS "IgorXOP: Target Name = ${XOP_TARGET}, Bundle Name = ${XOP_BUNDLE_NAME}")
message(STATUS "IgorXOP: Staging to sub-directory in: ${PYLABHUB_STAGING_DIR}")

# -------------------------
# Sources
# -------------------------
set(XOP_SOURCES
  WaveAccess.cpp
  WaveAccess.h
  resource.h
)

# Add platform-specific resource files based on the top-level PLATFORM_* variables.
# This is the correct pattern for this project, ensuring top-down control.
if(DEFINED PLATFORM_APPLE)
  set(_rfile "${CMAKE_CURRENT_SOURCE_DIR}/WaveAccess.r")
  if(EXISTS "${_rfile}")
    list(APPEND XOP_SOURCES "${_rfile}")
    message(STATUS "IgorXOP: adding macOS resource ${_rfile}")
  endif()
elseif(DEFINED PLATFORM_WIN64)
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
  # Ensure no lib prefix and canonical output name includes "64"
  set_target_properties(${XOP_TARGET} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "${XOP_BUNDLE_NAME}" # e.g., pylabhubxop64
    MACOSX_BUNDLE TRUE # This is the key property to create a .xop bundle structure.
  )
elseif(DEFINED PLATFORM_WIN64)
  add_library(${XOP_TARGET} SHARED ${XOP_SOURCES})
  set_target_properties(${XOP_TARGET} PROPERTIES
    PREFIX ""
    OUTPUT_NAME "${XOP_BUNDLE_NAME}"
    SUFFIX ".xop"
  )
endif()

# Expose a stable, namespaced alias for other components in this project to use.
add_library(pylabhub::${XOP_TARGET} ALIAS ${XOP_TARGET})

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
if(NOT TARGET pylabhub::third_party::XOPToolKit)
  message(FATAL_ERROR "IgorXOP: The pylabhub::third_party::XOPToolKit target was not created. Ensure the third_party directory is configured first.")
endif()

target_link_libraries(${XOP_TARGET} PRIVATE pylabhub::third_party::XOPToolKit)
message(STATUS "IgorXOP: linked to pylabhub::third_party::XOPToolKit")

if(TARGET pylabhub::corelib)
  target_link_libraries(${XOP_TARGET} PRIVATE pylabhub::corelib) # Use the namespaced alias
  message(STATUS "IgorXOP: linked to pylabhub::corelib")
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
endif()

if(DEFINED PLATFORM_WIN64)
  if(EXISTS "${_exports_def}")
    target_link_options(${XOP_TARGET} PRIVATE "/DEF:${_exports_def}")
    message(STATUS "IgorXOP: using Exports.def for Windows exports")
  endif()
endif()

# -------------------------
# Prepare r_include_dirs_list (semicol-separated) to pass to assemble script.
# Preference order:
#   1) INTERFACE_INCLUDE_DIRECTORIES of XOP::XOPSupport (if target exists)
# -------------------------
set(_r_include_dirs_list "")
get_target_property(_r_include_dirs_list pylabhub::third_party::XOPToolKit INTERFACE_INCLUDE_DIRECTORIES)
if(NOT _r_include_dirs_list)
  # Fallback in case the property is empty or not found
  set(_r_include_dirs_list "")
endif()

# Log for visibility
if(NOT "${_r_include_dirs_list}" STREQUAL "")
  message(STATUS "IgorXOP: will pass Rez include dirs to assemble script: ${_r_include_dirs_list}")
  message(STATUS "IgorXOP: Passing Rez include dirs to bundle assembly script: ${_r_include_dirs_list}")
else()
  message(STATUS "IgorXOP: no Rez include dirs discovered (Rez may fail if .r includes toolkit headers)")
endif()

# -------------------------
# Assembler script invocation (macOS) or staging (Windows).
# We create a single custom-command that produces a single marker file to keep it
# robust across generators. The final assemble marker target is added as a dependency.
# -------------------------
set(_assemble_script "${CMAKE_CURRENT_LIST_DIR}/cmake/assemble_xop.cmake")
set(_configured_assemble_script "${CMAKE_CURRENT_BINARY_DIR}/assemble_xop.cmake")
set(_assemble_config_template "${CMAKE_CURRENT_LIST_DIR}/cmake/assemble_xop.config.cmake.in")
set(_assemble_config_file_abs "${CMAKE_CURRENT_BINARY_DIR}/assemble_xop.config.cmake")
set(_bundle_marker "${CMAKE_BINARY_DIR}/src/IgorXOP/${XOP_BUNDLE_NAME}.assembled")
set(_bundle_dir "${CMAKE_BINARY_DIR}/src/IgorXOP/${XOP_BUNDLE_NAME}.xop")

if(DEFINED PLATFORM_APPLE)
  # Add a post-build step to automatically assemble the .xop bundle
  if(EXISTS "${_assemble_script}")
    # 1. Configure a file to pass variables to the assembly script.
    configure_file("${_assemble_config_template}" "${_assemble_config_file_abs}" @ONLY)

    # 2. Configure the assembly script to know the absolute path to its config.
    configure_file("${_assemble_script}" "${_configured_assemble_script}" @ONLY)

    add_custom_command(
      TARGET ${XOP_TARGET}
      POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E echo "IgorXOP: Assembling ${XOP_BUNDLE_NAME}.xop bundle..."
      COMMAND ${CMAKE_COMMAND} -P "${_configured_assemble_script}"
      COMMENT "Assembling ${XOP_BUNDLE_NAME}.xop bundle"
      VERBATIM
    )
    message(STATUS "IgorXOP: Registered automatic assembly for ${XOP_TARGET}")
  else()
    message(WARNING "IgorXOP: assemble_xop.cmake not found; bundle assembly will be skipped")
  endif()

endif()

if(DEFINED PLATFORM_WIN64)
  # Add a post-build step to stage the .xop file automatically
  add_custom_command(
    TARGET ${XOP_TARGET}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PYLABHUB_STAGING_DIR}/IgorXOP"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${XOP_TARGET}>" "${PYLABHUB_STAGING_DIR}/IgorXOP/"
    COMMENT "Staging ${XOP_BUNDLE_NAME}.xop to ${PYLABHUB_STAGING_DIR}/IgorXOP/"
    VERBATIM
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
  # Stage the helper files alongside the XOP bundle automatically.
  add_custom_command(
    TARGET ${XOP_TARGET}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/${_xop_package_files}"
      "${PYLABHUB_STAGING_DIR}/IgorXOP/"
    COMMENT "Staging IgorXOP helper files (.ipf, .ihf) to ${PYLABHUB_STAGING_DIR}/IgorXOP/"
    VERBATIM
  )
endif()

# The installation of helper files depends on the master install switch.
# The staging of the bundle itself is unconditional.
if(PYLABHUB_CREATE_INSTALL_TARGET)
  foreach(_rel IN LISTS _xop_package_files)
    set(_src "${CMAKE_CURRENT_SOURCE_DIR}/${_rel}")
    if(EXISTS "${_src}") # This install() is for the final installation step.
      install(FILES "${_src}" DESTINATION "IgorXOP")
      message(STATUS "IgorXOP: Scheduling install of ${_rel} -> <install-root>/IgorXOP/")
    else()
      message(WARNING "IgorXOP: Expected package file ${_src} not found at install scheduling time")
    endif()
  endforeach()
endif() # PYLABHUB_CREATE_INSTALL_TARGET

# -------------------------
# Publish final state to top-level (cache vars & messages)
# -------------------------
message(STATUS "IgorXOP: configured XOP target '${XOP_TARGET}'.")
message(STATUS "  XOP build target:    ${XOP_TARGET}")
message(STATUS "  XOP bundle name:     ${XOP_BUNDLE_NAME}")
message(STATUS "==============================================")
