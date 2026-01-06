# src/IgorXOP/CMakeLists.txt
# ------------------------------------------------------------------------------
# Igor XOP subproject (macOS x64 and Windows x64)
#
# Responsibilities:
#   - build the XOP module (MODULE on macOS, SHARED on Windows)
#   - assemble .xop bundle on macOS via cmake/assemble_xop.cmake
#   - stage the bundle / dll into a post-build staging root (default: <build>/stage)
#   - export the XOP target name and an alias so the parent CMake can refer to it
#
# Expected top-level behavior (caller / parent CMake):
#   - Define platform macros before including this subdir: PLATFORM_APPLE or PLATFORM_WIN64
#   - Preferably define&create `pylabhub::basic` and `XOP::XOPSupport` targets before adding this subdir,
#     so this subproject can link to them at configure time and acquire include dirs.
#   - Optionally pass DXOP_INSTALL_ROOT to override post-build staging root.
#
# Exports to parent:
#   - Cache var: XOP_TARGET_NAME  (name of the cmake target created here)
#   - Alias: pylabhub::pylabhubxop -> <actual target>
# ------------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.29)
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
  message(FATAL_ERROR "IgorXOP: Only PLATFORM_APPLE or PLATFORM_WIN64 is supported.")
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
  # On macOS, we build a standard MODULE library. The bundling is handled by a post-build script.
  add_library(${XOP_TARGET} MODULE ${XOP_SOURCES})
  # Ensure no lib prefix and canonical output name includes "64"

  # Configure Info.plist from a template
  set(INFO_PLIST_TEMPLATE "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in")
  set(CONFIGURED_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist")
  configure_file("${INFO_PLIST_TEMPLATE}" "${CONFIGURED_INFO_PLIST}" @ONLY)

  set_target_properties(${XOP_TARGET} PROPERTIES
    PREFIX "" # No 'lib' prefix for a bundle executable
    OUTPUT_NAME "${XOP_BUNDLE_NAME}" # e.g., pylabhubxop64
    SUFFIX "" # No suffix for a bundle executable
    # Prevent CMake from adding an RPATH to the build tree, which is not suitable for a distributable bundle.
    # A bundle executable should be self-contained or use @rpath to find frameworks within the bundle itself.
    # By setting these, we ensure the binary does not have hardcoded paths to the build directory,
    # which would break it on any other machine.
    BUILD_WITH_INSTALL_RPATH FALSE
    INSTALL_RPATH ""
  )
elseif(DEFINED PLATFORM_WIN64)
  # On Windows, XOPs are standard DLLs. Using MODULE accurately describes
  # this target as a runtime-loadable plugin (like a DLL), rather than
  # a library intended for static linkage within the build system.
  # This choice resolves LNK4070 by better informing the linker about the
  # nature of the output artifact.
  add_library(${XOP_TARGET} MODULE ${XOP_SOURCES})
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
target_compile_features(${XOP_TARGET} PRIVATE cxx_std_20)

# Check if we are using MSVC
if(MSVC)
  # Enable synchronous exception handling and disable warning C5105.
  target_compile_options(${XOP_TARGET} PRIVATE /EHsc /wd5105)

  # Enable the standards-conforming preprocessor
  target_compile_options(${XOP_TARGET} PRIVATE /Zc:preprocessor)
endif()

# The following compile definitions are provided by XOPStandardHeaders.h and were causing macro redefinition warnings.
# They are commented out to avoid redundancy and rely on the toolkit's intended mechanism for platform detection.
# In case of future issues with XOPStandardHeaders.h changes, these can be uncommented for testing.
#if(DEFINED PLATFORM_APPLE)
#  target_compile_definitions(${XOP_TARGET} PRIVATE MACIGOR IGOR64)
#  message(STATUS "IgorXOP: adding compile definitions for macOS: MACIGOR IGOR64")
#endif()
#if(DEFINED PLATFORM_WIN64)
#  target_compile_definitions(${XOP_TARGET} PRIVATE WINIGOR IGOR64)
#  message(STATUS "IgorXOP: adding compile definitions for Windows: WINIGOR IGOR64")
#endif()

# -------------------------
# Consume top-level provided helper/core targets if available
# Parent is expected to create pylabhub::basic and XOP::XOPSupport (recommended).
# If present, link to them so include dirs and other interface properties propagate.
# -------------------------
if(NOT TARGET pylabhub::third_party::XOPToolKit)
  message(FATAL_ERROR "IgorXOP: The pylabhub::third_party::XOPToolKit target was not created. Ensure the third_party directory is configured first.")
endif()

target_link_libraries(${XOP_TARGET} PRIVATE pylabhub::third_party::XOPToolKit)
message(STATUS "IgorXOP: linked to pylabhub::third_party::XOPToolKit")

if(TARGET pylabhub::utils)
  target_link_libraries(${XOP_TARGET} PRIVATE pylabhub::utils) # Use the namespaced alias
  message(STATUS "IgorXOP: linked to pylabhub::utils")
endif()

if(PYLABHUB_SANITIZER_FLAG)
  target_link_libraries(${XOP_TARGET} PRIVATE pylabhub::sanitizer_flags)
endif()

# -------------------------
# Linker/export flags
# -------------------------
set(_exports_exp "${CMAKE_CURRENT_SOURCE_DIR}/Exports.exp")
set(_exports_def "${CMAKE_CURRENT_SOURCE_DIR}/Exports.def")

if(DEFINED PLATFORM_APPLE)
  if(EXISTS "${_exports_exp}")
    target_link_options(${XOP_TARGET} PRIVATE "-Wl,-exported_symbols_list,${_exports_exp}")
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
#   1) C++ include directories from the XOPToolKit target.
#   2) Rez-specific include directories from the XOPToolKit target's custom property.
# -------------------------
set(_r_include_dirs_list "")

# Get the Rez-specific include directories from the custom property.
get_target_property(_rez_includes pylabhub::third_party::XOPToolKit INTERFACE_REZ_INCLUDE_DIRECTORIES)

if(_rez_includes)
  list(APPEND _r_include_dirs_list ${_rez_includes})
endif()

# Log for visibility
if(NOT "${_r_include_dirs_list}" STREQUAL "")
  message(STATUS "IgorXOP: Passing Rez include dirs to bundle assembly script: ${_r_include_dirs_list}")
else()
  message(STATUS "IgorXOP: no Rez include dirs discovered (Rez may fail if .r includes toolkit headers)")
endif()

# -------------------------
# Staging the final bundle/dll.
#
# The philosophy is to separate ASSEMBLY from STAGING.
# - Assembly happens as a POST_BUILD step on the main target (e.g., creating the .xop bundle).
# - Staging happens via a separate custom target, `stage_${XOP_TARGET}`, which can be
#   invoked explicitly after the build is complete.
# -------------------------

# Find optional helper files to be staged.
file(GLOB _xop_help_files "${CMAKE_CURRENT_SOURCE_DIR}/*.ihf")
file(GLOB _xop_proc_files "${CMAKE_CURRENT_SOURCE_DIR}/*.ipf")
set(_xop_helper_files "")

if(_xop_help_files)
    list(APPEND _xop_helper_files ${_xop_help_files})
    message(STATUS "IgorXOP: Found Igor Help File(s) (.ihf) to be staged: ${_xop_help_files}")
endif()
if(_xop_proc_files)
    list(APPEND _xop_helper_files ${_xop_proc_files})
    message(STATUS "IgorXOP: Found Igor Procedure File(s) (.ipf) to be staged: ${_xop_proc_files}")
endif()

if(NOT _xop_help_files AND NOT _xop_proc_files)
    message(STATUS "IgorXOP: No optional Igor Help File(s) (.ihf) or Procedure File(s) (.ipf) were found.")
endif()

# ==============================================================================
# macOS: Assembly (POST_BUILD) and Staging (custom target)
# ==============================================================================
if(DEFINED PLATFORM_APPLE)
  # --- Overview of the macOS Post-Build Process ---
  # The goal is to create a standard macOS .xop bundle. This is done via a
  # POST_BUILD command on the main library target.
  #   1. Compile the C++ code into a single Mach-O binary.
  #   2. After the build, a bootstrap script (`run_assemble_xop.cmake`) is executed.
  #   3. This script first runs an assembly script to create the .xop bundle
  #      directory structure in the build tree.
  #   4. It then runs a processing script to clean, sign, and verify the bundle in-place.
  #   5. A separate `stage_pylabhubxop` target copies the final, verified bundle
  #      to the global staging directory.

  # --- The Bootstrap Mechanism for Running Post-Build Scripts ---
  # A challenge in CMake is that the exact path to a built target (the Mach-O binary)
  # is only known at build time. We solve this using a two-stage "bootstrap" process:
  #
  # 1. `run_assemble_xop.cmake.in` (The Bootstrap Script):
  #    At configure time, we create this script from a template. It is designed
  #    to receive the target's path as a command-line argument.
  #
  # 2. `add_custom_command(POST_BUILD ...)`:
  #    After the `pylabhubxop` target is built, this command executes the
  #    bootstrap script, passing the binary's path using a "generator expression"
  #    (`$<TARGET_FILE:...>`), which CMake resolves at build time. The script then
  #    handles the rest of the post-build steps.
  set(BOOTSTRAP_SCRIPT_TEMPLATE "${CMAKE_CURRENT_LIST_DIR}/cmake/run_assemble_xop.cmake.in")
  set(CONFIGURED_BOOTSTRAP_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/run_assemble_xop.cmake")

  set(ASSEMBLE_SCRIPT_TEMPLATE "${CMAKE_CURRENT_LIST_DIR}/cmake/assemble_xop.cmake.in")
  set(CONFIGURED_ASSEMBLE_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/assemble_xop.cmake")

  set(BUNDLE_DIR "${CMAKE_CURRENT_BINARY_DIR}/${XOP_BUNDLE_NAME}.xop")
  set(REZ_SOURCE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/WaveAccess.r")
  set(INFO_PLIST_STRINGS_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/InfoPlist.strings")
  set(R_INCLUDE_DIRS_LIST "${_r_include_dirs_list}")

  # Use "-" for an ad-hoc signature, which is sufficient for local execution and testing.
  set(MACOSX_CODESIGN_IDENTITY "-" CACHE STRING "The identity to use for code signing the macOS bundle.")
  if(MACOSX_CODESIGN_IDENTITY)
    message(STATUS "IgorXOP: macOS code signing is ENABLED with identity: ${MACOSX_CODESIGN_IDENTITY}")
  endif()

  set(PROCESS_BUNDLE_SCRIPT_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake/process_bundle.cmake")
  set(CODESIGN_SCRIPT_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake/CodeSign.cmake")
  set(VERIFY_SCRIPT_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake/VerifyXOP.cmake")
  
  configure_file("${BOOTSTRAP_SCRIPT_TEMPLATE}" "${CONFIGURED_BOOTSTRAP_SCRIPT}" @ONLY)

  # --- Step 1: Assembly, Processing, and Verification (POST_BUILD) ---
  # After the binary is built, run the bootstrap script to assemble, clean,
  # sign, and verify the .xop bundle in the build directory.
  add_custom_command(
    TARGET ${XOP_TARGET}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND}
      -DMACHO_BINARY_PATH_ARG=$<TARGET_FILE:${XOP_TARGET}>
      -P "${CONFIGURED_BOOTSTRAP_SCRIPT}"
    COMMENT "Assembling, processing, and verifying ${XOP_BUNDLE_NAME}.xop"
    VERBATIM
  )
endif() # End of PLATFORM_APPLE specific assembly

# ==============================================================================
# Staging (Custom Target for both macOS and Windows)
# ==============================================================================
# This section defines a single, unified `stage_pylabhubxop` target that handles
# copying the final artifacts for both platforms. This removes redundancy while
# clearly separating the platform-specific logic.

set(stage_commands "")

# Add the platform-specific command for the main artifact.
if(DEFINED PLATFORM_APPLE)
  # For macOS, copy the entire assembled .xop bundle directory.
  list(APPEND stage_commands
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${BUNDLE_DIR}" "${PYLABHUB_STAGING_DIR}/IgorXOP/${XOP_BUNDLE_NAME}.xop"
  )
elseif(DEFINED PLATFORM_WIN64)
  # For Windows, copy the final .xop DLL.
  list(APPEND stage_commands
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${XOP_TARGET}>" "${PYLABHUB_STAGING_DIR}/IgorXOP/"
  )
endif()

# Conditionally add the command to copy helper files.
# This is critical to prevent a build error if `_xop_helper_files` is empty,
# as `copy_if_different` requires at least one source file.
if(_xop_helper_files)
  list(APPEND stage_commands
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_xop_helper_files}" "${PYLABHUB_STAGING_DIR}/IgorXOP/"
  )
endif()

# Create the unified staging target for this component.
add_custom_target(stage_${XOP_TARGET}
  ${stage_commands}
  COMMENT "Staging final ${XOP_BUNDLE_NAME}.xop and helper files to ${PYLABHUB_STAGING_DIR}/IgorXOP/"
  VERBATIM
)
add_dependencies(stage_${XOP_TARGET} ${XOP_TARGET} create_staging_dirs)

# -------------------------
# Publish final state to top-level (cache vars & messages)
# -------------------------
message(STATUS "IgorXOP: configured XOP target '${XOP_TARGET}'.")
message(STATUS "  XOP build target:    ${XOP_TARGET}")
message(STATUS "  XOP bundle name:     ${XOP_BUNDLE_NAME}")
message(STATUS "  XOP staging target:  stage_${XOP_TARGET}")
message(STATUS "==============================================")