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
# - pylabhub_stage_headers(...)
#   Schedules third-party header files or directories to be copied to the
#   staging area's `include/` directory.
#
# - pylabhub_stage_executable(...)
#   Schedules an executable target to be copied to a destination in the
#   staging area.
#
# - pylabhub_get_library_staging_commands(...)
#   Generates a list of commands to stage a library target. It correctly
#   handles platform differences (e.g., DLLs vs. .so) and separates runtime
#   from link-time artifacts. This function is intended for use with our own
#   project's targets via `add_custom_target`.
#
# - pylabhub_stage_libraries(...)
#   A convenience wrapper that schedules third-party library artifacts to be
#   copied to the staging area. It attaches commands directly to the global
#   `stage_third_party_deps` target.
#
# These functions rely on variables being set by the top-level `CMakeLists.txt`:
#
# - PYLABHUB_STAGING_DIR: The root directory for all staged artifacts.
#
# And on custom targets defined in the build system:
#
# - stage_third_party_deps: The global custom target to which staging
#   commands for third-party libraries are attached.
# ---------------------------------------------------------------------------
#
cmake_minimum_required(VERSION 3.18)

# --- pylabhub_stage_headers ---
#
# Schedules the copying of header files or directories to the staging area.
# This function is specifically designed for staging third-party headers by
# attaching copy commands to the `stage_third_party_deps` target.
#
# Usage:
#   pylabhub_stage_headers(
#     [TARGETS <target1> ...]       # Stage headers from the INTERFACE_INCLUDE_DIRECTORIES of these targets.
#     [DIRECTORIES <dir1> ...]      # Stage headers from these explicit directories.
#     SUBDIR <subdir_name>          # The subdirectory within `${PYLABHUB_STAGING_DIR}/include` to copy to. (Required)
#     [ATTACH_TO <target_name>]     # The custom target to attach commands to. (Default: stage_third_party_deps)
#   )
#
# Design Notes:
#   This function is primarily for third-party headers, hence the default ATTACH_TO target.
#
#   Staging headers from a target's `INTERFACE_INCLUDE_DIRECTORIES` is complex
#   because the property can contain a generator expression that expands to a
#   list of paths at build time. To handle this robustly, this function uses
#   `file(GENERATE)` to create a small CMake script. This script is executed at
#   build time (via `add_custom_command`), where it can correctly resolve the
#   generator expression, iterate over the resulting list of directories, and
#   copy each one.
#
function(pylabhub_stage_headers)
  set(options "")
  set(oneValueArgs "SUBDIR;ATTACH_TO")
  set(multiValueArgs "TARGETS;DIRECTORIES")
  cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if(NOT ARG_SUBDIR)
    message(FATAL_ERROR "pylabhub_stage_headers requires a non-empty SUBDIR argument.")
  endif()
  if(NOT ARG_ATTACH_TO)
    set(ARG_ATTACH_TO "stage_third_party_deps")
  endif()

  set(DEST_DIR "${PYLABHUB_STAGING_DIR}/include/${ARG_SUBDIR}")

  # Stage headers from explicit directories
  foreach(DIR IN LISTS ARG_DIRECTORIES)
    add_custom_command(TARGET ${ARG_ATTACH_TO} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${DIR}" "${DEST_DIR}"
      COMMENT "Staging headers from ${DIR} to ${DEST_DIR}"
      VERBATIM)
  endforeach()

  # Stage headers associated with targets
  foreach(TGT IN LISTS ARG_TARGETS)
    if(TARGET ${TGT})
      # The INTERFACE_INCLUDE_DIRECTORIES property can be a list. The add_custom_command
      # needs to handle this list, which is expanded at build time via the generator
      # expression. We use file(GENERATE) to create a small helper script that iterates
      # the list and performs the copy for each directory. This is robust against
      # multiple include directories and paths with spaces.
      set(SCRIPT_PATH "${CMAKE_CURRENT_BINARY_DIR}/stage_headers_scripts/stage_${TGT}_headers.cmake")
      file(GENERATE
        OUTPUT ${SCRIPT_PATH}
        CONTENT "
          set(DIRS \"$<TARGET_PROPERTY:${TGT},INTERFACE_INCLUDE_DIRECTORIES>\")
          foreach(DIR IN LISTS DIRS)
            if(EXISTS \"\${DIR}\")
              execute_process(COMMAND ${CMAKE_COMMAND} -E copy_directory \"\${DIR}\" \"${DEST_DIR}\")
            endif()
          endforeach()
        "
      )
      add_custom_command(TARGET ${ARG_ATTACH_TO} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -P ${SCRIPT_PATH}
        COMMENT "Staging headers for target ${TGT} to ${DEST_DIR}"
        VERBATIM)
    endif()
  endforeach()
endfunction()

# --- pylabhub_stage_executable ---
#
# Schedules the copying of an executable target to the staging area.
# This is a simple wrapper around `add_custom_command` that ensures a
# consistent pattern for staging executables.
#
# Usage:
#   pylabhub_stage_executable(
#     TARGET <target_name>          # The executable target to stage.
#     DESTINATION <subdir>          # The subdirectory within `${PYLABHUB_STAGING_DIR}` (e.g., 'bin').
#     ATTACH_TO <target_name>       # The custom target to attach the copy command to.
#   )
#
function(pylabhub_stage_executable)
  set(options "")
  set(oneValueArgs "TARGET;DESTINATION;ATTACH_TO")
  set(multiValueArgs "")
  cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if(NOT ARG_TARGET OR NOT ARG_DESTINATION OR NOT ARG_ATTACH_TO)
    message(FATAL_ERROR "pylabhub_stage_executable requires TARGET, DESTINATION, and ATTACH_TO arguments.")
  endif()
  if(NOT TARGET ${ARG_TARGET})
    message(FATAL_ERROR "pylabhub_stage_executable: Target '${ARG_TARGET}' does not exist.")
  endif()

  set(DEST_DIR "${PYLABHUB_STAGING_DIR}/${ARG_DESTINATION}")

  add_custom_command(TARGET ${ARG_ATTACH_TO} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:${ARG_TARGET}>"
            "${DEST_DIR}/"
    COMMENT "Staging executable for ${ARG_TARGET} to ${DEST_DIR}"
    VERBATIM)
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
  cmake_parse_arguments(ARG "" "TARGET;DESTINATION;OUT_COMMANDS" "" ${ARGN})

  if(NOT ARG_TARGET OR NOT ARG_DESTINATION OR NOT ARG_OUT_COMMANDS)
    message(FATAL_ERROR "pylabhub_get_library_staging_commands requires TARGET, "
                        "DESTINATION, and OUT_COMMANDS arguments.")
  endif()
  if(NOT TARGET ${ARG_TARGET})
    message(FATAL_ERROR "pylabhub_get_library_staging_commands: Target '${ARG_TARGET}' does not exist.")
  endif()

  set(RUNTIME_DEST_DIR "${PYLABHUB_STAGING_DIR}/${ARG_DESTINATION}")
  set(LINKTIME_DEST_DIR "${PYLABHUB_STAGING_DIR}/lib")
  get_target_property(TGT_TYPE ${ARG_TARGET} TYPE)

  set(commands_list "")

  if(TGT_TYPE STREQUAL "SHARED_LIBRARY" OR TGT_TYPE STREQUAL "MODULE_LIBRARY")
    if(PLATFORM_WIN64)
      # On Windows, a shared library has a runtime part (.dll) and an import library part (.lib).
      # Stage the runtime to the destination (e.g., 'bin') and the link-time lib to 'lib'.
      list(APPEND commands_list COMMAND ${CMAKE_COMMAND} -E copy_if_different
           "$<TARGET_FILE:RUNTIME:${ARG_TARGET}>" "${RUNTIME_DEST_DIR}/")
      list(APPEND commands_list COMMAND ${CMAKE_COMMAND} -E copy_if_different
           "$<TARGET_FILE:ARCHIVE:${ARG_TARGET}>" "${LINKTIME_DEST_DIR}/")
    else()
      # On non-Windows platforms (Linux, macOS), the shared library file is used for both
      # runtime and linking. Stage it to the specified destination (e.g., 'bin').
      list(APPEND commands_list COMMAND ${CMAKE_COMMAND} -E copy_if_different
           "$<TARGET_FILE:${ARG_TARGET}>" "${RUNTIME_DEST_DIR}/")
      # On Linux, also place a copy/symlink in the 'lib' directory for consumers to find during linking.
      if(PLATFORM_LINUX)
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
  endif()

  set(${ARG_OUT_COMMANDS} ${commands_list} PARENT_SCOPE)
endfunction()

# --- pylabhub_stage_libraries ---
#
# A convenience wrapper for staging third-party libraries.
#
# This function attaches staging commands for the given library targets directly
# to the `stage_third_party_deps` custom target. It uses the more general
# `pylabhub_get_library_staging_commands` function internally to generate the
# correct, platform-aware copy commands.
#
# Usage:
#   pylabhub_stage_libraries(TARGETS <target1> <target2> ...)
#   pylabhub_stage_libraries(
#     TARGETS <target1> ...         # The library targets to stage.
#     [ATTACH_TO <target_name>]     # The custom target to attach commands to. (Default: stage_third_party_deps)
#   )
#
function(pylabhub_stage_libraries)
  set(options "")
  set(oneValueArgs "ATTACH_TO")
  set(multiValueArgs "TARGETS")
  cmake_parse_arguments(ARG "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  if(NOT ARG_ATTACH_TO)
    set(ARG_ATTACH_TO "stage_third_party_deps")
  endif()

  foreach(TGT IN LISTS ARG_TARGETS)
    if(TARGET ${TGT})
      # Generate the appropriate staging commands for this library target.
      # Runtime components will be directed to 'bin/'.
      # Link-time components will be directed to 'lib/'.
      pylabhub_get_library_staging_commands(
        TARGET ${TGT}
        DESTINATION bin
        OUT_COMMANDS stage_commands
      )
      add_custom_command(TARGET ${ARG_ATTACH_TO} POST_BUILD
        ${stage_commands}
        COMMENT "Staging library artifacts for ${TGT}" VERBATIM)
    endif()
  endforeach()
endfunction()
