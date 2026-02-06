# ---------------------------------------------------------------------------
# cmake/StageHelpers.cmake
#
# Purpose: Provides a consistent API for staging project artifacts.
#
# This module defines a set of functions that abstract the process of copying
# build artifacts (headers, libraries, executables) into the unified staging
# directory (`${PYLABHUB_STAGING_DIR}`). This ensures that all components of
# the project follow the same conventions for creating a runnable, testable
# layout.
#
# This script provides the following functions:
#
# - pylabhub_register_headers_for_staging(...)
#   Registers a directory of header files to be staged.
#
# - pylabhub_stage_executable(...)
#   Sets the output directory for an executable to stage it directly.
#
# - pylabhub_get_library_staging_commands(...)
#   Generates a list of commands to stage a library's files. It correctly
#   handles platform differences and is used by the centralized staging logic.
#
# - pylabhub_register_library_for_staging(...)
#   Registers a library target to a global property, marking it for staging.
#
# - pylabhub_register_test_for_staging(...)
#   Registers a test executable for staging and inclusion in the test aggregator target.
#
# - pylabhub_attach_headers_staging_commands(...)
#   Attaches custom commands to a target to stage header directories.
#
# These functions rely on variables like PYLABHUB_STAGING_DIR and custom targets
# being defined in the top-level CMakeLists.txt.
# ---------------------------------------------------------------------------
#
cmake_minimum_required(VERSION 3.29)

# - pylabhub_register_headers_for_staging ---
#
# Registers a request to stage header files from a given directory.
# This function serializes the arguments into a string and appends it to the
# PYLABHUB_HEADERS_TO_STAGE global property list. A centralized loop later
# processes this property to generate the staging commands.
#
# For a detailed explanation of how `DIRECTORIES`, `FILES`, and `SUBDIR`
# interact to control the resulting include path structure, refer to:
# `docs/README_CMake_Design.md#111-header-staging-mechanics`
#
function(pylabhub_register_headers_for_staging)
  set(options "")
  set(oneValueArgs "SUBDIR;ATTACH_TO;EXTERNAL_PROJECT_DEPENDENCY")
  set(multiValueArgs "DIRECTORIES;FILES")
  cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  # Serialize the arguments into a single list.
  set(REGISTRATION_LIST "")
  if(ARG_DIRECTORIES)
    list(APPEND REGISTRATION_LIST "DIRECTORIES;${ARG_DIRECTORIES}")
  endif()
  if(ARG_FILES)
    list(APPEND REGISTRATION_LIST "FILES;${ARG_FILES}")
  endif()
  if(ARG_SUBDIR)
    list(APPEND REGISTRATION_LIST "SUBDIR;${ARG_SUBDIR}")
  endif()
  if(ARG_ATTACH_TO)
    list(APPEND REGISTRATION_LIST "ATTACH_TO;${ARG_ATTACH_TO}")
  endif()
  if(ARG_EXTERNAL_PROJECT_DEPENDENCY)
    list(APPEND REGISTRATION_LIST "EXTERNAL_PROJECT_DEPENDENCY;${ARG_EXTERNAL_PROJECT_DEPENDENCY}")
  endif()

  # Serialize the list into a string with a unique separator and append it
  # to a global property that holds a LIST of these registration strings.
  string(REPLACE ";" "@@" REGISTRATION_STRING "${REGISTRATION_LIST}")
  set_property(GLOBAL APPEND PROPERTY PYLABHUB_HEADERS_TO_STAGE "${REGISTRATION_STRING}")
endfunction()

