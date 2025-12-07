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
#   2. Uses the `PYLABHUB_STAGING_DIR` variable (defined at the top level) as
#      the destination for all staged artifacts.
#   3. Creates a `stage_third_party_deps` custom target to act as a "hook" for
#      all third-party staging commands.
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
  # This custom target's first responsibility is to create the core directories.
  # This custom target is a hook for all third-party staging commands.
  # It no longer creates directories itself; that is now handled by the
  # top-level 'create_staging_dirs' target, on which this target depends.
  add_custom_target(stage_third_party_deps
    COMMENT "Staging third-party dependencies"
  )

  # Ensure the staging directories are created before this target runs.
  # 'create_staging_dirs' is defined in the top-level CMakeLists.txt.
  add_dependencies(stage_third_party_deps create_staging_dirs)
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