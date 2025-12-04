# third_party/CMakeLists.txt
#
# Purpose:
#   This file is the central orchestrator for all third-party dependencies.
#   It establishes the global policies and infrastructure, then includes the
#   individual wrapper scripts for each library.
#
# Design:
#   1. Includes `ThirdPartyPolicyAndHelper.cmake` to define global options
#      (e.g., `THIRD_PARTY_INSTALL`) and helper functions.
#   2. Defines the `THIRD_PARTY_STAGING_DIR` variable, which points to the
#      common directory where all dependency artifacts will be copied.
#   3. Creates a global custom target, `stage_third_party_deps`, which serves
#      as the "hook" for all staging operations.
#   4. Includes each library's wrapper script (e.g., `fmt.cmake`), which is
#      responsible for configuring the library and attaching its staging
#      commands to the `stage_third_party_deps` target.
#
cmake_minimum_required(VERSION 3.18)
project(third_party NONE)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ThirdPartyPolicyAndHelper)

# --- Staging Infrastructure (Inherited) ---
# The PYLABHUB_STAGING_DIR is defined in the top-level CMakeLists.txt.
# We create a specific target for staging only third-party dependencies,
# which the global `stage_all` target will depend on.

# Create the global custom target that all package scripts will attach their
# staging commands to. Building this target will collect all required headers
# and libraries into the staging directory.
if(THIRD_PARTY_INSTALL)
  add_custom_target(stage_third_party_deps COMMENT "Staging all third-party dependencies to ${PYLABHUB_STAGING_DIR}")
  add_dependencies(stage_all stage_third_party_deps)
  message(STATUS "[pylabhub-third-party] Staging enabled. Build the 'stage_third_party_deps' target to collect artifacts.")
endif()

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
include(nlohmann_json)

# setup fmt target
include(fmt)

# setup libzmq target
include(libzmq)

# setup XOPToolKit
include(XOPToolKit)

# ---------------------------------------------------------------------------
# As a result of the above includes, the third-party subprojects should now be
# properly configured and added with alias targets named with the prefix
# "pylabhub::third_party::" (e.g. pylabhub::third_party::fmt).
# If THIRD_PARTY_INSTALL is ON, their artifacts can be collected into the
# staging directory by building the `stage_third_party_deps` target.
# End of third_party/CMakeLists.txt
# ---------------------------------------------------------------------------