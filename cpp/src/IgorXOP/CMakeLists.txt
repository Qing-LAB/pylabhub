# src/IgorXOP/CMakeLists.txt
# ------------------------------------------------------------------------------
# Igor XOP subproject (macOS x64 and Windows x64)
#
# Responsibilities:
#   - build the XOP module (MODULE on macOS, MODULE on Windows)
#   - assemble .xop bundle on macOS via cmake/assemble_xop.cmake
#   - stage the bundle / dll into a post-build staging root (default: <build>/stage)
#   - export the XOP target name and an alias so the parent CMake can refer to it
#
# Expected top-level behavior (caller / parent CMake):
#   - Define platform macros before including this subdir: PLATFORM_APPLE or PLATFORM_WIN64
#   - Preferably define&create `pylabhub::basic` and `XOP::XOPSupport` targets before adding this subdir,
#     so this subproject can link to them at configure time and acquire include dirs.

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
# Parent is required to set one of PLATFORM_APPLE or PLATFORM_WIN64.
# A FATAL_ERROR is triggered if neither is defined.
if(NOT (DEFINED PLATFORM_APPLE OR DEFINED PLATFORM_WIN64))
  message(FATAL_ERROR "IgorXOP: Only PLATFORM_APPLE or PLATFORM_WIN64 is supported.")
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
# Create the build target (MODULE for mac, MODULE for win)
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
    CXX_VISIBILITY_PRESET "default" # Ensure symbols are exported by default for this target
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

if(PYLABHUB_SANITIZER_FLAGS_SET)
  message(STATUS "** Sanitizer detected, adding to ${XOP_TARGET} with compilation flags.")
  target_compile_options(${XOP_TARGET} PRIVATE $<TARGET_PROPERTY:pylabhub::sanitizer_flags,INTERFACE_COMPILE_OPTIONS>)
  # If sanitizer is enabled, and a runtime path is found, explicitly link to it.
  # This is necessary when a shared library's objects are compiled with sanitizer
  # instrumentation, as the shared library itself needs to resolve these symbols.
  if(PYLABHUB_SANITIZER_RUNTIME_PATH)
    if(PYLABHUB_SANITIZER_RUNTIME_TYPE STREQUAL "shared")
      message(STATUS "** The sanitizer is a dynamic lib, we will link ${XOP_TARGET} to the runtime: ${PYLABHUB_SANITIZER_RUNTIME_PATH}")
      target_link_libraries(${XOP_TARGET} PRIVATE pylabhub::sanitizer_flags)
    else()
      if(PYLABHUB_SANITIZER_RUNTIME_TYPE STREQUAL "static" AND PYLABHUB_SANITIZER_LINK_WITH_DYNAMIC_LIB)
        message(STATUS "** The sanitizer is a static lib, with PYLABHUB_SANITIZER_LINK_WITH_DYNAMIC_LIB=ON, we will force link with ${XOP_TARGET} to: ${PYLABHUB_SANITIZER_RUNTIME_PATH}")
        target_link_libraries(${XOP_TARGET} PRIVATE pylabhub::sanitizer_flags)
      else()
        message(STATUS "** The sanitizer is not a dynamic lib, we will not link by default to ${XOP_TARGET}. Please make sure your final executive that uses pylabhub-utils will be linked to the sanitizer lib: ${PYLABHUB_SANITIZER_RUNTIME_PATH}")
        message(STATUS "")
      endif()
    endif()
  endif()
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
  # --- macOS Post-Build: Bundle Assembly, Processing, and Verification ---
  # On macOS, a post-build command is used to assemble the .xop bundle.
  # This is handled by a single, consolidated script that performs all necessary
  # steps: creating the bundle structure, compiling resources, cleaning
  # attributes, code signing, and verification.
  # --- macOS Post-Build: Bundle Assembly, Processing, and Verification ---
  # On macOS, a post-build command is used to assemble the .xop bundle.
  # This is handled by a single, consolidated script that performs all necessary
  # steps: creating the bundle structure, compiling resources, cleaning
  # attributes, code signing, and verification.

  set(BUNDLE_DIR "${CMAKE_CURRENT_BINARY_DIR}/${XOP_BUNDLE_NAME}.xop")
  set(MACOSX_CODESIGN_IDENTITY "-" CACHE STRING "The identity to use for code signing the macOS bundle.")
  if(MACOSX_CODESIGN_IDENTITY)
    message(STATUS "IgorXOP: macOS code signing is ENABLED with identity: ${MACOSX_CODESIGN_IDENTITY}")
  endif()

  # Determine if there is a dependency dylib to bundle.
  set(XOP_DEPENDENCY_DYLIB_PATH "")
  if(TARGET pylabhub::utils)
      get_target_property(_utils_type pylabhub::utils TYPE)
      if(_utils_type STREQUAL "SHARED_LIBRARY" OR _utils_type STREQUAL "MODULE_LIBRARY")
          set(XOP_DEPENDENCY_DYLIB_PATH "$<TARGET_FILE:pylabhub::utils>")
          message(STATUS "IgorXOP: Found dependency dylib to bundle: pylabhub::utils")
      endif()
  endif()

  # Add the post-build command to run the consolidated script.
  # We pass all variables via -D to the script. The path to the compiled
  # binary is passed using a generator expression ($<TARGET_FILE:...>) which
  # is resolved at build time.
  add_custom_command(
    TARGET ${XOP_TARGET}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND}
      -DMACHO_BINARY_PATH=$<TARGET_FILE:${XOP_TARGET}>
      -DDEPENDENCY_DYLIB_PATH=${XOP_DEPENDENCY_DYLIB_PATH}
      -DBUNDLE_DIR=${BUNDLE_DIR}
      -DXOP_BUNDLE_NAME=${XOP_BUNDLE_NAME}
      -DCONFIGURED_INFO_PLIST=${CONFIGURED_INFO_PLIST}
      -DINFO_PLIST_STRINGS_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/InfoPlist.strings
      -DREZ_SOURCE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/WaveAccess.r
      -DR_INCLUDE_DIRS_LIST="${_r_include_dirs_list}"
      -DMACOSX_CODESIGN_IDENTITY=${MACOSX_CODESIGN_IDENTITY}
      -P "${CMAKE_CURRENT_LIST_DIR}/cmake/consolidated_xop_assembly.cmake.in"
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
  # Also copy the pylabhub::utils dependency if it is a shared library
  if(TARGET pylabhub::utils)
    get_target_property(_utils_type pylabhub::utils TYPE)
    if(_utils_type STREQUAL "SHARED_LIBRARY" OR _utils_type STREQUAL "MODULE_LIBRARY")
      list(APPEND stage_commands
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:pylabhub::utils>" "${PYLABHUB_STAGING_DIR}/IgorXOP/"
      )
      message(STATUS "IgorXOP: Adding command to stage dependent library: pylabhub::utils")
    endif()
  endif()
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