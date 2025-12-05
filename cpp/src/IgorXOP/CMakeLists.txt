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
# -------------------------

# ==============================================================================
# macOS BUNDLE ASSEMBLY AND STAGING (PLATFORM_APPLE)
# ==============================================================================
if(DEFINED PLATFORM_APPLE)
  # --- Overview of the macOS Post-Build Process ---
  # The goal is to create a standard macOS .xop bundle, which is a directory
  # with a specific internal structure. This cannot be done with a simple
  # `add_library` command. We use a sequence of post-build commands to:
  #   1. Compile the C++ code into a single Mach-O binary.
  #   2. Run a CMake script (`assemble_xop.cmake.in`) to build the .xop directory
  #      structure around this binary.
  #   3. Stage the complete bundle and any helper files to the project's
  #      staging directory (`build/stage/`).
  #   4. Prepare the bundle for distribution by cleaning metadata and signing it.

  # --- The Bootstrap Mechanism for Running the Assembly Script ---
  # A challenge in CMake is that the exact path to a built target (the Mach-O binary)
  # is only known at build time, not at configure time. We solve this using a
  # two-stage "bootstrap" process:
  #
  # 1. `run_assemble_xop.cmake.in` (The Bootstrap Script):
  #    At configure time, we create a script from this template. It is designed
  #    to receive the target's path as a command-line argument.
  #
  # 2. `assemble_xop.cmake.in` (The Main Assembly Script):
  #    This script does the actual work of creating the bundle. The bootstrap
  #    script configures this script with the now-known path to the binary and
  #    then executes it.
  #
  # 3. `add_custom_command(POST_BUILD ...)`:
  #    This is where the magic happens. After the `pylabhubxop` target is built,
  #    this command executes the bootstrap script, passing the binary's path
  #    using a "generator expression" (`$<TARGET_FILE:...>`), which CMake
  #    resolves right before running the command.

  # 3. The bootstrap script then uses `configure_file` to create the *final*
  #    assembly script, passing in the now-resolved paths.
  # 4. Finally, the bootstrap script executes the final assembly script.

  set(BOOTSTRAP_SCRIPT_TEMPLATE "${CMAKE_CURRENT_LIST_DIR}/cmake/run_assemble_xop.cmake.in")
  set(CONFIGURED_BOOTSTRAP_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/run_assemble_xop.cmake")

  # Define paths for the *actual* assembly script. These will be passed into the bootstrap script.
  set(ASSEMBLE_SCRIPT_TEMPLATE "${CMAKE_CURRENT_LIST_DIR}/cmake/assemble_xop.cmake.in")
  set(CONFIGURED_ASSEMBLE_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/assemble_xop.cmake")

  # Define variables that will be substituted into the assembly script template.
  # These are known at configure time.
  set(BUNDLE_DIR "${CMAKE_CURRENT_BINARY_DIR}/${XOP_BUNDLE_NAME}.xop")
  set(REZ_SOURCE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/WaveAccess.r") # Passed to assemble_xop.cmake.in
  set(INFO_PLIST_STRINGS_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/InfoPlist.strings") # Passed to assemble_xop.cmake.in
  set(R_INCLUDE_DIRS_LIST "${_r_include_dirs_list}") # Passed to assemble_xop.cmake.in
  # CONFIGURED_INFO_PLIST is set earlier and will be passed to assemble_xop.cmake.in
  
  # At configure time, create the bootstrap script.
  # The @ONLY flag ensures that only @VAR@ variables are replaced, not ${VAR} variables,
  # which is crucial for passing generator expressions.
  configure_file("${BOOTSTRAP_SCRIPT_TEMPLATE}" "${CONFIGURED_BOOTSTRAP_SCRIPT}" @ONLY)

  # Find helper files to be staged alongside the bundle.
  file(GLOB _xop_helper_files
      "${CMAKE_CURRENT_SOURCE_DIR}/*.ihf"
      "${CMAKE_CURRENT_SOURCE_DIR}/*.ipf"
  )

  # Use "-" for an ad-hoc signature, which is sufficient for local execution and testing.
  # For distribution, this should be a proper Developer ID.
  set(MACOSX_CODESIGN_IDENTITY "-" CACHE STRING "The identity to use for code signing the macOS bundle.")

  # Add a single, sequential post-build command to assemble, stage, clean, and sign.
  add_custom_command(
    TARGET ${XOP_TARGET}
    POST_BUILD

    # Step 1: ASSEMBLE the bundle in the build directory.
    # This invokes the bootstrap script, which in turn runs the main assembly script.
    # The generator expression `$<TARGET_FILE:${XOP_TARGET}>` is resolved to the
    # concrete path of the compiled binary just before this command is run.
    COMMAND ${CMAKE_COMMAND}
      -DMACHO_BINARY_PATH_ARG=$<TARGET_FILE:${XOP_TARGET}>
      -P "${CONFIGURED_BOOTSTRAP_SCRIPT}"

    # Step 2: STAGE the assembled bundle.
    # This copies the entire `pylabhubxop64.xop` directory into the global staging area.
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PYLABHUB_STAGING_DIR}/IgorXOP"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${BUNDLE_DIR}" "${PYLABHUB_STAGING_DIR}/IgorXOP/${XOP_BUNDLE_NAME}.xop"

    # Step 3: STAGE helper files (.ipf, .ihf) alongside the bundle.
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_xop_helper_files}" "${PYLABHUB_STAGING_DIR}/IgorXOP/"

    # Step 4: CLEAN the bundle for distribution.
    # `xattr -cr` removes extended attributes (like quarantine flags) that macOS
    # adds to downloaded files. These attributes can interfere with code signing
    # and cause Gatekeeper issues, so cleaning them is a critical step.
    COMMAND xattr -cr "${PYLABHUB_STAGING_DIR}/IgorXOP/${XOP_BUNDLE_NAME}.xop"

    # Step 5: SIGN the bundle.
    COMMAND ${CMAKE_COMMAND}
      -D "BUNDLE_PATH=${PYLABHUB_STAGING_DIR}/IgorXOP/${XOP_BUNDLE_NAME}.xop"
      -D "SIGNING_IDENTITY=${MACOSX_CODESIGN_IDENTITY}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CodeSign.cmake"

    # Step 6: VERIFY the final staged bundle.
    COMMAND ${CMAKE_COMMAND}
      -D "BUNDLE_TO_VERIFY=${PYLABHUB_STAGING_DIR}/IgorXOP/${XOP_BUNDLE_NAME}.xop"
      -D "EXECUTABLE_NAME=${XOP_BUNDLE_NAME}"
      -P "${CMAKE_CURRENT_LIST_DIR}/cmake/VerifyXOP.cmake"
    COMMENT "Assembling, staging, signing, and verifying ${XOP_BUNDLE_NAME}.xop and helper files"
    VERBATIM
  )
  if(MACOSX_CODESIGN_IDENTITY)
    message(STATUS "IgorXOP: macOS code signing is ENABLED with identity: ${MACOSX_CODESIGN_IDENTITY}")
  endif()
endif()

# ==============================================================================
# Windows STAGING (PLATFORM_WIN64)
# ==============================================================================
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
# NOTE: For macOS, staging of helper files is integrated into the main post-build
# sequence to guarantee correct execution order. For Windows, we add a separate
# command here. The final `install` step is handled by the top-level CMakeLists.txt.
if(DEFINED PLATFORM_WIN64)
  # Find helper files if they haven't been found already.
  if(NOT _xop_helper_files)
    file(GLOB _xop_helper_files
        "${CMAKE_CURRENT_SOURCE_DIR}/*.ihf"
        "${CMAKE_CURRENT_SOURCE_DIR}/*.ipf"
    )
  endif()

  if(_xop_helper_files)
    add_custom_command(TARGET ${XOP_TARGET} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_xop_helper_files}" "${PYLABHUB_STAGING_DIR}/IgorXOP/"
      COMMENT "Staging IgorXOP helper files to ${PYLABHUB_STAGING_DIR}/IgorXOP/"
    )
  endif()
endif()

# -------------------------
# Publish final state to top-level (cache vars & messages)
# -------------------------
message(STATUS "IgorXOP: configured XOP target '${XOP_TARGET}'.")
message(STATUS "  XOP build target:    ${XOP_TARGET}")
message(STATUS "  XOP bundle name:     ${XOP_BUNDLE_NAME}")
message(STATUS "==============================================")