# third_party/CMakeLists.txt
# Purpose: manage third-party submodules (fmt, libzmq, XOPSupport, nlohmann_json ...) in a scoped, non-leaking way.
#
# This wrapper sets subproject-specific cache variables (the exact knobs the upstream
# project uses) before calling add_subdirectory(...), and restores any changed
# cache variables after the subproject is handled so state does not leak.
#
# Note: fmt upstream exposes options such as FMT_TEST, FMT_INSTALL, FMT_DOC,
#       FMT_HEADER_ONLY and checks BUILD_SHARED_LIBS for shared/static choice.
#       Those exact names are used below to ensure the wrapper is accurate.
#       (Validated against uploaded fmt/CMakeLists.txt.)
#
cmake_minimum_required(VERSION 3.18)
project(third_party NONE)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(cmake/ThirdPartyPolicyAndHelper.cmake)

# ---------------------------------------------------------------------------
# Begin third_party subproject setups
# Individual third-party subprojects are handled in their own cmake/*.cmake files.
# These files should use the helper macros snapshot_cache_var / restore_cache_var
# to avoid leaking cache variable changes outside their scope.
# The scripts should respect the variables and options defined in
# cmake/ThirdPartyPolicyAndHelper.cmake.
# Both building and installation should be properly handled according to the
# wrapper-level intent variables.
# ---------------------------------------------------------------------------

# setup nlohmann_json target
include(cmake/nlohmann_json.cmake)

# setup fmt target
include(cmake/fmt.cmake)

# setup libzmq target
include(cmake/libzmq.cmake)

# setup XOPToolKit
include(cmake/XOPToolKit.cmake)

# ---------------------------------------------------------------------------
# As a result of the above includes, the third-party subprojects should now be
# properly configured and added with alias targets named with the prefix
# "pylabhub::" (e.g. pylabhub::fmt, pylabhub::zmq, pylabhub::nlohmann_json).
# Third party header files and libraries will be installed under corresponding
# directories in the main project install tree if THIRD_PARTY_INSTALL is ON.
# End of third_party/CMakeLists.txt
# ---------------------------------------------------------------------------