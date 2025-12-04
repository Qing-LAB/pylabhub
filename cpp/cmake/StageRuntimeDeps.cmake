# cmake/StageRuntimeDeps.cmake
#
# This script is executed at BUILD TIME by a custom command.
#
# Purpose:
#   To robustly copy the runtime dependencies (e.g., DLLs on Windows) of a
#   given target to a destination directory.
#
# Arguments:
#   -DTARGET_NAME=<name>   : The name of the CMake target.
#   -DDEST_DIR=<path>      : The absolute path to the destination directory.
#   -DRUNTIME_DEPS=<list>  : A semicolon-separated list of runtime dependency files.
#
# This script is designed to be platform-agnostic. On non-Windows platforms,
# the RUNTIME_DEPS list will be empty, and the script will do nothing.
#

if(NOT DEFINED RUNTIME_DEPS OR "${RUNTIME_DEPS}" STREQUAL "")
  # No runtime dependencies to copy, exit successfully.
  return()
endif()

message(STATUS "Staging runtime dependencies for ${TARGET_NAME} to ${DEST_DIR}")
file(COPY ${RUNTIME_DEPS} DESTINATION "${DEST_DIR}")