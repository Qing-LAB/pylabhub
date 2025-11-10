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

# setup nlohmann_json target
include(cmake/nlohmann_json.cmake)

# setup fmt target
include(cmake/fmt.cmake)

# setup libzmq target
include(cmake/libzmq.cmake)

# ---------------------------------------------------------------------------
# End of third_party/CMakeLists.txt, set up all the variables that will be
# used by the whole project
# ---------------------------------------------------------------------------