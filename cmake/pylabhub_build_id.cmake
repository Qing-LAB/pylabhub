# cmake/pylabhub_build_id.cmake
#
# Generates `<build>/src/utils/pylabhub_build_id.h` containing a
# `PYLABHUB_BUILD_ID` macro set to `<git-short-sha>-<CMAKE_BUILD_TYPE>`.
# Also defines `PYLABHUB_HAVE_BUILD_ID` for preprocessor gating.
#
# Consumed by `plh_version_registry.hpp::abi_expected_here()` in strict
# mode (Debug default or `PYLABHUB_STRICT_ABI_CHECK=ON`).  See
# HEP-CORE-0032 and `docs/tech_draft/abi_check_facility_design.md`.

# Resolve a short git SHA for the current HEAD.  Tolerates non-git
# source trees (CI tarball, release tarball) by falling back to "unknown".
find_package(Git QUIET)
set(PYLABHUB_GIT_SHA "unknown")
if(Git_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short=12 HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE PYLABHUB_GIT_SHA_CAPTURED
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _git_rc
    )
    if(_git_rc EQUAL 0 AND PYLABHUB_GIT_SHA_CAPTURED)
        set(PYLABHUB_GIT_SHA "${PYLABHUB_GIT_SHA_CAPTURED}")
    endif()
endif()

# Build type contributes to the ID so Debug vs Release binaries never
# look identical even at the same git SHA.
if(NOT CMAKE_BUILD_TYPE)
    set(_build_type_for_id "NoType")
else()
    set(_build_type_for_id "${CMAKE_BUILD_TYPE}")
endif()

set(PYLABHUB_BUILD_ID_VALUE "${PYLABHUB_GIT_SHA}-${_build_type_for_id}")
message(STATUS "pylabhub build_id: ${PYLABHUB_BUILD_ID_VALUE}")

# Generate the header into the library's private build dir.  Consumers
# of pylabhub-utils that include <plh_version_registry.hpp> will pick
# it up transitively via the same include path as pylabhub_version.h.
configure_file(
    "${CMAKE_SOURCE_DIR}/cmake/pylabhub_build_id.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/pylabhub_build_id.h"
    @ONLY
)
