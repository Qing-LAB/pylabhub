# cmake/Version.cmake
#
# Provides version information for the pylabhub project:
#   - PYLABHUB_VERSION_MAJOR, PYLABHUB_VERSION_MINOR: from project() VERSION
#   - PYLABHUB_VERSION_ROLLING: from git (commit count or 0 if not a git repo)
#   - PYLABHUB_VERSION_STRING: major.minor.rolling (e.g., 0.1.42)
#
# These variables are used to set VERSION and SOVERSION on shared libraries
# (e.g., pylabhub-utils) for proper ABI/soname handling on POSIX and Windows.
#

if(DEFINED PYLABHUB_VERSION_INCLUDED)
  return()
endif()
set(PYLABHUB_VERSION_INCLUDED TRUE)

# Major and minor from project (project VERSION 0.1 yields PROJECT_VERSION_MAJOR=0, PROJECT_VERSION_MINOR=1)
set(PYLABHUB_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(PYLABHUB_VERSION_MINOR ${PROJECT_VERSION_MINOR})

# Rolling number: from git (commit count). Override with -DPYLABHUB_VERSION_ROLLING=N.
# Falls back to 0 if not a git repo or git is unavailable.
set(PYLABHUB_VERSION_ROLLING "0" CACHE STRING "Override rolling version (default: from git rev-list --count HEAD)")
if(PYLABHUB_VERSION_ROLLING STREQUAL "0")
  find_program(_git_executable git)
  if(_git_executable AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(
      COMMAND "${_git_executable}" rev-list --count HEAD
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      OUTPUT_VARIABLE _git_rolling
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE _git_result
    )
    if(_git_result EQUAL 0 AND _git_rolling MATCHES "^[0-9]+$")
      set(PYLABHUB_VERSION_ROLLING "${_git_rolling}" CACHE STRING "Rolling version from git" FORCE)
    endif()
  endif()
endif()

# Full version string for library VERSION property
set(PYLABHUB_VERSION_STRING "${PYLABHUB_VERSION_MAJOR}.${PYLABHUB_VERSION_MINOR}.${PYLABHUB_VERSION_ROLLING}")

message(STATUS "[pylabhub-version] ${PROJECT_NAME} version: ${PYLABHUB_VERSION_STRING} (major=${PYLABHUB_VERSION_MAJOR} minor=${PYLABHUB_VERSION_MINOR} rolling=${PYLABHUB_VERSION_ROLLING})")
