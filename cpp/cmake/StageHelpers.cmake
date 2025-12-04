# ---------------------------------------------------------------------------
# cmake/StageHelpers.cmake
#
# Purpose:
#   Provides simple, direct helper functions for staging third-party artifacts
#   (headers and libraries) into a common directory. These helpers are designed
#   to be called from the individual package wrapper scripts (e.g., `fmt.cmake`).
#
# This script provides the following functions:
#
# - pylabhub_stage_headers(...)
#   Schedules header files or directories to be copied to the staging area.
#
# - pylabhub_stage_libraries(...)
#   Schedules library artifacts to be copied to the staging area.
#
# These functions rely on two variables being set by the parent scope
# (typically `third_party/CMakeLists.txt`):
#
# - THIRD_PARTY_STAGING_DIR: The root directory for all staged artifacts.
# - stage_third_party_deps: The global custom target to which staging
#   commands are attached.
# ---------------------------------------------------------------------------
#
cmake_minimum_required(VERSION 3.18)

# --- pylabhub_stage_headers ---
#
# Schedules the copying of header files or directories to the staging area.
#
# Usage:
#   pylabhub_stage_headers(
#     [TARGETS <target1> <target2> ...]
#     [DIRECTORIES <dir1> <dir2> ...]
#     SUBDIR <subdir_name>
#   )
#
function(pylabhub_stage_headers)
  cmake_parse_arguments(ARG "" "SUBDIR" "TARGETS;DIRECTORIES" ${ARGN})

  if(NOT DEFINED ARG_SUBDIR)
    message(FATAL_ERROR "pylabhub_stage_headers requires a SUBDIR argument.")
  endif()

  set(DEST_DIR "${THIRD_PARTY_STAGING_DIR}/include/${ARG_SUBDIR}")

  # Stage headers from explicit directories
  foreach(DIR IN LISTS ARG_DIRECTORIES)
    add_custom_command(TARGET stage_third_party_deps POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory "${DIR}" "${DEST_DIR}"
      COMMENT "Staging headers from ${DIR} to ${DEST_DIR}"
      VERBATIM)
  endforeach()

  # Stage headers associated with targets
  foreach(TGT IN LISTS ARG_TARGETS)
    if(TARGET ${TGT})
      add_custom_command(TARGET stage_third_party_deps POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "$<TARGET_PROPERTY:${TGT},INTERFACE_INCLUDE_DIRECTORIES>" "${DEST_DIR}"
        COMMENT "Staging headers for target ${TGT} to ${DEST_DIR}"
        VERBATIM)
    endif()
  endforeach()
endfunction()

# --- pylabhub_stage_libraries ---
#
# Schedules library artifacts (.lib, .a, .so, .dll) to be copied to the
# staging area's `lib` directory.
#
# Usage:
#   pylabhub_stage_libraries(TARGETS <target1> <target2> ...)
#
function(pylabhub_stage_libraries)
  cmake_parse_arguments(ARG "" "" "TARGETS" ${ARGN})

  set(DEST_DIR "${THIRD_PARTY_STAGING_DIR}/lib")
  set(RUNTIME_DEST_DIR "${THIRD_PARTY_STAGING_DIR}/bin") # For Windows DLLs

  foreach(TGT IN LISTS ARG_TARGETS)
    if(TARGET ${TGT})
      # Stage the import/static library (.lib, .a)
      add_custom_command(TARGET stage_third_party_deps POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${TGT}>" "${DEST_DIR}"
        COMMENT "Staging library for ${TGT} to ${DEST_DIR}"
        VERBATIM)

      # On Windows, also stage the runtime DLL to the bin directory
      if(WIN32)
        add_custom_command(TARGET stage_third_party_deps POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:RUNTIME:${TGT}>" "${RUNTIME_DEST_DIR}"
          COMMENT "Staging runtime DLL for ${TGT} to ${RUNTIME_DEST_DIR}"
          VERBATIM)
      endif()
    endif()
  endforeach()
endfunction()