# --- pylabhub_stage_executable ---
#
# Sets the output directory for an executable target to place it directly
# into the staging area. This is the preferred modern CMake approach.
#
# Usage:
#   pylabhub_stage_executable(
#     TARGET <target_name>          # The executable target to stage.
#     DESTINATION <subdir>          # The subdirectory within `${PYLABHUB_STAGING_DIR}` (e.g., 'bin').
#   )
#
function(pylabhub_stage_executable)
  set(options "")
  set(oneValueArgs "TARGET;DESTINATION")
  set(multiValueArgs "")
  cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if(NOT ARG_TARGET OR NOT ARG_DESTINATION)
    message(FATAL_ERROR "pylabhub_stage_executable requires TARGET and DESTINATION arguments.")
  endif()
  if(NOT TARGET ${ARG_TARGET})
    message(FATAL_ERROR "pylabhub_stage_executable: Target '${ARG_TARGET}' does not exist.")
  endif()

  set(STAGED_DEST_DIR "${PYLABHUB_STAGING_DIR}/${ARG_DESTINATION}")

  # For multi-config generators (e.g., Visual Studio, Xcode), we must set the
  # output directory property for each configuration type. For single-config
  # generators, setting the base property is sufficient.
  if(CMAKE_CONFIGURATION_TYPES)
    set(output_dir_props "")
    foreach(config ${CMAKE_CONFIGURATION_TYPES})
      string(TOUPPER ${config} config_upper)
      list(APPEND output_dir_props RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${STAGED_DEST_DIR}")
      if(MSVC)
        list(APPEND output_dir_props PDB_OUTPUT_DIRECTORY_${config_upper} "${STAGED_DEST_DIR}")
      endif()
    endforeach()
    set_target_properties(${ARG_TARGET} PROPERTIES ${output_dir_props})
  else()
    set_target_properties(${ARG_TARGET} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${STAGED_DEST_DIR}"
    )
    if(MSVC)
       set_target_properties(${ARG_TARGET} PROPERTIES
        PDB_OUTPUT_DIRECTORY "${STAGED_DEST_DIR}"
      )
    endif()
  endif()

endfunction()


# --- pylabhub_get_library_staging_commands ---
#
# Generates a list of CMake COMMANDs needed to stage a project library.
# This function does not execute the commands itself; it returns them in an
# output variable, making it suitable for use with `add_custom_target`.
#
# It correctly handles platform-specific library types:
#   - Runtime artifacts (.dll, .so, .dylib) are staged to the `DESTINATION` subdir.
#   - Link-time artifacts (.lib, .a) are always staged to the `lib/` subdir.
#
# Usage:
#   pylabhub_get_library_staging_commands(
#     TARGET <target_name>          # The library target to stage.
#     DESTINATION <subdir>          # The subdirectory within `${PYLABHUB_STAGING_DIR}` for RUNTIME artifacts (e.g., 'bin').
#     OUT_COMMANDS <var_name>       # The variable to store the generated list of commands in.
#   )
#
function(pylabhub_get_library_staging_commands)
  cmake_parse_arguments(ARG "" "TARGET;DESTINATION;OUT_COMMANDS;ABSOLUTE_DIR" "" ${ARGN})

  if(NOT ARG_TARGET OR NOT ARG_DESTINATION OR NOT ARG_OUT_COMMANDS)
    message(FATAL_ERROR "pylabhub_get_library_staging_commands requires TARGET, "
                        "DESTINATION, and OUT_COMMANDS arguments.")
  endif()

  if(NOT TARGET ${ARG_TARGET})
    message(FATAL_ERROR "pylabhub_get_library_staging_commands: Target '${ARG_TARGET}' does not exist.")
  endif()

  # handle ABSOLUTE_DIR safely (keep calling convention the same)
  if(ARG_ABSOLUTE_DIR)
    set(RUNTIME_DEST_DIR "${ARG_DESTINATION}")
    set(LINKTIME_DEST_DIR "${ARG_DESTINATION}")
  else()
    set(RUNTIME_DEST_DIR "${PYLABHUB_STAGING_DIR}/${ARG_DESTINATION}")
    set(LINKTIME_DEST_DIR "${PYLABHUB_STAGING_DIR}/lib")
  endif()
  
  get_target_property(TGT_TYPE ${ARG_TARGET} TYPE)
  # detect if the target is imported (handle imported targets robustly while preserving UNKNOWN_LIBRARY branch)
  get_target_property(_is_imported ${ARG_TARGET} IMPORTED)

  set(commands_list "")

  if(_is_imported)
    # Imported target: prefer IMPORTED_LOCATION, fall back to per-config IMPORTED_LOCATION_<CONFIG>
    get_target_property(imported_location ${ARG_TARGET} IMPORTED_LOCATION)
    if(imported_location)
      list(APPEND commands_list COMMAND ${CMAKE_COMMAND} -E copy_if_different
           "${imported_location}" "${LINKTIME_DEST_DIR}/")
    else()
      if(CMAKE_CONFIGURATION_TYPES)
        foreach(config ${CMAKE_CONFIGURATION_TYPES})
          string(TOUPPER ${config} config_upper)
          get_target_property(_imported_loc_cfg ${ARG_TARGET} "IMPORTED_LOCATION_${config_upper}")
          if(_imported_loc_cfg)
            list(APPEND commands_list COMMAND ${CMAKE_COMMAND} -E copy_if_different
                 "${_imported_loc_cfg}" "${LINKTIME_DEST_DIR}/")
          endif()
        endforeach()
        if(NOT commands_list)
          message(WARNING "Imported target ${ARG_TARGET} has no IMPORTED_LOCATION or IMPORTED_LOCATION_<CONFIG>; Skipping staging.")
        endif()
      else()
        message(WARNING "Imported target ${ARG_TARGET} has no IMPORTED_LOCATION property. Skipping staging.")
      endif()
    endif()

  elseif(TGT_TYPE STREQUAL "SHARED_LIBRARY" OR TGT_TYPE STREQUAL "MODULE_LIBRARY")
    if(PYLABHUB_IS_WINDOWS)
      # On Windows, a shared library has a runtime part (.dll) and an import library part (.lib).
      # Stage the runtime to the destination (e.g., 'bin') and the link-time lib to 'lib'.
      list(APPEND commands_list COMMAND ${CMAKE_COMMAND} -E copy_if_different
           "$<TARGET_FILE:${ARG_TARGET}>" "${RUNTIME_DEST_DIR}/")
      if(MSVC)
        list(APPEND commands_list COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "$<TARGET_PDB_FILE:${ARG_TARGET}>" "${RUNTIME_DEST_DIR}/")
      endif()
      # Only Shared Libraries have import libs (.lib); Module Libraries (plugins) generally do not.
      if(TGT_TYPE STREQUAL "SHARED_LIBRARY")
        list(APPEND commands_list COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_LINKER_FILE:${ARG_TARGET}>" "${LINKTIME_DEST_DIR}/")
      endif()
    else() # Non-Windows platforms (Linux, macOS)
      # On non-Windows platforms (Linux, macOS), the shared library file is used for both
      # runtime and linking. Stage it to the specified destination.
      list(APPEND commands_list COMMAND ${CMAKE_COMMAND} -E copy_if_different
           "$<TARGET_FILE:${ARG_TARGET}>" "${RUNTIME_DEST_DIR}/")
      
      # On Linux and macOS, also place a copy/symlink in the 'lib' directory
      # if the runtime destination is not already 'lib'. This ensures the library
      # is discoverable for linking and also at runtime if needed via default search paths.
      if(PYLABHUB_IS_POSIX) # Handle POSIX systems (Linux and macOS)
        if(NOT "${RUNTIME_DEST_DIR}" STREQUAL "${LINKTIME_DEST_DIR}")
          list(APPEND commands_list COMMAND ${CMAKE_COMMAND} -E copy_if_different
               "$<TARGET_FILE:${ARG_TARGET}>" "${LINKTIME_DEST_DIR}/")
        endif()
      endif()
    endif()
  elseif(TGT_TYPE STREQUAL "STATIC_LIBRARY")
    # Static libraries are link-time only. Stage the archive (.a, .lib) to the link-time directory.
    list(APPEND commands_list COMMAND ${CMAKE_COMMAND} -E copy_if_different
         "$<TARGET_FILE:${ARG_TARGET}>" "${LINKTIME_DEST_DIR}/")
  elseif(TGT_TYPE STREQUAL "UNKNOWN_LIBRARY")
    # Handle UNKNOWN IMPORTED libraries, which are typically pre-built binaries.
    # We rely on their IMPORTED_LOCATION property to find the actual file.
    get_target_property(imported_location ${ARG_TARGET} IMPORTED_LOCATION)
    if(imported_location)
        list(APPEND commands_list COMMAND ${CMAKE_COMMAND} -E copy_if_different
             "${imported_location}" "${LINKTIME_DEST_DIR}/")
    else()
        message(WARNING "UNKNOWN_LIBRARY target ${ARG_TARGET} has no IMPORTED_LOCATION property. Skipping staging.")
    endif()
  endif()

  set(${ARG_OUT_COMMANDS} ${commands_list} PARENT_SCOPE)
endfunction()

# --- pylabhub_register_library_for_staging ---
#
# Registers a library target to be staged.
#
# This function simply appends the target name to a global property. A separate,
# centralized process will collect all targets from this property and generate
# the necessary staging commands. This decouples the declaration from the
# implementation.
#
# Usage:
#   pylabhub_register_library_for_staging(TARGET <target_name>)
#
function(pylabhub_register_library_for_staging)
  set(options "")
  set(oneValueArgs "TARGET")
  set(multiValueArgs "")
  cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if(NOT ARG_TARGET)
    message(FATAL_ERROR "pylabhub_register_library_for_staging requires a TARGET argument.")
  endif()

  if(NOT TARGET ${ARG_TARGET})
    message(FATAL_ERROR "pylabhub_register_library_for_staging: Target '${ARG_TARGET}' does not exist.")
  endif()
  
  set_property(GLOBAL APPEND PROPERTY PYLABHUB_LIBRARIES_TO_STAGE ${ARG_TARGET})
endfunction()


# --- pylabhub_register_test_for_staging ---
#
# Registers a test executable for staging. This function sets the executable's
# output directory and appends the target name to a global property. The parent
# CMake scope can then collect all registered test targets and add them as
# dependencies to the main 'stage_tests' target.
#
# This pattern avoids directory scope issues with add_custom_command.
#
# Usage:
#   pylabhub_register_test_for_staging(TARGET <target_name>)
#
function(pylabhub_register_test_for_staging)
  set(options "")
  set(oneValueArgs "TARGET")
  set(multiValueArgs "")
  cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if(NOT ARG_TARGET)
    message(FATAL_ERROR "pylabhub_register_test_for_staging requires a TARGET argument.")
  endif()

  if(NOT TARGET ${ARG_TARGET})
    message(FATAL_ERROR "pylabhub_register_test_for_staging: Target '${ARG_TARGET}' does not exist.")
  endif()

  # Set the output directory for the executable to be inside the staged 'tests' folder.
  # For single-config generators (e.g., Makefiles, Ninja), setting the base
  # RUNTIME_OUTPUT_DIRECTORY is sufficient. For multi-config generators (e.g.,
  # Visual Studio, Xcode), CMake appends a per-configuration subdirectory by
  # default. To override this and ensure a consistent path, we explicitly set the
  # output directory for each configuration.
  if(CMAKE_CONFIGURATION_TYPES)
    # This is a multi-config generator.
    set(output_dir_props "")
    foreach(config ${CMAKE_CONFIGURATION_TYPES})
      string(TOUPPER ${config} config_upper)
      list(APPEND output_dir_props RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${PYLABHUB_STAGING_DIR}/tests")
    endforeach()
    set_target_properties(${ARG_TARGET} PROPERTIES ${output_dir_props})
  else()
    # This is a single-config generator.
    set_target_properties(${ARG_TARGET} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${PYLABHUB_STAGING_DIR}/tests"
    )
  endif()

  # Register this target to a global property so the parent scope can collect it
  set_property(GLOBAL APPEND PROPERTY PYLABHUB_TEST_EXECUTABLES_TO_STAGE ${ARG_TARGET})
endfunction()

# --- pylabhub_attach_library_staging_commands ---
#
# Attaches custom commands to a given target to stage a library's artifacts.
# This function encapsulates the logic for determining which files to copy
# and adding the necessary dependencies and custom commands. It correctly
# handles platform-specific conventions: on Windows, shared libraries (.dll) are
# placed in 'bin/', while on other platforms they are placed in 'lib/' and
# found via RPATH.
#
# Usage:
#   pylabhub_attach_library_staging_commands(
#     TARGET <target_name>      # The library target to stage.
#     ATTACH_TO <target_name>   # The custom target to attach the commands to.
#   )
#
function(pylabhub_attach_library_staging_commands)
  set(options "")
  set(oneValueArgs "TARGET;ATTACH_TO")
  set(multiValueArgs "")
  cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if(NOT ARG_TARGET OR NOT ARG_ATTACH_TO)
    message(FATAL_ERROR "pylabhub_attach_library_staging_commands requires TARGET and ATTACH_TO arguments.")
  endif()
  if(NOT TARGET ${ARG_TARGET})
    message(FATAL_ERROR "pylabhub_attach_library_staging_commands: Target '${ARG_TARGET}' does not exist.")
  endif()
  if(NOT TARGET ${ARG_ATTACH_TO})
    message(FATAL_ERROR "pylabhub_attach_library_staging_commands: Target '${ARG_ATTACH_TO}' does not exist.")
  endif()

  # Determine the correct destination for runtime artifacts based on the platform.
  # On Windows, executables expect DLLs to be in the same directory or in PATH.
  # On Linux/macOS, we use RPATH to point executables in 'bin/' to libraries in 'lib/'.
  if(PYLABHUB_IS_WINDOWS)
    set(runtime_dest "bin")
  else()
    set(runtime_dest "lib")
  endif()

  # Generate the list of staging commands for the given library target.
  pylabhub_get_library_staging_commands(
    TARGET ${ARG_TARGET}
    DESTINATION ${runtime_dest}
    OUT_COMMANDS stage_commands_list
  )

  if(NOT stage_commands_list)
    # This can happen for INTERFACE libraries or other non-artifact targets.
    return()
  endif()
  
  # Ensure the main staging target depends on the library target itself, so the
  # library is built before we try to stage it.
  add_dependencies(${ARG_ATTACH_TO} ${ARG_TARGET})

  # Attach the generated commands to the 'ATTACH_TO' target. These commands
  # will execute after the 'ATTACH_TO' target is built.
  add_custom_command(
    TARGET ${ARG_ATTACH_TO}
    POST_BUILD
    COMMAND_EXPAND_LISTS
    ${stage_commands_list}
    COMMENT "Staging library artifacts for ${ARG_TARGET}"
    VERBATIM
  )
endfunction()

# --- pylabhub_attach_headers_staging_commands ---
#
# Attaches custom commands to a target to stage header directories. This is
# used to simplify the staging logic for third-party libraries.
#
# This version is hardened to correctly expand list arguments and ensure
# commands are deferred until post-build using COMMAND_EXPAND_LISTS.
#

# --- pylabhub_attach_headers_staging_commands ---
#
# Attaches custom commands to a target to stage header directories. This is
# used to simplify the staging logic for third-party libraries.
#
# This version is hardened to correctly expand list arguments and ensure
# commands are deferred until post-build using COMMAND_EXPAND_LISTS.
#
function(pylabhub_attach_headers_staging_commands)
  set(options "")
  set(oneValueArgs "SUBDIR;ATTACH_TO;EXTERNAL_PROJECT_DEPENDENCY")
  set(multiValueArgs "DIRECTORIES;FILES")
  cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if(NOT ARG_ATTACH_TO)
    message(FATAL_ERROR "pylabhub_attach_headers_staging_commands requires ATTACH_TO argument.")
  endif()
  if(NOT TARGET ${ARG_ATTACH_TO})
    message(FATAL_ERROR "pylabhub_attach_headers_staging_commands: Target '${ARG_ATTACH_TO}' does not exist.")
  endif()

  if(NOT ARG_DIRECTORIES AND NOT ARG_FILES)
    return()
  endif()

  if(ARG_SUBDIR)
    set(DEST_DIR "${PYLABHUB_STAGING_DIR}/include/${ARG_SUBDIR}")
  else()
    set(DEST_DIR "${PYLABHUB_STAGING_DIR}/include")
  endif()

  set(custom_cmds "")
  list(APPEND custom_cmds COMMAND ${CMAKE_COMMAND} -E echo "[pylabhub-staging] Preparing staging headers -> ${DEST_DIR}")
  # Ensure destination directory exists (idempotent — does not remove siblings)
  list(APPEND custom_cmds COMMAND ${CMAKE_COMMAND} -E make_directory "${DEST_DIR}")

  # Copy directories (use copy_directory_if_different — non-destructive for siblings)
  foreach(SRC_DIR IN LISTS ARG_DIRECTORIES)
    list(APPEND custom_cmds COMMAND ${CMAKE_COMMAND} -E echo "[pylabhub-staging] Copying directory: ${SRC_DIR} -> ${DEST_DIR}")
    list(APPEND custom_cmds COMMAND ${CMAKE_COMMAND} -E copy_directory_if_different "${SRC_DIR}" "${DEST_DIR}")
  endforeach()

  # Copy individual files (copy_if_different each file)
  foreach(_file IN LISTS ARG_FILES)
    list(APPEND custom_cmds COMMAND ${CMAKE_COMMAND} -E echo "[pylabhub-staging] Copying file: ${_file} -> ${DEST_DIR}/")
    list(APPEND custom_cmds COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_file}" "${DEST_DIR}/")
  endforeach()

  add_custom_command(
    TARGET ${ARG_ATTACH_TO}
    POST_BUILD
    COMMAND_EXPAND_LISTS
    ${custom_cmds}
    COMMENT "Staging headers for ${ARG_ATTACH_TO}"
    VERBATIM
  )

  if(ARG_EXTERNAL_PROJECT_DEPENDENCY)
    add_dependencies(${ARG_ATTACH_TO} ${ARG_EXTERNAL_PROJECT_DEPENDENCY})
  endif()
endfunction()


# --- pylabhub_register_directory_for_staging ---
#
# Registers a full directory structure (e.g., an install prefix) to be staged.
# This is useful for external projects that install multiple subdirectories
# (bin, lib, include, share) into a single prefix. It leverages a template
# script to handle the actual file operations and platform-specific logic
# like Windows DLL staging.
#
# Usage:
#   pylabhub_register_directory_for_staging(
#     SOURCE_DIR <path_to_source_directory> # The root directory to copy from.
#     ATTACH_TO <target_name>               # The custom target to attach the commands to.
#     [SUBDIRS <list_of_subdirs>]           # Optional: List of subdirectories to copy (e.g., "bin;lib;include").
#                                           # If not provided, common subdirs ("bin;lib;include;share") are used.
#   )
#
function(pylabhub_register_directory_for_staging)
  set(options "")
  set(oneValueArgs "SOURCE_DIR;ATTACH_TO")
  set(multiValueArgs "SUBDIRS")
  cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if(NOT ARG_SOURCE_DIR OR NOT ARG_ATTACH_TO)
    message(FATAL_ERROR "pylabhub_register_directory_for_staging requires SOURCE_DIR and ATTACH_TO arguments.")
  endif()
  if(NOT TARGET ${ARG_ATTACH_TO})
    message(FATAL_ERROR "pylabhub_register_directory_for_staging: Target '${ARG_ATTACH_TO}' does not exist.")
  endif()

  set(_subdirs_to_copy "")
  if(ARG_SUBDIRS)
    set(_subdirs_to_copy "${ARG_SUBDIRS}")
  else()
    set(_subdirs_to_copy "bin;lib;include;share") # Default common subdirectories
  endif()

  set(_bulk_stage_template "${CMAKE_SOURCE_DIR}/cmake/BulkStageSingleDirectory.cmake.in")

  # Derive a unique name for the configured script for better readability.
  get_filename_component(_source_dir_name "${ARG_SOURCE_DIR}" NAME)
  string(REPLACE "/" "-" _source_dir_name "${_source_dir_name}") # Replace slashes in name for path safety

  # Generate and attach custom commands for each configuration type.
  if(CMAKE_CONFIGURATION_TYPES)
    foreach(cfg IN LISTS CMAKE_CONFIGURATION_TYPES)
      string(TOUPPER "${cfg}" CFGU)
      set(output_script "${CMAKE_BINARY_DIR}/BulkStage-${ARG_ATTACH_TO}-${cfg}-${_source_dir_name}.cmake")

      # Define variables that will be substituted in the template
      set(SOURCE_DIR "${ARG_SOURCE_DIR}")
      set(STAGING_ROOT_DIR "${PYLABHUB_STAGING_DIR}")
      set(SUBDIRS_TO_COPY "${_subdirs_to_copy}")

      configure_file(
        "${_bulk_stage_template}"
        "${output_script}"
        @ONLY
      )

      add_custom_command(
        TARGET ${ARG_ATTACH_TO}
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -P "${output_script}"
        COMMENT "Bulk staging directory '${ARG_SOURCE_DIR}' (config ${cfg})"
        VERBATIM
        CONFIGURATIONS ${cfg}
      )
    endforeach()
  else() # Single-configuration generator
    set(output_script "${CMAKE_BINARY_DIR}/BulkStage-${ARG_ATTACH_TO}-${_source_dir_name}.cmake")

    # Define variables that will be substituted in the template
    set(SOURCE_DIR "${ARG_SOURCE_DIR}")
    set(STAGING_ROOT_DIR "${PYLABHUB_STAGING_DIR}")
    set(SUBDIRS_TO_COPY "${_subdirs_to_copy}")

    configure_file(
      "${_bulk_stage_template}"
      "${output_script}"
      @ONLY
    )

    add_custom_command(
      TARGET ${ARG_ATTACH_TO}
      POST_BUILD
      COMMAND ${CMAKE_COMMAND} -P "${output_script}"
      COMMENT "Bulk staging directory '${ARG_SOURCE_DIR}'"
      VERBATIM
    )
  endif()

endfunction()


