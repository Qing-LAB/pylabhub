# cmake/Versions.cmake
#
# Single source of truth for all pyLabHub version constants.
#
# ┌────────────────────┬───────────────┬─────────────────────────────────────────┐
# │ Variable           │ Example       │ Purpose                                 │
# ├────────────────────┼───────────────┼─────────────────────────────────────────┤
# │ PYLABHUB_RELEASE   │ 0.1.0a0       │ PEP 440 release (PyPI, git tags)        │
# │ PYLABHUB_PYTHON_RT │ 3.14.3+202602 │ Bundled Python runtime (astral-sh pin)  │
# │ PYLABHUB_PYTHON_TAG│ 20260211      │ astral-sh release tag for download URL  │
# └────────────────────┴───────────────┴─────────────────────────────────────────┘
#
# ABI/protocol versions (shm, wire, script_api, facade sizes) live in
# src/utils/core/version_registry.cpp — they change on a different cadence
# and are compiled into the shared library.
#
# Consumers of these variables:
#   - CMakeLists.txt        → project(VERSION ...) and pylabhub_version.h
#   - python.cmake          → Python runtime download URL
#   - pylabhub_version.h.in → C++ macros (PYLABHUB_VERSION_*)
#   - _version_generated.py → Python package (generated at configure time)
#   - pyproject.toml        → reads via scikit-build-core metadata

if(DEFINED PYLABHUB_VERSIONS_INCLUDED)
  return()
endif()
set(PYLABHUB_VERSIONS_INCLUDED TRUE)

# ── Release version (PEP 440) ────────────────────────────────────────────────
# This is the user-visible package version.  Bump here for new releases.
set(PYLABHUB_RELEASE_VERSION "0.1.0a0")

# ── Python runtime (astral-sh/python-build-standalone) ────────────────────────
# Must match the SHA-256 hashes in _runtime.py PYTHON_BUILDS table.
set(PYLABHUB_PYTHON_RUNTIME_VERSION "3.14.3+20260211")
set(PYLABHUB_PYTHON_RELEASE_TAG     "20260211")

# ── Derived: library SOVERSION components ─────────────────────────────────────
# Extracted from the release version for CMake project() and shared lib VERSION.
# Note: project() only accepts numeric major.minor.patch, so we strip pre-release.
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" _release_match "${PYLABHUB_RELEASE_VERSION}")
if(_release_match)
  set(PYLABHUB_VERSION_MAJOR "${CMAKE_MATCH_1}")
  set(PYLABHUB_VERSION_MINOR "${CMAKE_MATCH_2}")
  set(PYLABHUB_VERSION_PATCH "${CMAKE_MATCH_3}")
else()
  message(FATAL_ERROR "Cannot parse PYLABHUB_RELEASE_VERSION='${PYLABHUB_RELEASE_VERSION}' — expected N.N.N[...]")
endif()

# Rolling number: from git commit count.  Override with -DPYLABHUB_VERSION_ROLLING=N.
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

# Full version string for library VERSION property (numeric only: major.minor.rolling)
set(PYLABHUB_VERSION_STRING "${PYLABHUB_VERSION_MAJOR}.${PYLABHUB_VERSION_MINOR}.${PYLABHUB_VERSION_ROLLING}")

message(STATUS "[pylabhub-version] release=${PYLABHUB_RELEASE_VERSION}  lib=${PYLABHUB_VERSION_STRING}  python_rt=${PYLABHUB_PYTHON_RUNTIME_VERSION}")
